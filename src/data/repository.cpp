#include "aps/data/repository.hpp"

// ---------------------------------------------------------------------------
// repository.cpp -- Repository 实现 + Problem 与 JSON 的导入导出
//
// 内容概要:
//   - initDemo/setLatest/logEvent: 状态写入
//   - problemToJson: Problem -> JSON (订单/设备/工艺/换型族/统计)
//   - problemFromJson: JSON -> Problem (导入时按 family 重建换型矩阵)
//   - problemJson/summaryJson/solutionJson/eventsJson: 给前端序列化
// ---------------------------------------------------------------------------

#include <algorithm>
#include <map>

#include "aps/data/demo_data.hpp"
#include "aps/kpi/metrics.hpp"

namespace aps {

// 用演示工厂重建主数据, 并清空旧结果与事件、记录 init 事件。
void Repository::initDemo(int orderCount, int horizonDays, std::uint64_t seed, bool twoShifts) {
  DemoConfig cfg;
  cfg.orderCount = orderCount > 0 ? orderCount : 80;
  cfg.horizonDays = horizonDays > 0 ? horizonDays : 14;
  cfg.seed = seed;
  cfg.twoShifts = twoShifts;
  initDemo(cfg);
}

// 真正的重建逻辑: 换主数据 + 清空旧结果/事件 + 记 init 事件。
// 两个 initDemo 重载都汇到这里, 保证"重置后状态干净"只有一份实现。
void Repository::initDemo(const DemoConfig& cfg) {
  DemoConfig safe = cfg;
  if (safe.orderCount <= 0) safe.orderCount = 80;
  if (safe.horizonDays <= 0) safe.horizonDays = 14;
  problem_ = makeDemoFactory(safe);
  latest_ = Solution{};
  events_.clear();
  lastAlgo_.clear();
  lastSolveAt_ = 0;
  logEvent("init", "载入演示算例: " + std::to_string(problem_.numOrders()) + " 订单 / " +
                       std::to_string(problem_.numOps()) + " 工序 / " +
                       std::to_string(problem_.numMachines()) + " 设备");
}

// 写回排产结果, 记录算法名与写回时刻 (服务端时间)。
void Repository::setLatest(Solution s, const std::string& algo) {
  latest_ = std::move(s);
  lastAlgo_ = algo;
  lastSolveAt_ = timeutil::now();
}

// 追加一条事件; 超过 200 条时删除最旧 100 条, 控制内存占用。
void Repository::logEvent(const std::string& type, const std::string& detail) {
  Event e;
  e.at = timeutil::now();
  e.type = type;
  e.detail = detail;
  events_.push_back(e);
  if (events_.size() > 200) events_.erase(events_.begin(), events_.begin() + 100);
}

// ---------------------------------------------------------------------------
// Problem -> JSON
// ---------------------------------------------------------------------------
// 将 Problem 全部主数据序列化为 JSON: 时间窗/订单/设备/工艺(含换型选项)/
// 换型族/统计。供 /api/state 与导出使用。
json::Value problemToJson(const Problem& p) {
  json::Value v = json::Value::object();
  v.set("name", p.name);
  v.set("horizonStart", p.horizonStart);
  v.set("horizonEnd", p.horizonEnd);
  v.set("horizonStartText", timeutil::format(p.horizonStart));
  v.set("horizonEndText", timeutil::format(p.horizonEnd));

  {
    json::Value arr = json::Value::array();
    for (const auto& o : p.orders) {
      json::Value j = json::Value::object();
      j.set("code", o.code);
      j.set("product", o.product);
      j.set("qty", o.qty);
      j.set("release", o.release);
      j.set("releaseText", timeutil::format(o.release));
      j.set("due", o.due);
      j.set("dueText", timeutil::format(o.due));
      j.set("priority", o.priority);
      j.set("urgent", o.urgent);
      arr.push(j);
    }
    v.set("orders", arr);
  }
  {
    json::Value arr = json::Value::array();
    for (const auto& m : p.machines) {
      json::Value j = json::Value::object();
      j.set("code", m.code);
      j.set("group", m.group);
      j.set("name", m.name);
      j.set("down", m.down);
      arr.push(j);
    }
    v.set("machines", arr);
  }
  {
    json::Value arr = json::Value::array();
    for (const auto& r : p.routings) {
      json::Value jr = json::Value::object();
      jr.set("product", r.product);
      json::Value ops = json::Value::array();
      for (const auto& d : r.ops) {
        json::Value jo = json::Value::object();
        jo.set("code", d.code);
        jo.set("name", d.name);
        jo.set("family", d.family);
        json::Value opts = json::Value::array();
        for (const auto& mo : d.options) {
          json::Value jm = json::Value::object();
          jm.set("machine", p.machines[static_cast<size_t>(mo.machine)].code);
          jm.set("unitSeconds", mo.unitSeconds);
          opts.push(jm);
        }
        jo.set("options", opts);
        ops.push(jo);
      }
      jr.set("ops", ops);
      arr.push(jr);
    }
    v.set("routings", arr);
  }
  {
    json::Value fam = json::Value::array();
    for (const auto& f : p.setup.families()) fam.push(json::Value(f));
    v.set("setupFamilies", fam);
  }
  {
    json::Value st = json::Value::object();
    st.set("orders", p.numOrders());
    st.set("ops", p.numOps());
    st.set("machines", p.numMachines());
    st.set("totalQty", [&] {
      long long s = 0;
      for (const auto& o : p.orders) s += o.qty;
      return s;
    }());
    v.set("stats", st);
  }
  return v;
}

// 从 JSON 反序列化 Problem: 校验为对象后依次读设备/工艺/订单, 建立 code->下标
// 映射; 导入时按 family 名重建换型矩阵 (跨族默认 30 分钟, 同族 0); 空主数据
// 报错。成功后 rebuild + buildCalendars。
//
// @param v    输入 JSON 对象
// @param p    输出 Problem (仅成功时赋值)
// @param err  失败原因 (输出)
// @return     成功 true
bool problemFromJson(const json::Value& v, Problem& p, std::string& err) {
  if (!v.isObject()) {
    err = "problem 必须是对象";
    return false;
  }
  Problem np;
  np.name = v["name"].asString("imported");
  np.horizonStart = v["horizonStart"].asInt(timeutil::dayStart(timeutil::now()));
  np.horizonEnd = v["horizonEnd"].asInt(np.horizonStart + 14 * 86400);

  np.calendar.workdays = {1, 2, 3, 4, 5, 6};
  np.calendar.shifts = {{8, 12}, {13, 17}};

  // 设备
  std::map<std::string, int> machineIdx;
  for (const auto& jm : v["machines"].items()) {
    Machine m;
    m.code = jm["code"].asString();
    m.group = jm["group"].asString();
    m.name = jm["name"].asString(m.code);
    m.down = jm["down"].asBool(false);
    if (m.code.empty()) continue;
    machineIdx[m.code] = static_cast<int>(np.machines.size());
    np.machines.push_back(m);
  }
  // 工艺
  std::map<std::string, int> routingIdx;
  for (const auto& jr : v["routings"].items()) {
    Routing r;
    r.product = jr["product"].asString();
    for (const auto& jo : jr["ops"].items()) {
      OperationDef d;
      d.code = jo["code"].asString();
      d.name = jo["name"].asString(d.code);
      d.family = jo["family"].asString();
      for (const auto& jopt : jo["options"].items()) {
        auto it = machineIdx.find(jopt["machine"].asString());
        if (it == machineIdx.end()) continue;
        d.options.push_back({it->second, jopt["unitSeconds"].asDouble(60.0)});
      }
      if (!d.options.empty()) r.ops.push_back(d);
    }
    if (!r.product.empty() && !r.ops.empty()) {
      routingIdx[r.product] = static_cast<int>(np.routings.size());
      np.routings.push_back(r);
    }
  }
  // 订单
  for (const auto& jo : v["orders"].items()) {
    Order o;
    o.code = jo["code"].asString();
    o.product = jo["product"].asString();
    o.qty = jo["qty"].asInt(1);
    o.release = jo["release"].asInt(np.horizonStart);
    o.due = jo["due"].asInt(np.horizonEnd);
    o.priority = static_cast<int>(jo["priority"].asInt(3));
    o.urgent = jo["urgent"].asBool(false);
    if (o.code.empty()) continue;
    np.orders.push_back(o);
  }

  // 换型: 导入时按 family 名重建, 跨族用 30 分钟默认 (原矩阵不导出以简化格式)
  std::vector<std::string> fams;
  for (const auto& f : v["setupFamilies"].items()) fams.push_back(f.asString());
  if (fams.empty()) {
    for (const auto& r : np.routings)
      for (const auto& d : r.ops)
        if (!d.family.empty() &&
            std::find(fams.begin(), fams.end(), d.family) == fams.end())
          fams.push_back(d.family);
  }
  {
    std::vector<std::vector<Seconds>> t(fams.size(), std::vector<Seconds>(fams.size(), 1800));
    for (size_t i = 0; i < fams.size(); ++i) t[i][i] = 0;
    np.setup.init(fams, t);
  }

  if (np.orders.empty() || np.machines.empty() || np.routings.empty()) {
    err = "订单/设备/工艺路线 不能为空";
    return false;
  }
  np.rebuild();
  np.buildCalendars();
  p = std::move(np);
  return true;
}

// ---------------------------------------------------------------------------
// Solution -> JSON
// ---------------------------------------------------------------------------
json::Value Repository::problemJson() const { return problemToJson(problem_); }

// 服务端摘要: 当前时间/名称/数量统计/是否已求解/最近算法与写回时刻。
json::Value Repository::summaryJson() const {
  json::Value j = json::Value::object();
  j.set("serverTime", timeutil::now());
  j.set("problemName", problem_.name);
  j.set("orders", problem_.numOrders());
  j.set("ops", problem_.numOps());
  j.set("machines", problem_.numMachines());
  j.set("solved", !latest_.assign.empty());
  j.set("lastAlgo", lastAlgo_);
  j.set("lastSolveAt", lastSolveAt_);
  return j;
}

// 排产结果序列化: 算法/可行性/KPI/设备负荷/订单汇总, 可选展开工序级 tasks。
// 未求解时仅返回 solved=false。
//
// @param includeTasks  是否包含每道工序明细 (tasks)
// @return              结果 JSON
json::Value Repository::solutionJson(bool includeTasks) const {
  json::Value j = json::Value::object();
  j.set("solved", !latest_.assign.empty());
  if (latest_.assign.empty()) return j;

  j.set("algo", latest_.solver);
  j.set("feasible", latest_.feasible);
  j.set("truncated", latest_.truncated);
  j.set("infeasibleReason", latest_.infeasibleReason);

  const KPI& k = latest_.kpi;
  {
    json::Value kj = json::Value::object();
    kj.set("otd", k.otd);
    kj.set("ordersTotal", k.ordersTotal);
    kj.set("ordersOnTime", k.ordersOnTime);
    kj.set("ordersLate", k.ordersLate);
    kj.set("totalTardinessHours", static_cast<double>(k.totalTardiness) / 3600.0);
    kj.set("maxTardinessHours", static_cast<double>(k.maxTardiness) / 3600.0);
    kj.set("makespanHours", static_cast<double>(k.makespanSpan) / 3600.0);
    kj.set("setupCount", k.setupCount);
    kj.set("setupMinutes", static_cast<double>(k.setupSec) / 60.0);
    kj.set("avgUtilization", k.avgUtilization);
    kj.set("peakWip", k.peakWip);
    kj.set("objective", k.objective);
    kj.set("solveMs", k.solveMs);
    kj.set("evaluations", k.evaluations);
    j.set("kpi", kj);
  }
  {
    json::Value arr = json::Value::array();
    for (size_t i = 0; i < latest_.kpi.machineUtilization.size(); ++i) {
      json::Value m = json::Value::object();
      m.set("code", problem_.machines[i].code);
      m.set("utilization", latest_.kpi.machineUtilization[i]);
      arr.push(m);
    }
    j.set("machineUtilization", arr);
  }

  // 订单级汇总 (完成时间 / 是否延期)
  std::vector<Seconds> completion(static_cast<size_t>(problem_.numOrders()), -1);
  for (const auto& a : latest_.assign) {
    if (a.op < 0 || a.op >= problem_.numOps()) continue;
    int o = problem_.ops[static_cast<size_t>(a.op)].order;
    completion[static_cast<size_t>(o)] = std::max(completion[static_cast<size_t>(o)], a.end);
  }
  {
    json::Value arr = json::Value::array();
    for (int o = 0; o < problem_.numOrders(); ++o) {
      const Order& ord = problem_.orders[static_cast<size_t>(o)];
      Seconds comp = completion[static_cast<size_t>(o)];
      if (comp < 0) continue;
      json::Value oj = json::Value::object();
      oj.set("code", ord.code);
      oj.set("product", ord.product);
      oj.set("qty", ord.qty);
      oj.set("priority", ord.priority);
      oj.set("urgent", ord.urgent);
      oj.set("due", ord.due);
      oj.set("completion", comp);
      oj.set("tardinessHours", comp > ord.due ? static_cast<double>(comp - ord.due) / 3600.0 : 0.0);
      oj.set("onTime", comp <= ord.due);
      arr.push(oj);
    }
    j.set("orders", arr);
  }

  if (includeTasks) {
    json::Value arr = json::Value::array();
    arr.items().reserve(latest_.assign.size());
    for (const auto& a : latest_.assign) {
      if (a.op < 0) continue;
      const OpInst& op = problem_.ops[static_cast<size_t>(a.op)];
      const Order& ord = problem_.orders[static_cast<size_t>(op.order)];
      json::Value t = json::Value::object();
      t.set("op", a.op);
      t.set("order", ord.code);
      t.set("product", ord.product);
      t.set("seq", op.seq);
      t.set("name", op.name);
      t.set("family", op.family);
      t.set("machine", problem_.machines[static_cast<size_t>(a.machine)].code);
      t.set("machineIdx", a.machine);
      t.set("start", a.start);
      t.set("end", a.end);
      t.set("setup", a.setup);
      t.set("duration", a.duration());
      t.set("qty", op.qty);
      t.set("priority", ord.priority);
      t.set("urgent", ord.urgent);
      t.set("frozen", a.frozen);
      arr.push(t);
    }
    j.set("tasks", arr);
  }
  return j;
}

// 事件流序列化: 每条含 at/atText/type/detail。
json::Value Repository::eventsJson() const {
  json::Value arr = json::Value::array();
  for (const auto& e : events_) {
    json::Value j = json::Value::object();
    j.set("at", e.at);
    j.set("atText", timeutil::format(e.at, true));
    j.set("type", e.type);
    j.set("detail", e.detail);
    arr.push(j);
  }
  return arr;
}

}  // namespace aps

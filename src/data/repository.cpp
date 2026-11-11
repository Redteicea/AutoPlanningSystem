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

/// @brief 用参数重建演示主数据 (便捷重载, 内部构造 DemoConfig 后转调下面的实现)。
///
/// @param orderCount  订单数; <=0 时回退 80
/// @param horizonDays 排产窗口天数; <=0 时回退 14
/// @param seed        随机种子, 决定订单的数量/交期/急单分布
/// @param twoShifts   true=加夜班 (8-12 / 13-17 / 18-22 三班), 作为自定义日历的替代
void Repository::initDemo(int orderCount, int horizonDays, std::uint64_t seed, bool twoShifts) {
  DemoConfig cfg;
  cfg.orderCount = orderCount > 0 ? orderCount : 80;
  cfg.horizonDays = horizonDays > 0 ? horizonDays : 14;
  cfg.seed = seed;
  cfg.twoShifts = twoShifts;
  initDemo(cfg);
}

/// @brief 用 DemoConfig 重建演示主数据（真正的重置实现）。
///
/// 规整参数（orderCount/horizonDays <=0 回退 80/14）后调用 makeDemoFactory 换主数据，
/// 并清空 latest_/events_/lastAlgo_/lastSolveAt_，最后记一条 init 事件。两个 initDemo
/// 重载都汇聚于此，保证"重置后状态干净"只有一份实现。
///
/// @param cfg  演示算例生成参数（订单数/窗口天数/种子/夜班/自定义日历）
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

/// @brief 写回最新排产结果, 同时记录产出算法与写回时刻。
///
/// 时刻取服务端当前时间 (timeutil::now()), 前端据此显示"结果有多新"。
///
/// @param s    排产结果, 移动语义接管 (调用后实参被置空)
/// @param algo 产出该结果的算法标识 ("rule:EDD" / "ga" 等), 只用于展示
void Repository::setLatest(Solution s, const std::string& algo) {
  latest_ = std::move(s);
  lastAlgo_ = algo;
  lastSolveAt_ = timeutil::now();
}

/// @brief 向事件流追加一条事件；超过 200 条时删除最旧 100 条以控内存。
///
/// 事件 at 取服务端当前时间。events_ 超过 200 条后，从头删除 100 条。
///
/// @param type    事件类型（如 "init"、"solve"）
/// @param detail  事件详情文本
void Repository::logEvent(const std::string& type, const std::string& detail) {
  Event e;
  e.at = timeutil::now();
  e.type = type;
  e.detail = detail;
  events_.push_back(e);
  if (events_.size() > 200) events_.erase(events_.begin(), events_.begin() + 100);
}

/// @brief 把 Problem 的全部主数据序列化成 JSON (供 /api/state 与算例导出使用)。
///
/// 包含: 时间窗 (含可读文本)、订单 (含产品/数量/交期/优先级/急单标记)、设备 (含状态)、
/// 工艺路线 (含每道工序的候选设备与单件工时)、换型矩阵、以及规模统计。
///
/// @param p 待序列化的算例
/// @return  JSON 对象; 结构见 web/app.js 里读取的字段
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

/// @brief 从 JSON 反序列化 Problem（导入时按 family 重建换型矩阵）。
///
/// 校验为对象后依次读设备/工艺/订单，建立 code->下标映射；导入时按 family 名重建换型
/// 矩阵（跨族默认 30 分钟、同族 0），原矩阵不导出以简化格式。订单/设备/工艺任一为空则
/// 报错。成功后 rebuild + buildCalendars。
///
/// @param v    输入 JSON 对象（须含 machines/routings/orders 等）
/// @param p    输出 Problem（仅成功时赋值）
/// @param err  失败原因（输出），如字段缺失或主数据为空
/// @return     true=导入成功并写回 p；false=失败（err 含原因，p 不变）
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

// ---- Problem / Solution 的 JSON 视图 ----

/// @brief 主数据的 JSON 视图 (转调 problemToJson, 让调用方不必拿锁去碰 problem_)。
///
/// @return Problem 的完整 JSON 表示
json::Value Repository::problemJson() const { return problemToJson(problem_); }

/// @brief 生成服务端摘要 JSON（状态概览，不含排产明细）。
///
/// 含当前服务端时间、问题名、订单/工序/设备数、是否已求解（latest_.assign 非空）、
/// 最近算法名与写回时刻。
///
/// @return  含 serverTime/problemName/orders/ops/machines/solved/lastAlgo/lastSolveAt 的对象
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

/// @brief 排产结果的 JSON 视图 (算法 / 可行性 / KPI / 设备负荷 / 订单汇总, 可选工序明细)。
///
/// 未求解 (assign 为空) 时只返回 {"solved":false}, 前端据此显示"还没排产"。
///
/// @param includeTasks true=附带工序级 tasks 数组 (甘特图用, 80 单约 467 条, 不传则省流量);
///                     false=只返回汇总与 KPI
/// @return             结果 JSON
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

/// @brief 事件流的 JSON 数组视图 (最近 200 条, 供前端"事件流水"面板展示)。
///
/// @return JSON 数组, 每个元素含 at (Unix 秒) / atText (可读时间) / type / detail
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

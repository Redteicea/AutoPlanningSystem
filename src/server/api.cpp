#include "aps/server/api.hpp"

// ---------------------------------------------------------------------------
// api.cpp -- REST 端点实现
//
// 内容概要:
//   - 匿名工具: msSince(计时) / errJson(错误响应) / parseBody / parseTime
//   - registerRoutes: 把 12 个端点绑定到 HttpServer
//   - 每个端点: 解析入参 -> 加锁操作 Repository 副本/主数据 -> 序列化 JSON
//   - solve/insert/breakdown 会调用引擎, 写回前用 generation 比对防止竞态
// ---------------------------------------------------------------------------

#include <algorithm>
#include <chrono>

#include "aps/data/demo_data.hpp"
#include "aps/engine/ga_scheduler.hpp"
#include "aps/engine/rule_scheduler.hpp"
#include "aps/engine/validator.hpp"
#include "aps/reschedule/rescheduler.hpp"

namespace aps {
namespace {

using Clock = std::chrono::steady_clock;

// 计算从 t0 到当前经过的毫秒数 (用于各端点计时)。
double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 构造统一错误响应: {"ok":false,"error":msg}, 默认状态码 400。
HttpResponse errJson(const std::string& msg, int code = 400) {
  json::Value v = json::Value::object();
  v.set("ok", false);
  v.set("error", msg);
  return HttpResponse::json(v.dump(), code);
}

// 解析请求体 JSON; 空体返回空对象, 解析失败通过 ok=false 标记。
json::Value parseBody(const HttpRequest& req, bool& ok) {
  ok = true;
  if (req.body.empty()) return json::Value::object();
  try {
    return json::parse(req.body);
  } catch (...) {
    ok = false;
    return json::Value::object();
  }
}

// 从 JSON 值取时间: 数字按秒直接取, 字符串走 timeutil::parse; 失败/空回退 def。
Seconds parseTime(const json::Value& v, Seconds def) {
  if (v.isNumber()) return v.asInt(def);
  std::string s = v.asString();
  if (s.empty()) return def;
  Seconds t = timeutil::parse(s);
  return t >= 0 ? t : def;
}

}  // namespace

// 把全部 12 个 REST 端点以 lambda 转发到对应成员函数, 注册到 srv。
void Api::registerRoutes(HttpServer& srv) {
  srv.route("GET", "/api/health", [this](const HttpRequest& r) { return health(r); });
  srv.route("GET", "/api/state", [this](const HttpRequest& r) { return state(r); });
  srv.route("GET", "/api/schedule", [this](const HttpRequest& r) { return getSchedule(r); });
  srv.route("POST", "/api/schedule", [this](const HttpRequest& r) { return solve(r); });
  srv.route("POST", "/api/demo/reset", [this](const HttpRequest& r) { return resetDemo(r); });
  srv.route("GET", "/api/config", [this](const HttpRequest& r) { return getConfig(r); });
  srv.route("POST", "/api/config", [this](const HttpRequest& r) { return saveConfig(r); });
  srv.route("POST", "/api/orders", [this](const HttpRequest& r) { return addOrder(r); });
  srv.route("POST", "/api/reschedule/insert", [this](const HttpRequest& r) { return insertOrder(r); });
  srv.route("POST", "/api/reschedule/breakdown", [this](const HttpRequest& r) { return breakdown(r); });
  srv.route("POST", "/api/machines/:code/restore", [this](const HttpRequest& r) { return restore(r); });
  srv.route("GET", "/api/events", [this](const HttpRequest& r) { return events(r); });
  srv.route("POST", "/api/benchmark", [this](const HttpRequest& r) { return benchmark(r); });
  srv.route("GET", "/api/export", [this](const HttpRequest& r) { return exportProblem(r); });
}

// 健康检查: 返回 ok/name/version/serverTime 的 JSON。
HttpResponse Api::health(const HttpRequest&) {
  json::Value v = json::Value::object();
  v.set("ok", true);
  v.set("name", "APS 自动排产服务");
  v.set("version", "1.0.0");
  v.set("serverTime", timeutil::now());
  return HttpResponse::json(v.dump());
}

// 返回主数据快照: summary(统计) + problem(订单/设备/工艺) + 是否已求解。
HttpResponse Api::state(const HttpRequest&) {
  std::lock_guard<std::mutex> lk(repo_.mutex());
  json::Value v = json::Value::object();
  v.set("ok", true);
  v.set("summary", repo_.summaryJson());
  v.set("problem", repo_.problemJson());
  v.set("hasResult", !repo_.latest().assign.empty());
  return HttpResponse::json(v.dump());
}

// 返回最近一次排产结果 (含工序级 tasks), 未求解时 solved=false。
HttpResponse Api::getSchedule(const HttpRequest&) {
  std::lock_guard<std::mutex> lk(repo_.mutex());
  json::Value v = repo_.solutionJson(true);
  v.set("ok", true);
  return HttpResponse::json(v.dump());
}

// 执行排产: 解析 algo/rule/timeLimitMs/seed -> 拷贝主数据副本 -> 调调度器
// (auto 模式对比 rule 与 ga 择优) -> 独立校验可行性 -> 比对 generation 防止
// 并发覆盖 -> 写回结果并记录事件。主数据竞态时返回 409。
//
// @param req  含 {algo, rule, timeLimitMs, seed, threads} 的 POST 体
HttpResponse Api::solve(const HttpRequest& req) {
  bool bodyOk = true;
  json::Value body = parseBody(req, bodyOk);
  if (!bodyOk) return errJson("请求体不是合法 JSON");

  const std::string algo = body["algo"].asString("ga");
  SolveOptions opt;
  opt.rule = body["rule"].asString("AUTO");
  opt.timeLimitMs = std::min(60000.0, std::max(50.0, body["timeLimitMs"].asDouble(2000.0)));
  opt.seed = static_cast<std::uint64_t>(body["seed"].asInt(20261105));
  opt.threads = static_cast<int>(body["threads"].asInt(0));
  opt.detailedKpi = true;

  Problem copy;
  std::uint64_t gen = 0;
  {
    std::lock_guard<std::mutex> lk(repo_.mutex());
    copy = repo_.problem();
    gen = repo_.generation();
  }
  if (copy.numOps() == 0) return errJson("当前算例没有可排工序");

  Solution sol;
  std::string usedAlgo = algo;
  try {
    if (algo == "rule") {
      RuleScheduler rs;
      sol = rs.solve(copy, opt);
      usedAlgo = sol.solver;
    } else if (algo == "auto") {
      RuleScheduler rs;
      Solution s1 = rs.solve(copy, opt);
      GaScheduler gs;
      SolveOptions opt2 = opt;
      opt2.detailedKpi = true;
      Solution s2 = gs.solve(copy, opt2);
      if (s2.kpi.objective <= s1.kpi.objective) {
        sol = std::move(s2);
        usedAlgo = "ga";
      } else {
        sol = std::move(s1);
        usedAlgo = s1.solver;
      }
    } else {
      GaScheduler gs;
      sol = gs.solve(copy, opt);
      usedAlgo = "ga";
    }
  } catch (const std::exception& ex) {
    return errJson(std::string("求解失败: ") + ex.what(), 500);
  }

  // 可行性独立校验 (只信校验器)
  ValidationResult vr = validate(copy, sol);
  sol.feasible = vr.ok && !sol.truncated;

  std::lock_guard<std::mutex> lk(repo_.mutex());
  if (repo_.generation() != gen) return errJson("主数据已变更, 请重新排产", 409);

  repo_.setLatest(std::move(sol), usedAlgo);
  repo_.logEvent("solve", "算法 " + usedAlgo + ", 工序 " + std::to_string(copy.numOps()) + ", 耗时 " +
                              std::to_string(static_cast<long long>(repo_.latest().kpi.solveMs)) +
                              "ms");

  json::Value v = repo_.solutionJson(true);
  v.set("ok", true);
  v.set("validation", json::Value(vr.ok ? "PASS" : vr.summary()));
  return HttpResponse::json(v.dump());
}

// 按 orders/days/seed/twoShifts 重建演示算例 (加锁后 initDemo + bump)。
// 注入通用配置 (main 里启动时 loadAppConfig 得到的) 与落盘路径。
void Api::setAppConfig(const AppConfig& cfg, const std::string& path) {
  appCfg_ = cfg;
  configPath_ = path;
}

// 读通用配置: 直接返回当前生效配置的 JSON (前端"日历配置"弹窗的数据源)。
HttpResponse Api::getConfig(const HttpRequest&) {
  return HttpResponse::json(appConfigToJson(appCfg_).dump(2));
}

// 改通用配置: 解析 -> 校验 -> 落盘 -> 按新配置重建演示算例。
// 先落盘再动内存: 万一写文件失败, 服务状态不被改掉, 前端拿到明确报错。
HttpResponse Api::saveConfig(const HttpRequest& req) {
  bool bodyOk = true;
  json::Value body = parseBody(req, bodyOk);
  if (!bodyOk) return errJson("请求体不是合法 JSON");

  AppConfig next = appCfg_;  // 部分更新: 只覆盖 body 里出现的字段
  std::string err;
  if (!appConfigFromJson(body, next, err)) return errJson("配置非法: " + err);

  std::string saveErr;
  if (!saveAppConfig(configPath_, next, saveErr)) return errJson("配置保存失败: " + saveErr);

  // 新日历要重新编译设备可用区间, 所以整表重建 (清空旧排产结果)
  std::lock_guard<std::mutex> lk(repo_.mutex());
  appCfg_ = next;
  repo_.initDemo(toDemoConfig(next));
  repo_.bump();
  repo_.logEvent("init", "通用配置已更新并写入 " + configPath_);
  json::Value v = json::Value::object();
  v.set("ok", true);
  v.set("config", appConfigToJson(appCfg_));
  v.set("summary", repo_.summaryJson());
  return HttpResponse::json(v.dump());
}

// 重建演示算例: 未传的字段取通用配置里的默认值; 日历始终来自配置
// (旧接口的 twoShifts=true 仍兼容, 等价于在配置日历上叠一个 18-22 夜班)。
HttpResponse Api::resetDemo(const HttpRequest& req) {
  bool bodyOk = true;
  json::Value body = parseBody(req, bodyOk);
  if (!bodyOk) return errJson("请求体不是合法 JSON");

  DemoConfig cfg = toDemoConfig(appCfg_);
  cfg.orderCount = static_cast<int>(body["orders"].asInt(appCfg_.orderCount));
  cfg.horizonDays = static_cast<int>(body["days"].asInt(appCfg_.horizonDays));
  cfg.seed = static_cast<std::uint64_t>(body["seed"].asInt(static_cast<long long>(appCfg_.seed)));
  if (body["twoShifts"].asBool(false))
    cfg.calendar.shifts = {{8, 12}, {13, 17}, {18, 22}};

  std::lock_guard<std::mutex> lk(repo_.mutex());
  repo_.initDemo(cfg);
  repo_.bump();
  json::Value v = json::Value::object();
  v.set("ok", true);
  v.set("summary", repo_.summaryJson());
  return HttpResponse::json(v.dump());
}

// 追加订单: 解析字段 -> 校验产品工艺存在与订单号唯一 -> 重建索引与日历 -> bump。
HttpResponse Api::addOrder(const HttpRequest& req) {
  bool bodyOk = true;
  json::Value body = parseBody(req, bodyOk);
  if (!bodyOk) return errJson("请求体不是合法 JSON");

  std::lock_guard<std::mutex> lk(repo_.mutex());
  Problem& p = repo_.problem();
  Order o;
  o.code = body["code"].asString();
  if (o.code.empty()) o.code = "SO-" + std::to_string(1001 + p.numOrders());
  o.product = body["product"].asString();
  o.qty = body["qty"].asInt(2000);
  o.release = parseTime(body["release"], timeutil::now());
  o.due = parseTime(body["due"], timeutil::now() + 5 * 86400);
  o.priority = std::min(5, std::max(1, static_cast<int>(body["priority"].asInt(3))));
  o.urgent = body["urgent"].asBool(false);

  if (p.findRouting(o.product) == nullptr) return errJson("产品 " + o.product + " 不存在工艺路线");
  if (p.findOrder(o.code) >= 0) return errJson("订单号 " + o.code + " 已存在");

  p.orders.push_back(o);
  p.rebuild();
  p.buildCalendars();
  repo_.bump();
  repo_.logEvent("order", "新增订单 " + o.code + " (" + o.product + ", qty=" +
                              std::to_string(o.qty) + ")");

  json::Value v = json::Value::object();
  v.set("ok", true);
  v.set("summary", repo_.summaryJson());
  return HttpResponse::json(v.dump());
}

// 急单插队重排: 需先有排产结果; 构造急单后调用 Rescheduler::insertOrder
// (冻结已开工工序), 写回并上报漂移/稳定性代价等统计。
HttpResponse Api::insertOrder(const HttpRequest& req) {
  bool bodyOk = true;
  json::Value body = parseBody(req, bodyOk);
  if (!bodyOk) return errJson("请求体不是合法 JSON");

  RescheduleOptions opt;
  opt.now = timeutil::now();
  opt.windowEnd = opt.now + 7 * 86400;
  opt.timeLimitMs = 400;
  opt.seed = static_cast<std::uint64_t>(body["seed"].asInt(20261105));

  std::lock_guard<std::mutex> lk(repo_.mutex());
  Problem& p = repo_.problem();
  if (repo_.latest().assign.empty()) return errJson("请先执行一次排产, 再进行插单");

  Order o;
  o.code = body["code"].asString();
  if (o.code.empty()) o.code = "URG-" + std::to_string(9001 + p.numOrders());
  o.product = body["product"].asString();
  if (o.product.empty() && !p.routings.empty()) o.product = p.routings[0].product;
  o.qty = body["qty"].asInt(1500);
  o.release = parseTime(body["release"], opt.now);
  o.due = parseTime(body["due"], opt.now + 3 * 86400);
  o.priority = 5;
  o.urgent = true;

  if (p.findRouting(o.product) == nullptr) return errJson("产品 " + o.product + " 不存在工艺路线");
  if (p.findOrder(o.code) >= 0) return errJson("订单号 " + o.code + " 已存在");

  RescheduleReport rep = Rescheduler::insertOrder(p, repo_.latest(), o, opt);
  if (!rep.ok && rep.solution.assign.empty()) return errJson(rep.message, 409);

  repo_.bump();
  repo_.setLatest(std::move(rep.solution), "reschedule:insert");
  repo_.logEvent("insert", "急单 " + o.code + " 插入: " + rep.message +
                               ", 漂移工序 " + std::to_string(rep.movedOps) + " 道");

  json::Value v = repo_.solutionJson(true);
  v.set("ok", true);
  v.set("message", rep.message);
  {
    json::Value r = json::Value::object();
    r.set("movedOps", rep.movedOps);
    r.set("machineChanges", rep.machineChanges);
    r.set("totalShiftHours", static_cast<double>(rep.totalShiftSec) / 3600.0);
    r.set("stabilityCost", rep.stabilityCost);
    r.set("objectiveBefore", rep.objectiveBefore);
    r.set("objectiveAfter", rep.objectiveAfter);
    r.set("elapsedMs", rep.explainMs);
    r.set("frozenOps", rep.frozenOps);
    v.set("reschedule", r);
  }
  return HttpResponse::json(v.dump());
}

// 设备故障重排: 校验设备存在且未停机 -> 调 Rescheduler::machineDown 将该设备
// 未开工工序迁出 -> 写回结果并记录事件。
HttpResponse Api::breakdown(const HttpRequest& req) {
  bool bodyOk = true;
  json::Value body = parseBody(req, bodyOk);
  if (!bodyOk) return errJson("请求体不是合法 JSON");

  std::lock_guard<std::mutex> lk(repo_.mutex());
  Problem& p = repo_.problem();
  if (repo_.latest().assign.empty()) return errJson("请先执行一次排产");

  std::string code = body["machine"].asString();
  int m = p.findMachine(code);
  if (m < 0) return errJson("设备 " + code + " 不存在");
  if (p.machines[static_cast<size_t>(m)].down) return errJson("设备 " + code + " 已经是停机状态");

  RescheduleOptions opt;
  opt.now = timeutil::now();
  opt.windowEnd = opt.now + 7 * 86400;
  opt.seed = 20261105;

  RescheduleReport rep = Rescheduler::machineDown(p, repo_.latest(), m, opt);
  repo_.bump();
  repo_.setLatest(std::move(rep.solution), "reschedule:breakdown");
  repo_.logEvent("breakdown", rep.message + ", 计划漂移 " + std::to_string(rep.movedOps) + " 道");

  json::Value v = repo_.solutionJson(true);
  v.set("ok", true);
  v.set("message", rep.message);
  {
    json::Value r = json::Value::object();
    r.set("movedOps", rep.movedOps);
    r.set("machineChanges", rep.machineChanges);
    r.set("totalShiftHours", static_cast<double>(rep.totalShiftSec) / 3600.0);
    r.set("stabilityCost", rep.stabilityCost);
    r.set("objectiveBefore", rep.objectiveBefore);
    r.set("objectiveAfter", rep.objectiveAfter);
    r.set("elapsedMs", rep.explainMs);
    v.set("reschedule", r);
  }
  return HttpResponse::json(v.dump());
}

// 设备恢复: 按路径参数 code 找到设备并标记上线 (machineUp), bump 并提示重排。
HttpResponse Api::restore(const HttpRequest& req) {
  std::lock_guard<std::mutex> lk(repo_.mutex());
  Problem& p = repo_.problem();
  int m = p.findMachine(req.param("code"));
  if (m < 0) return errJson("设备不存在", 404);
  Rescheduler::machineUp(p, m);
  repo_.bump();
  repo_.logEvent("restore", "设备 " + p.machines[static_cast<size_t>(m)].code + " 恢复上线");
  json::Value v = json::Value::object();
  v.set("ok", true);
  v.set("message", "设备已恢复, 请重新排产");
  return HttpResponse::json(v.dump());
}

// 返回事件流 (eventsJson), 前端用于展示操作/重排历史。
HttpResponse Api::events(const HttpRequest&) {
  std::lock_guard<std::mutex> lk(repo_.mutex());
  json::Value v = json::Value::object();
  v.set("ok", true);
  v.set("events", repo_.eventsJson());
  return HttpResponse::json(v.dump());
}

// 规模-性能评测: 按 sweep 或指定规模生成合成算例, 对 rule 与 ga 各求解并
// 对比目标值/耗时/OTD, 返回多档明细行 (不读写主数据)。
HttpResponse Api::benchmark(const HttpRequest& req) {
  bool bodyOk = true;
  json::Value body = parseBody(req, bodyOk);
  if (!bodyOk) return errJson("请求体不是合法 JSON");

  const double tLimit = std::min(20000.0, std::max(100.0, body["timeLimitMs"].asDouble(800.0)));
  const std::uint64_t seed = static_cast<std::uint64_t>(body["seed"].asInt(20261105));

  struct Case {
    int orders, ops, machines;
  };
  std::vector<Case> cases;
  if (body["sweep"].asBool(true)) {
    cases = {{20, 6, 8}, {50, 6, 10}, {100, 6, 12}, {200, 6, 14}};
  } else {
    cases.push_back({static_cast<int>(body["orders"].asInt(50)),
                     static_cast<int>(body["opsPerOrder"].asInt(6)),
                     static_cast<int>(body["machines"].asInt(10))});
  }

  json::Value rows = json::Value::array();
  RuleScheduler rs;
  GaScheduler gs;

  for (const auto& c : cases) {
    Problem p = makeSynthetic(c.orders, c.ops, c.machines, seed);
    const int totalOps = p.numOps();

    SolveOptions ro;
    ro.detailedKpi = true;
    ro.rule = "AUTO";
    auto t0 = Clock::now();
    Solution sRule = rs.solve(p, ro);
    double ruleMs = msSince(t0);

    SolveOptions go;
    go.detailedKpi = true;
    go.timeLimitMs = tLimit;
    go.seed = seed;
    t0 = Clock::now();
    Solution sGa = gs.solve(p, go);
    double gaMs = msSince(t0);

    double improve = 0.0;
    if (sRule.kpi.objective > 1e-9)
      improve = (sRule.kpi.objective - sGa.kpi.objective) / sRule.kpi.objective;

    json::Value r = json::Value::object();
    r.set("orders", c.orders);
    r.set("ops", totalOps);
    r.set("machines", p.numMachines());
    r.set("ruleMs", ruleMs);
    r.set("ruleObjective", sRule.kpi.objective);
    r.set("ruleOtd", sRule.kpi.otd);
    r.set("ruleTardinessHours", static_cast<double>(sRule.kpi.totalTardiness) / 3600.0);
    r.set("ruleSetupMinutes", static_cast<double>(sRule.kpi.setupSec) / 60.0);
    r.set("gaMs", gaMs);
    r.set("gaObjective", sGa.kpi.objective);
    r.set("gaOtd", sGa.kpi.otd);
    r.set("gaTardinessHours", static_cast<double>(sGa.kpi.totalTardiness) / 3600.0);
    r.set("gaSetupMinutes", static_cast<double>(sGa.kpi.setupSec) / 60.0);
    r.set("improvement", improve);
    r.set("evaluations", sGa.kpi.evaluations);
    r.set("opsPerSec", gaMs > 0 ? static_cast<double>(totalOps) * 1000.0 / gaMs : 0.0);
    rows.push(r);
  }

  json::Value v = json::Value::object();
  v.set("ok", true);
  v.set("timeLimitMs", tLimit);
  v.set("rows", rows);
  v.set("cpuThreads", static_cast<int>(std::thread::hardware_concurrency()));
  return HttpResponse::json(v.dump());
}

// 导出当前主数据为 JSON (缩进 2 空格), 供导入/保存算例用。
HttpResponse Api::exportProblem(const HttpRequest&) {
  std::lock_guard<std::mutex> lk(repo_.mutex());
  HttpResponse r;
  r.status = 200;
  r.contentType = "application/json; charset=utf-8";
  r.body = problemToJson(repo_.problem()).dump(2);
  return r;
}

}  // namespace aps

// ---------------------------------------------------------------------------
// main.cpp -- 服务入口 + CLI 工具
//
//   aps_server serve [--port 8080] [--web ./web] [--config config.json]
//                    [--orders 80] [--days 14] [--two-shifts]
//   aps_server bench [--time 800] [--seed 20261105]
//   aps_server solve [--algo ga|lns|auto|all|rule] [--time 2000] [--orders 80]
//
// serve 是主业务: 起 HTTP 服务 + 内置演示算例, 浏览器打开即用。
// 通用配置 (工作日/班次/算例规模/求解预算) 从 config.json 读写, 详见 data/app_config.hpp。
// bench / solve 是用于命令行自测
// ---------------------------------------------------------------------------
//
//   - argValue/hasFlag: 解析命令行参数
//   - printKpi: 格式化打印一组 KPI
//   - cmdServe: 启动 HTTP 服务 + 后台自动排产
//   - cmdBench: 多档规模 rule / ga / lns 性能对比
//   - cmdSolve: 对演示算例跑规则/GA/LNS 并打印 KPI 与校验
//   - main: 按子命令分发
//
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "aps/data/app_config.hpp"
#include "aps/data/demo_data.hpp"
#include "aps/data/repository.hpp"
#include "aps/engine/ga_scheduler.hpp"
#include "aps/engine/lns_scheduler.hpp"
#include "aps/engine/rule_scheduler.hpp"
#include "aps/engine/validator.hpp"
#include "aps/server/api.hpp"
#include "aps/server/http_server.hpp"

using namespace aps;

namespace {

/// @brief 取 "--key value" 形式的参数值; 未找到返回 def。从 argv[2] 开始 (跳过程序名/子命令)。
/// @param argc 命令行参数个数
/// @param argv 命令行参数数组 (argv[0]=程序名, argv[1]=子命令)
/// @param key  要查的参数名, 含前缀, 如 "--port"
/// @param def  参数缺失 (或 --key 位于末尾没有值) 时返回的默认值
/// @return     参数值字符串; 缺失时返回 def
std::string argValue(int argc, char** argv, const std::string& key, const std::string& def) {
  for (int i = 2; i + 1 < argc; ++i)
    if (key == argv[i]) return argv[i + 1];
  return def;
}

/// @brief 判断是否存在某个纯开关型参数 (如 --two-shifts); 从 argv[2] 起。
/// @param argc 命令行参数个数
/// @param argv 命令行参数数组
/// @param key  要查的开关名, 含前缀, 如 "--two-shifts"
/// @return     存在返回 true; 否则 false
bool hasFlag(int argc, char** argv, const std::string& key) {
  for (int i = 2; i < argc; ++i)
    if (key == argv[i]) return true;
  return false;
}

/// @brief 按固定格式打印一组关键 KPI (目标值/OTD/延期/换型/跨度/耗时/解码次数)。
/// @param tag 行首标签, 一般是求解器名 (rule / ga), 用于区分多行结果
/// @param s   待打印的解 (读其 kpi 字段)
void printKpi(const std::string& tag, const Solution& s) {
  const KPI& k = s.kpi;
  std::printf("%-10s obj=%9.2f  OTD=%5.1f%%  late=%3d/%3d  tard=%8.1fh  setup=%6.0fmin  makespan=%7.1fh  %8.1fms  evals=%lld\n",
              tag.c_str(), k.objective, k.otd * 100.0, k.ordersLate, k.ordersTotal,
              static_cast<double>(k.totalTardiness) / 3600.0,
              static_cast<double>(k.setupSec) / 60.0,
              static_cast<double>(k.makespanSpan) / 3600.0, k.solveMs, k.evaluations);
}

// serve 子命令: 读通用配置 (缺文件则生成模板) -> 初始化演示算例
// -> 起 HTTP 服务 (后台线程按配置预排一版) -> 监听端口 -> 阻塞于 run()。
// 优先级: 命令行参数 > config.json > 内置默认值。
int cmdServe(int argc, char** argv) {
  const int port = std::atoi(argValue(argc, argv, "--port", "8080").c_str());
  const std::string web = argValue(argc, argv, "--web", "web");
  const std::string cfgPath = argValue(argc, argv, "--config", "config.json");

  // ---- 通用配置: 读文件, 不存在则用内置默认并生成模板, 方便用户直接编辑 ----
  AppConfig cfg;
  std::string cfgErr;
  if (!loadAppConfig(cfgPath, cfg, cfgErr)) {
    if (cfgErr != "missing")
      std::printf("[WARN] 配置文件 %s 解析失败 (%s), 改用内置默认值\n", cfgPath.c_str(),
                  cfgErr.c_str());
    cfg = defaultAppConfig();
  }

  // 命令行覆盖 (优先级最高, 但不回写配置文件)
  //  cliOrders --orders N      本次运行造多少个订单 (0/缺省 = 用配置里的 demo.orders)
  //  cliDays   --days   N      本次排产窗口多少天 (0/缺省 = 用配置里的 demo.days)
  //  twoShifts --two-shifts    在配置日历上再叠一个 18-22 夜班 (产能翻倍; 不带则完全按配置)
  const int cliOrders = std::atoi(argValue(argc, argv, "--orders", "0").c_str());
  const int cliDays = std::atoi(argValue(argc, argv, "--days", "0").c_str());
  const bool twoShifts = hasFlag(argc, argv, "--two-shifts");
  if (cliOrders > 0) cfg.orderCount = cliOrders;
  if (cliDays > 0) cfg.horizonDays = cliDays;
  if (twoShifts) cfg.calendar.shifts = {{8, 12}, {13, 17}, {18, 22}};

  // 若配置文件缺失，则生成默认配置文件
  if (cfgErr == "missing") {
    std::string saveErr;
    if (saveAppConfig(cfgPath, cfg, saveErr))
      std::printf("[INFO] 已生成默认配置文件 %s, 可直接编辑后重启生效\n", cfgPath.c_str());
    else
      std::printf("[WARN] 默认配置文件生成失败: %s\n", saveErr.c_str());
  }

  // 全局共享状态容器
  Repository repo;
  repo.initDemo(toDemoConfig(cfg));

  // Web服务
  HttpServer srv;
  srv.setStaticDir(web);
  srv.setThreads(8);

  // 接口层，容器数据转发Web
  Api api(repo);
  api.setAppConfig(cfg, cfgPath);
  api.registerRoutes(srv);

  // 启动即按配置排一版, 打开页面就有结果可看 (不阻塞端口监听)
  std::thread([&repo, cfg]() {
    try {
      SolveOptions opt;
      opt.detailedKpi = true;
      opt.timeLimitMs = cfg.timeLimitMs;
      Problem p = repo.problem();
      Solution sol;
      if (cfg.algo == "rule") {
        RuleScheduler rs;
        sol = rs.solve(p, opt);
      } else if (cfg.algo == "lns") {
        LnsScheduler ls;
        sol = ls.solve(p, opt);
      } else {
        GaScheduler gs;
        sol = gs.solve(p, opt);
      }
      std::lock_guard<std::mutex> lk(repo.mutex());
      repo.setLatest(std::move(sol), cfg.algo);
      repo.logEvent("solve", "服务启动自动排产 (algo=" + cfg.algo +
                                  ", 预算 " + std::to_string(static_cast<int>(cfg.timeLimitMs)) + "ms)");
    } catch (...) {
    }
  }).detach();

  // 检查web服务是否正常启动
  if (!srv.listen(port, "0.0.0.0")) {
    std::printf("[ERROR] 端口 %d 监听失败 (可能已被占用)\n", port);
    return 1;
  }

  std::printf("======================================================\n");
  std::printf("  APS 自动排产系统  v1.0\n");
  std::printf("  算例: %d 订单 / %d 工序 / %d 设备 / %d 天窗口\n", repo.problem().numOrders(),
              repo.problem().numOps(), repo.problem().numMachines(), cfg.horizonDays);
  std::printf("  本机访问: http://localhost:%d\n", port);
  std::printf("  配置文件: %s\n", cfgPath.c_str());
  std::printf("  静态目录: %s\n", web.c_str());
  std::printf("  Ctrl+C 退出\n");
  std::printf("======================================================\n");
  std::fflush(stdout);

  srv.run();
  return 0;
}

// bench 子命令: 对多档规模合成算例分别跑 rule / ga / lns, 打印耗时/目标值/改进率
// 与校验结果 (纯命令行, 不依赖 HTTP 服务)。
int cmdBench(int argc, char** argv) {
  const double tLimit = std::atof(argValue(argc, argv, "--time", "800").c_str());
  const std::uint64_t seed = static_cast<std::uint64_t>(
      std::strtoull(argValue(argc, argv, "--seed", "20261105").c_str(), nullptr, 10));

  /// @brief bench 的一档评测规模: 订单数 / 每单工序数 / 设备数。
  struct Case {
    int orders, ops, machines;
  };
  std::vector<Case> cases = {{20, 6, 8}, {50, 6, 10}, {100, 6, 12}, {200, 6, 14}, {400, 6, 16}};

  std::printf("\n=== APS 规模-性能评测 (每档 GA/LNS 预算 %.0fms, 线程数 %u) ===\n\n", tLimit,
              std::thread::hardware_concurrency());
  std::printf("%6s %7s %8s | %9s %9s %9s | %10s %10s %10s | %7s %7s %6s\n", "orders", "ops",
              "machines", "rule(ms)", "ga(ms)", "lns(ms)", "rule_obj", "ga_obj", "lns_obj", "ga+",
              "lns+", "check");
  std::printf("--------------------------------------------------------------------------------"
              "------------------------------------------\n");

  RuleScheduler rs;
  GaScheduler gs;
  LnsScheduler ls;

  for (const auto& c : cases) {
    Problem p = makeSynthetic(c.orders, c.ops, c.machines, seed);

    SolveOptions ro;
    ro.detailedKpi = true;
    Solution sRule = rs.solve(p, ro);

    SolveOptions go;
    go.detailedKpi = true;
    go.timeLimitMs = tLimit;
    go.seed = seed;
    Solution sGa = gs.solve(p, go);

    SolveOptions lo = go;
    Solution sLns = ls.solve(p, lo);

    ValidationResult vr = validate(p, sGa);
    ValidationResult vl = validate(p, sLns);
    double gaImp = sRule.kpi.objective > 1e-9
                       ? (sRule.kpi.objective - sGa.kpi.objective) / sRule.kpi.objective
                       : 0.0;
    double lnsImp = sRule.kpi.objective > 1e-9
                        ? (sRule.kpi.objective - sLns.kpi.objective) / sRule.kpi.objective
                        : 0.0;

    std::printf("%6d %7d %8d | %9.1f %9.1f %9.1f | %10.2f %10.2f %10.2f | %6.1f%% %6.1f%% %s\n",
                c.orders, p.numOps(), p.numMachines(), sRule.kpi.solveMs, sGa.kpi.solveMs,
                sLns.kpi.solveMs, sRule.kpi.objective, sGa.kpi.objective, sLns.kpi.objective,
                gaImp * 100.0, lnsImp * 100.0, (vr.ok && vl.ok) ? "OK" : "INVALID");
  }
  std::printf("\n");
  return 0;
}

// solve 子命令: 对演示算例跑规则解, 再按 --algo 跑 GA / LNS, 打印 KPI、校验与改进率。
// --algo rule 只跑规则; ga / lns 单跑一种; auto 或 all 则 GA + LNS 都跑, 便于三方对照。
// 算例日历同样来自 config.json (没有则内置默认)。
int cmdSolve(int argc, char** argv) {
  const std::string algo = argValue(argc, argv, "--algo", "ga");
  const double tLimit = std::atof(argValue(argc, argv, "--time", "2000").c_str());
  const int orders = std::atoi(argValue(argc, argv, "--orders", "0").c_str());

  AppConfig cfg;
  std::string cfgErr;
  if (!loadAppConfig(argValue(argc, argv, "--config", "config.json"), cfg, cfgErr))
    cfg = defaultAppConfig();

  DemoConfig dcfg = toDemoConfig(cfg);
  if (orders > 0) dcfg.orderCount = orders;
  Problem p = makeDemoFactory(dcfg);

  std::printf("\n=== 演示算例: %d 订单 / %d 工序 / %d 设备 ===\n\n", p.numOrders(), p.numOps(),
              p.numMachines());

  RuleScheduler rs;
  Solution sRule = rs.solve(p, SolveOptions{});
  ValidationResult v1 = validate(p, sRule);
  printKpi("rule", sRule);
  std::printf("           校验: %s\n", v1.summary().c_str());

  const bool wantGa = algo == "ga" || algo == "auto" || algo == "all";
  const bool wantLns = algo == "lns" || algo == "auto" || algo == "all";

  if (wantGa) {
    GaScheduler gs;
    SolveOptions opt;
    opt.timeLimitMs = tLimit;
    opt.seed = 20261105;
    Solution sGa = gs.solve(p, opt);
    ValidationResult v2 = validate(p, sGa);
    printKpi("ga", sGa);
    std::printf("           校验: %s\n", v2.summary().c_str());
    if (sRule.kpi.objective > 1e-9) {
      std::printf("           GA 相比规则解改进: %.1f%%\n",
                  (sRule.kpi.objective - sGa.kpi.objective) / sRule.kpi.objective * 100.0);
    }
  }

  if (wantLns) {
    LnsScheduler ls;
    SolveOptions opt;
    opt.timeLimitMs = tLimit;
    opt.seed = 20261105;
    Solution sLns = ls.solve(p, opt);
    ValidationResult v3 = validate(p, sLns);
    printKpi("lns", sLns);
    std::printf("           校验: %s\n", v3.summary().c_str());
    if (sRule.kpi.objective > 1e-9) {
      std::printf("           LNS 相比规则解改进: %.1f%%\n",
                  (sRule.kpi.objective - sLns.kpi.objective) / sRule.kpi.objective * 100.0);
    }
  }
  std::printf("\n");
  return 0;
}

}  // namespace

// 入口: 设置 UTF-8 控制台 (Windows), 按 argv[1] 分发到 serve/bench/solve,
// 未知命令打印用法并返回 1。
int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif
  std::string cmd = argc > 1 ? argv[1] : "serve";
  // 主程序
  if (cmd == "serve") return cmdServe(argc, argv);
  // 性能测试
  if (cmd == "bench") return cmdBench(argc, argv);
  // 校验求解逻辑是否正确
  if (cmd == "solve") return cmdSolve(argc, argv);

  std::printf("用法:\n");
  std::printf("  aps_server serve [--port 8080] [--web ./web] [--config config.json]\n");
  std::printf("                    [--orders 80] [--days 14] [--two-shifts]\n");
  std::printf("  aps_server bench [--time 800] [--seed 20261105]\n");
  std::printf("  aps_server solve [--algo ga|lns|auto|all|rule] [--time 2000] [--orders 80]\n");
  std::printf("\n通用配置 (工作日/班次/订单数/求解预算) 在 config.json, 首次启动自动生成模板,\n");
  std::printf("运行中也可通过 GET/POST /api/config 读写。\n");
  return 1;
}

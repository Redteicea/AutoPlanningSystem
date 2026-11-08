// ---------------------------------------------------------------------------
// main.cpp -- 服务入口 + CLI 工具
//
//   aps_server serve [--port 8080] [--web ./web] [--orders 80] [--days 14]
//   aps_server bench [--time 800] [--seed 1]
//   aps_server solve [--algo ga] [--time 2000] [--orders 80]
//
// serve 是主业务: 起 HTTP 服务 + 内置演示算例, 浏览器打开即用。
// bench / solve 是用于命令行自测
// ---------------------------------------------------------------------------
//
//   - argValue/hasFlag: 解析命令行参数
//   - printKpi: 格式化打印一组 KPI
//   - cmdServe: 启动 HTTP 服务 + 后台自动排产
//   - cmdBench: 多档规模 rule vs ga 性能对比
//   - cmdSolve: 对演示算例跑规则/GA 并打印 KPI 与校验
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

#include "aps/data/demo_data.hpp"
#include "aps/data/repository.hpp"
#include "aps/engine/ga_scheduler.hpp"
#include "aps/engine/rule_scheduler.hpp"
#include "aps/engine/validator.hpp"
#include "aps/server/api.hpp"
#include "aps/server/http_server.hpp"

using namespace aps;

namespace {

/// @brief 取 "--key value" 形式的参数值; 未找到返回 def。从 argv[2] 开始 (跳过程序名/子命令)。
/// @param argc 
/// @param argv 
/// @param key 
/// @param def 
/// @return 
std::string argValue(int argc, char** argv, const std::string& key, const std::string& def) {
  for (int i = 2; i + 1 < argc; ++i)
    if (key == argv[i]) return argv[i + 1];
  return def;
}

/// @brief 判断是否存在某个纯开关型参数 (如 --two-shifts); 从 argv[2] 起。
/// @param argc 
/// @param argv 
/// @param key 
/// @return 
bool hasFlag(int argc, char** argv, const std::string& key) {
  for (int i = 2; i < argc; ++i)
    if (key == argv[i]) return true;
  return false;
}

/// @brief 按固定格式打印一组关键 KPI (目标值/OTD/延期/换型/跨度/耗时/解码次数)。
/// @param tag 
/// @param s 
void printKpi(const std::string& tag, const Solution& s) {
  const KPI& k = s.kpi;
  std::printf("%-10s obj=%9.2f  OTD=%5.1f%%  late=%3d/%3d  tard=%8.1fh  setup=%6.0fmin  makespan=%7.1fh  %8.1fms  evals=%lld\n",
              tag.c_str(), k.objective, k.otd * 100.0, k.ordersLate, k.ordersTotal,
              static_cast<double>(k.totalTardiness) / 3600.0,
              static_cast<double>(k.setupSec) / 60.0,
              static_cast<double>(k.makespanSpan) / 3600.0, k.solveMs, k.evaluations);
}

// serve 子命令: 读参数 -> 初始化演示算例 -> 起 HTTP 服务 (后台线程预排一版)
// -> 监听端口 -> 阻塞于 run() 直至退出。
int cmdServe(int argc, char** argv) {
  const int port = std::atoi(argValue(argc, argv, "--port", "8080").c_str());
  const std::string web = argValue(argc, argv, "--web", "web");
  const int orders = std::atoi(argValue(argc, argv, "--orders", "80").c_str());
  const int days = std::atoi(argValue(argc, argv, "--days", "14").c_str());
  const bool twoShifts = hasFlag(argc, argv, "--two-shifts");

  Repository repo;
  repo.initDemo(orders, days, 20261105, twoShifts);

  HttpServer srv;
  srv.setStaticDir(web);
  srv.setThreads(8);

  Api api(repo);
  api.registerRoutes(srv);

  // 启动即后台排一版, 打开页面就有结果可看 (不阻塞端口监听)
  std::thread([&repo]() {
    try {
      SolveOptions opt;
      opt.detailedKpi = true;
      opt.timeLimitMs = 2000;
      Problem p = repo.problem();
      GaScheduler gs;
      Solution sol = gs.solve(p, opt);
      std::lock_guard<std::mutex> lk(repo.mutex());
      repo.setLatest(std::move(sol), "ga");
      repo.logEvent("solve", "服务启动自动排产 (GA, 预算 2s)");
    } catch (...) {
    }
  }).detach();

  if (!srv.listen(port, "0.0.0.0")) {
    std::printf("[ERROR] 端口 %d 监听失败 (可能已被占用)\n", port);
    return 1;
  }

  std::printf("======================================================\n");
  std::printf("  APS 自动排产系统  v1.0\n");
  std::printf("  算例: %d 订单 / %d 工序 / %d 设备 / %d 天窗口\n", repo.problem().numOrders(),
              repo.problem().numOps(), repo.problem().numMachines(), days);
  std::printf("  本机访问: http://localhost:%d\n", port);
  std::printf("  静态目录: %s\n", web.c_str());
  std::printf("  Ctrl+C 退出\n");
  std::printf("======================================================\n");
  std::fflush(stdout);

  srv.run();
  return 0;
}

// bench 子命令: 对多档规模合成算例分别跑 rule 与 ga, 打印耗时/目标值/改进率
// 与校验结果 (纯命令行, 不依赖 HTTP 服务)。
int cmdBench(int argc, char** argv) {
  const double tLimit = std::atof(argValue(argc, argv, "--time", "800").c_str());
  const std::uint64_t seed = static_cast<std::uint64_t>(
      std::strtoull(argValue(argc, argv, "--seed", "20261105").c_str(), nullptr, 10));

  struct Case {
    int orders, ops, machines;
  };
  std::vector<Case> cases = {{20, 6, 8}, {50, 6, 10}, {100, 6, 12}, {200, 6, 14}, {400, 6, 16}};

  std::printf("\n=== APS 规模-性能评测 (每档 GA 预算 %.0fms, 线程数 %u) ===\n\n", tLimit,
              std::thread::hardware_concurrency());
  std::printf("%6s %8s %9s | %10s %10s | %11s %11s %9s %8s\n", "orders", "ops", "machines",
              "rule(ms)", "ga(ms)", "rule_obj", "ga_obj", "improve", "ga_OTD");
  std::printf("------------------------------------------------------------------------------"
              "-----------------\n");

  RuleScheduler rs;
  GaScheduler gs;

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

    ValidationResult vr = validate(p, sGa);
    double improve = sRule.kpi.objective > 1e-9
                         ? (sRule.kpi.objective - sGa.kpi.objective) / sRule.kpi.objective
                         : 0.0;

    std::printf("%6d %8d %9d | %10.1f %10.1f | %11.2f %11.2f %8.1f%% %7.1f%%  %s\n",
                c.orders, p.numOps(), p.numMachines(), sRule.kpi.solveMs, sGa.kpi.solveMs,
                sRule.kpi.objective, sGa.kpi.objective, improve * 100.0, sGa.kpi.otd * 100.0,
                vr.ok ? "OK" : "INVALID");
  }
  std::printf("\n");
  return 0;
}

// solve 子命令: 对演示算例跑规则解, 必要时再跑 GA, 打印 KPI、校验与改进率。
int cmdSolve(int argc, char** argv) {
  const std::string algo = argValue(argc, argv, "--algo", "ga");
  const double tLimit = std::atof(argValue(argc, argv, "--time", "2000").c_str());
  const int orders = std::atoi(argValue(argc, argv, "--orders", "80").c_str());

  DemoConfig cfg;
  cfg.orderCount = orders;
  Problem p = makeDemoFactory(cfg);

  std::printf("\n=== 演示算例: %d 订单 / %d 工序 / %d 设备 ===\n\n", p.numOrders(), p.numOps(),
              p.numMachines());

  RuleScheduler rs;
  Solution sRule = rs.solve(p, SolveOptions{});
  ValidationResult v1 = validate(p, sRule);
  printKpi("rule", sRule);
  std::printf("           校验: %s\n", v1.summary().c_str());

  if (algo == "ga" || algo == "auto") {
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
  if (cmd == "serve") return cmdServe(argc, argv);
  if (cmd == "bench") return cmdBench(argc, argv);
  if (cmd == "solve") return cmdSolve(argc, argv);

  std::printf("用法:\n");
  std::printf("  aps_server serve [--port 8080] [--web ./web] [--orders 80] [--days 14]\n");
  std::printf("  aps_server bench [--time 800] [--seed 1]\n");
  std::printf("  aps_server solve [--algo ga|rule|auto] [--time 2000] [--orders 80]\n");
  return 1;
}

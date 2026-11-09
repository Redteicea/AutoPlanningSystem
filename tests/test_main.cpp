// ---------------------------------------------------------------------------
// tests/test_main.cpp -- 零依赖单元测试
//
//   T1 JSON 往返
//   T2 日历推算 (跨班次/跨天)
//   T3 手算小算例: 解码结果与人工推演一致
//   T4 约束校验: 正常解 PASS, 人为破坏后被抓住
//   T5 规则解 / GA 在演示算例上均可行, 且 GA 不劣于规则解
//   T6 插单: 已开工工序冻结 (开工时间不变)
//   T7 设备故障: 故障设备上不再有未开工工序
// ---------------------------------------------------------------------------
//
//   - CHECK/CHECK_EQ 宏: 计数通过/失败并打印首个失败点
//   - T1~T7: 覆盖 JSON/日历/解码/校验/求解/插单/故障
//   - main: 依次运行全部测试并汇总结果
//
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "aps/data/app_config.hpp"
#include "aps/data/demo_data.hpp"
#include "aps/engine/decoder.hpp"
#include "aps/engine/ga_scheduler.hpp"
#include "aps/engine/rule_scheduler.hpp"
#include "aps/engine/validator.hpp"
#include "aps/kpi/metrics.hpp"
#include "aps/reschedule/rescheduler.hpp"
#include "aps/util/json.hpp"

using namespace aps;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (cond) {                                                         \
      ++g_pass;                                                         \
    } else {                                                            \
      ++g_fail;                                                         \
      std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                   \
  } while (0)

#define CHECK_EQ(a, b)                                                          \
  do {                                                                          \
    auto va = (a);                                                              \
    auto vb = (b);                                                              \
    if (va == vb) {                                                             \
      ++g_pass;                                                                 \
    } else {                                                                    \
      ++g_fail;                                                                 \
      std::printf("  [FAIL] %s:%d  %s == %s (left=%lld right=%lld)\n", __FILE__, \
                  __LINE__, #a, #b, static_cast<long long>(va),                  \
                  static_cast<long long>(vb));                                   \
    }                                                                           \
  } while (0)

// T1: 验证 JSON 解析与序列化往返 (含转义 \uXXXX 与嵌套数组/对象)。
static void testJson() {
  std::printf("[T1] JSON 往返\n");
  const std::string src = R"({"a":1,"b":[true,null,"x\u4e2d"],"c":{"d":2.5}})";
  json::Value v = json::parse(src);
  CHECK_EQ(v["a"].asInt(), 1);
  CHECK_EQ(v["b"].size(), 3u);
  CHECK(v["b"][2].asString() == "x中");
  CHECK(v["c"]["d"].asDouble() > 2.4);
  json::Value round = json::parse(v.dump());
  CHECK_EQ(round["a"].asInt(), 1);
  CHECK_EQ(round["b"].size(), 3u);
}

// T2: 验证日历 advance/alignStart/capacity (跨午休、跨天、非工作时段顺延)。
static void testTimeline() {
  std::printf("[T2] 日历推算\n");
  CalendarSpec spec;
  spec.workdays = {0, 1, 2, 3, 4, 5, 6};
  spec.shifts = {{8, 12}, {13, 17}};

  Seconds day = timeutil::dayStart(timeutil::parse("2026-03-02 00:00"));
  Timeline cal = Timeline::build(spec, day, timeutil::addDays(day, 5));

  // 8:00 起干 4h -> 12:00
  CHECK_EQ(cal.advance(day + 8 * 3600, 4 * 3600), day + 12 * 3600);
  // 8:00 起干 5h -> 跨午休, 13:00 + 1h = 14:00
  CHECK_EQ(cal.advance(day + 8 * 3600, 5 * 3600), day + 14 * 3600);
  // 16:00 起干 2h -> 次日 9:00
  CHECK_EQ(cal.advance(day + 16 * 3600, 2 * 3600), day + 86400 + 9 * 3600);
  // 非工作时段(午休)开工 -> 顺延到 13:00
  CHECK_EQ(cal.alignStart(day + 12 * 3600 + 600), day + 13 * 3600);
  CHECK(cal.capacity(day + 8 * 3600, day + 17 * 3600) == 8 * 3600);
}

// 手算算例: 1 台设备, 2 张订单, 各 1 道工序 (用于 T3/T4 确定性强校验)。
// 返回构造好的 Problem (含日历与主数据)。
static Problem smallProblem() {
  Problem p;
  p.name = "tiny";
  Seconds day = timeutil::dayStart(timeutil::parse("2026-03-02 00:00"));
  p.horizonStart = day;
  p.horizonEnd = timeutil::addDays(day, 3);
  p.calendar.workdays = {0, 1, 2, 3, 4, 5, 6};
  p.calendar.shifts = {{8, 17}};

  Machine m;
  m.code = "M1";
  m.group = "G";
  p.machines.push_back(m);

  Routing r;
  r.product = "P";
  OperationDef d;
  d.code = "OP1";
  d.name = "加工";
  d.family = "F";
  d.options.push_back({0, 3600});  // 1 件 = 1 小时
  r.ops.push_back(d);
  p.routings.push_back(r);

  Order a;
  a.code = "A";
  a.product = "P";
  a.qty = 1;
  a.release = day + 8 * 3600;
  a.due = day + 12 * 3600;
  a.priority = 3;
  p.orders.push_back(a);

  Order b;
  b.code = "B";
  b.product = "P";
  b.qty = 1;
  b.release = day + 8 * 3600;
  b.due = day + 17 * 3600;
  b.priority = 3;
  p.orders.push_back(b);

  p.rebuild();
  p.buildCalendars();
  return p;
}

// T3: 手算小算例用规则解解码, 核对 EDD 顺序下的开工/完工与 KPI 与人工推演一致。
static void testDecode() {
  std::printf("[T3] 手算小算例\n");
  Problem p = smallProblem();
  CHECK_EQ(p.numOps(), 2);

  RuleScheduler rs;
  Solution s = rs.solve(p, SolveOptions{});
  CHECK_EQ(s.assign.size(), 2u);

  Seconds day = p.horizonStart;
  // EDD: A(12:00) 先于 B(17:00)
  int opA = p.orderOps[0][0];
  int opB = p.orderOps[1][0];
  CHECK_EQ(s.assign[static_cast<size_t>(opA)].start, day + 8 * 3600);
  CHECK_EQ(s.assign[static_cast<size_t>(opA)].end, day + 9 * 3600);
  CHECK_EQ(s.assign[static_cast<size_t>(opB)].start, day + 9 * 3600);
  CHECK_EQ(s.assign[static_cast<size_t>(opB)].end, day + 10 * 3600);

  CHECK_EQ(s.kpi.ordersTotal, 2);
  CHECK_EQ(s.kpi.ordersOnTime, 2);
  CHECK(s.kpi.otd > 0.99);
  CHECK_EQ(s.kpi.makespanSpan, 2 * 3600);

  ValidationResult vr = validate(p, s);
  CHECK(vr.ok);
}

// T4: 校验器能抓违规 —— 正常解 PASS; 人为把两道工序压到同一时刻应报设备重叠。
static void testValidator() {
  std::printf("[T4] 校验器能抓出违规\n");
  Problem p = smallProblem();
  RuleScheduler rs;
  Solution s = rs.solve(p, SolveOptions{});
  CHECK(validate(p, s).ok);

  // 人为把两道工序压到同一时刻 -> 应报设备重叠
  Solution bad = s;
  int opB = p.orderOps[1][0];
  bad.assign[static_cast<size_t>(opB)].start = s.assign[static_cast<size_t>(0)].start;
  bad.assign[static_cast<size_t>(opB)].end = s.assign[static_cast<size_t>(0)].end;
  ValidationResult vr = validate(p, bad);
  CHECK(!vr.ok);
  CHECK(vr.violations.size() > 0);
}

// T5: 演示算例上规则解与 GA 均可行, 且 GA 目标值不应明显劣于规则解 (<=1.02 倍)。
static void testSolvers() {
  std::printf("[T5] 规则解 / GA 可行性\n");
  DemoConfig cfg;
  cfg.orderCount = 40;
  cfg.horizonDays = 14;
  cfg.seed = 7;
  Problem p = makeDemoFactory(cfg);
  CHECK(p.numOps() > 100);

  RuleScheduler rs;
  Solution sRule = rs.solve(p, SolveOptions{});
  ValidationResult v1 = validate(p, sRule);
  CHECK(v1.ok);
  if (!v1.ok) std::printf("%s\n", v1.summary(3).c_str());

  GaScheduler gs;
  SolveOptions opt;
  opt.timeLimitMs = 1200;
  opt.seed = 7;
  Solution sGa = gs.solve(p, opt);
  ValidationResult v2 = validate(p, sGa);
  CHECK(v2.ok);
  if (!v2.ok) std::printf("%s\n", v2.summary().c_str());

  std::printf("       rule obj=%.3f otd=%.3f  ga obj=%.3f otd=%.3f (%.0fms)\n",
              sRule.kpi.objective, sRule.kpi.otd, sGa.kpi.objective, sGa.kpi.otd,
              sGa.kpi.solveMs);
  CHECK(sGa.kpi.objective <= sRule.kpi.objective * 1.02);  // GA 不应明显劣于规则解
}

// T6: 急单插队, 校验已开工工序开工时间冻结不变, 且结果仍可行、工序数守恒。
static void testInsert() {
  std::printf("[T6] 急单插队 + 冻结\n");
  DemoConfig cfg;
  cfg.orderCount = 30;
  cfg.seed = 11;
  Problem p = makeDemoFactory(cfg);

  RuleScheduler rs;
  Solution base = rs.solve(p, SolveOptions{});
  CHECK(validate(p, base).ok);

  RescheduleOptions ro;
  ro.now = p.horizonStart + 86400 + 10 * 3600;  // 模拟"第二天上午, 首日工序已开工"
  ro.windowEnd = ro.now + 7 * 86400;

  Order urgent;
  urgent.code = "URG-001";
  urgent.product = "CARD-STD";
  urgent.qty = 1200;
  urgent.release = ro.now;
  urgent.due = ro.now + 2 * 86400;
  urgent.priority = 5;
  urgent.urgent = true;

  RescheduleReport rep = Rescheduler::insertOrder(p, base, urgent, ro);
  CHECK(rep.ok);
  CHECK(rep.solution.assign.size() == static_cast<size_t>(p.numOps()));
  ValidationResult vr = validate(p, rep.solution);
  CHECK(vr.ok);
  if (!vr.ok) std::printf("%s\n", vr.summary().c_str());

  // 冻结校验: 已开工的工序开工时间不得变化
  int frozenChecked = 0;
  for (int i = 0; i < static_cast<int>(base.assign.size()); ++i) {
    if (base.assign[static_cast<size_t>(i)].start <= ro.now) {
      CHECK_EQ(rep.solution.assign[static_cast<size_t>(i)].start,
               base.assign[static_cast<size_t>(i)].start);
      ++frozenChecked;
    }
  }
  std::printf("       冻结工序 %d 道, 漂移 %d 道, 稳定性代价 %.1f\n", frozenChecked, rep.movedOps,
              rep.stabilityCost);
  CHECK(rep.solution.kpi.otd >= 0.0);
}

// T7: 设备故障后重排, 故障设备 no 之后的未开工工序应为 0, 且结果仍可行。
static void testBreakdown() {
  std::printf("[T7] 设备故障迁移\n");
  DemoConfig cfg;
  cfg.orderCount = 30;
  cfg.seed = 13;
  Problem p = makeDemoFactory(cfg);

  RuleScheduler rs;
  Solution base = rs.solve(p, SolveOptions{});
  CHECK(validate(p, base).ok);

  RescheduleOptions ro;
  ro.now = p.horizonStart + 86400 + 10 * 3600;
  const int m = p.findMachine("P1");

  RescheduleReport rep = Rescheduler::machineDown(p, base, m, ro);
  CHECK(rep.solution.assign.size() == static_cast<size_t>(p.numOps()));

  int onBroken = 0;
  for (const auto& a : rep.solution.assign) {
    if (a.op < 0) continue;
    if (a.machine == m && a.start > ro.now) ++onBroken;
  }
  CHECK_EQ(onBroken, 0);
  ValidationResult vr = validate(p, rep.solution);
  CHECK(vr.ok);
  if (!vr.ok) {
    std::printf("%s\n", vr.summary(3).c_str());
    for (int i = 0; i < (int)rep.solution.assign.size(); ++i) {
      const auto& a = rep.solution.assign[(size_t)i];
      if (a.op >= 0 && a.start <= 0)
        std::printf("        ZERO idx=%d op=%d order=%s seq=%d m=%s end=%lld work=%lld\n", i, a.op,
                    p.orders[(size_t)p.ops[(size_t)a.op].order].code.c_str(),
                    p.ops[(size_t)a.op].seq,
                    a.machine >= 0 ? p.machines[(size_t)a.machine].code.c_str() : "?",
                    (long long)a.end, (long long)a.work());
    }
  }
  std::printf("       %s, 漂移 %d 道, 稳定性代价 %.1f\n", rep.message.c_str(), rep.movedOps,
              rep.stabilityCost);
}

// T8: 通用配置 (AppConfig) 的 JSON 转换与文件读写。
//     覆盖: 默认值 -> JSON -> 解析还原一致; 部分更新只覆盖出现的字段;
//     非法值被拒绝; 真实文件 save 后 load 回来一致; toDemoConfig 让日历随配置走。
static void testAppConfig() {
  std::printf("-- T8 app_config\n");

  // 默认值往返: JSON 化再解析, 字段一致
  AppConfig def = defaultAppConfig();
  AppConfig rt = def;
  std::string err;
  CHECK(appConfigFromJson(appConfigToJson(def), rt, err));
  CHECK_EQ(rt.orderCount, def.orderCount);
  CHECK_EQ(rt.horizonDays, def.horizonDays);
  CHECK_EQ((int)rt.calendar.shifts.size(), (int)def.calendar.shifts.size());
  CHECK_EQ(rt.calendar.shifts[0].startHour, 8);
  CHECK_EQ(rt.calendar.shifts[1].endHour, 17);
  CHECK_EQ((int)rt.calendar.workdays.size(), 5);

  // 部分更新: 只传 calendar, demo/solve 沿用现值
  AppConfig part = def;
  part.orderCount = 123;
  json::Value patch = json::Value::object();
  json::Value cal = json::Value::object();
  json::Value wd = json::Value::array();
  wd.push(0); wd.push(6);                       // 周日 + 周六上班
  cal.set("workdays", wd);
  json::Value sh = json::Value::array();
  json::Value s1 = json::Value::object(); s1.set("start", 22); s1.set("end", 6);  // 跨夜班
  sh.push(s1);
  cal.set("shifts", sh);
  patch.set("calendar", cal);
  CHECK(appConfigFromJson(patch, part, err));
  CHECK_EQ((int)part.calendar.workdays.size(), 2);
  CHECK_EQ(part.calendar.workdays[0], 0);
  CHECK_EQ(part.calendar.shifts[0].startHour, 22);  // 跨夜班原样保留 (编译时展开)
  CHECK_EQ(part.orderCount, 123);                   // 未出现的字段不动

  // 非法值必须被拒绝, 且不改 out
  AppConfig bad = def;
  json::Value badV = json::parse("{\"calendar\":{\"workdays\":[]}}");
  CHECK(!appConfigFromJson(badV, bad, err));
  CHECK_EQ(bad.calendar.workdays.size(), def.calendar.workdays.size());
  badV = json::parse("{\"solve\":{\"algo\":\"magic\"}}");
  CHECK(!appConfigFromJson(badV, bad, err));
  badV = json::parse("{\"calendar\":{\"shifts\":[{\"start\":25,\"end\":6}]}}");
  CHECK(!appConfigFromJson(badV, bad, err));

  // 真实文件: save 后 load 一致; 不存在的文件 err=="missing"
  const std::string path = ".tmp_test_config.json";
  AppConfig saved = def;
  saved.calendar.workdays = {0, 2, 4};
  saved.calendar.shifts = {{9, 18}};
  saved.orderCount = 55;
  CHECK(saveAppConfig(path, saved, err));
  AppConfig loaded;
  CHECK(loadAppConfig(path, loaded, err));
  CHECK_EQ(loaded.orderCount, 55);
  CHECK_EQ((int)loaded.calendar.workdays.size(), 3);
  CHECK_EQ(loaded.calendar.shifts[0].startHour, 9);
  AppConfig miss;
  CHECK(!loadAppConfig("no_such_config_file_xyz.json", miss, err));
  CHECK(err == "missing");
  std::remove(path.c_str());

  // toDemoConfig: 配置日历覆盖内置默认
  Problem p = makeDemoFactory(toDemoConfig(loaded));
  CHECK_EQ((int)p.calendar.shifts.size(), 1);
  CHECK_EQ(p.calendar.shifts[0].startHour, 9);
}

// 测试入口: 依次运行 T1~T8, 打印通过/失败总数, 全过返回 0 否则 1。
int main() {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif
  std::printf("=== APS 单元测试 ===\n");
  testJson();
  testTimeline();
  testDecode();
  testValidator();
  testSolvers();
  testInsert();
  testBreakdown();
  testAppConfig();

  std::printf("\n---- 结果: %d passed, %d failed ----\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

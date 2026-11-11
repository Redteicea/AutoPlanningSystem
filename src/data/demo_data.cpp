#include "aps/data/demo_data.hpp"

// ---------------------------------------------------------------------------
// demo_data.cpp -- 演示算例与合成算例生成
//
// 内容概要:
//   - applyOverrides: 在换型矩阵上铺默认 30 分钟并覆盖指定跨族耗时
//   - makeDemoFactory: 卡片车间真实感算例 (柔性工序 + 换型 + 急单)
//   - makeSynthetic: 任意规模的合成算例 (压测用)
// ---------------------------------------------------------------------------

#include <algorithm>
#include <map>

#include "aps/util/random.hpp"

namespace aps {
namespace {

struct FamilySpec {
  const char* from;
  const char* to;
  Seconds sec;
};

/// @brief 在换型矩阵上铺默认跨族耗时并覆盖指定跨族耗时。
///
/// 先将所有同族（i==j）置 0、跨族（i!=j）填默认 30 分钟（1800 秒），再用 list 中
/// 指定的 from->to 跨族耗时覆盖对应单元格。最终调用 sm.init 重建矩阵。
///
/// @param sm    换型矩阵（原地修改，重建为 list 指定的族集合与耗时）
/// @param list  指定跨族耗时覆盖项（FamilySpec 列表；族不在矩阵中则跳过该项）
void applyOverrides(SetupMatrix& sm, const std::vector<FamilySpec>& list) {
  std::vector<std::vector<Seconds>> t(static_cast<size_t>(sm.size()),
                                      std::vector<Seconds>(static_cast<size_t>(sm.size()), 0));
  for (size_t i = 0; i < t.size(); ++i)
    for (size_t j = 0; j < t.size(); ++j) t[i][j] = 0;  // 同族 0

  // 跨族默认 30 分钟, 先铺满
  for (size_t i = 0; i < t.size(); ++i)
    for (size_t j = 0; j < t.size(); ++j)
      if (i != j) t[i][j] = 1800;

  for (const auto& f : list) {
    int a = sm.indexOf(f.from);
    int b = sm.indexOf(f.to);
    if (a >= 0 && b >= 0) t[static_cast<size_t>(a)][static_cast<size_t>(b)] = f.sec;
  }
  sm.init(sm.families(), t);
}

}  // namespace

/// @brief 生成卡片印刷包装车间的演示算例 (固定设备组 + 换型矩阵 + 3 种工艺路线)。
///
/// 构成: 6 道工序的工艺链 (印刷 -> 覆膜 -> 模切 -> 分切 -> 检测 -> 包装), 每种工序有
/// 1~3 台可选设备且单件工时不同 (柔性), 产品分 3 个族, 跨族换型 30 分钟。
/// 订单按权重随机生成, 约 8% 标记为急单。最后调 rebuild() 展开工序、buildCalendars()
/// 编译设备日历。
///
/// 注意: 订单数量按瓶颈工序 (检测台) 的产能反推校准过, 直接改大 orderCount 会让 OTD 崩掉。
///
/// @param cfg 生成参数: 订单数 / 排产窗口天数 / 随机种子 / 是否加夜班 /
///            可选的自定义日历 (hasCalendar=true 时用 calendar 覆盖内置班次)
/// @return    构造好的算例 (工序已展开、日历已编译, 可直接求解)
Problem makeDemoFactory(const DemoConfig& cfg) {
  Problem p;
  p.name = "卡片印刷包装车间 (demo)";

  const Seconds today = timeutil::dayStart(timeutil::now());
  p.horizonStart = today;
  p.horizonEnd = timeutil::addDays(today, cfg.horizonDays);

  // 日历来源二选一: 配置文件的自定义日历 (hasCalendar) > 内置默认 (周 日休两班, 可加夜班)
  if (cfg.hasCalendar) {
    p.calendar = cfg.calendar;
  } else {
    p.calendar.workdays = {1, 2, 3, 4, 5, 6};  // 周日休息
    p.calendar.shifts = {{8, 12}, {13, 17}};
    if (cfg.twoShifts) p.calendar.shifts = {{8, 12}, {13, 17}, {18, 22}};
  }

  // ---- 设备 ----
  /// @brief 一台演示设备的静态定义 (编码 / 设备组 / 显示名), 用于成表初始化。
  struct MDef {
    const char* code;
    const char* group;
    const char* name;
  };
  const MDef defs[] = {
      {"P1", "PRINT", "四色印刷机 1"}, {"P2", "PRINT", "四色印刷机 2"},
      {"P3", "PRINT", "单色印刷机"},   {"L1", "LAMINATE", "层压机 1 (高档)"},
      {"L2", "LAMINATE", "层压机 2"},  {"D1", "DIE", "冲切机 1"},
      {"D2", "DIE", "冲切机 2"},       {"D3", "DIE", "冲切机 3 (大幅面)"},
      {"W1", "PERSON", "写卡机 1"},    {"W2", "PERSON", "写卡机 2"},
      {"Q1", "INSPECT", "检测台 1"},   {"Q2", "INSPECT", "检测台 2"},
      {"K1", "PACK", "包装线 1"},      {"K2", "PACK", "包装线 2"},
  };
  for (const auto& d : defs) {
    Machine m;
    m.code = d.code;
    m.group = d.group;
    m.name = d.name;
    p.machines.push_back(m);
  }
  auto M = [&](const char* code) { return p.findMachine(code); };
  const int P1 = M("P1"), P2 = M("P2"), P3 = M("P3");
  const int L1 = M("L1"), L2 = M("L2");
  const int D1 = M("D1"), D2 = M("D2"), D3 = M("D3");
  const int W1 = M("W1"), W2 = M("W2");
  const int Q1 = M("Q1"), Q2 = M("Q2");
  const int K1 = M("K1"), K2 = M("K2");

  // ---- 换型矩阵 ----
  p.setup.init({"PRINT-A", "PRINT-B", "PRINT-C", "LAM-STD", "LAM-PREM", "DIE-S", "DIE-M",
                "DIE-L", "PERS", "PERS-2", "INSP", "PACK"},
               {});
  applyOverrides(p.setup, {
      {"PRINT-A", "PRINT-B", 2700},  // 换墨 + 换版
      {"PRINT-B", "PRINT-A", 2700},
      {"PRINT-A", "PRINT-C", 3600},
      {"PRINT-C", "PRINT-A", 3600},
      {"PRINT-B", "PRINT-C", 3000},
      {"PRINT-C", "PRINT-B", 3000},
      {"LAM-STD", "LAM-PREM", 2400},  // 换辊
      {"LAM-PREM", "LAM-STD", 2400},
      {"DIE-S", "DIE-M", 1500},       // 换模
      {"DIE-M", "DIE-S", 1500},
      {"DIE-M", "DIE-L", 2100},
      {"DIE-L", "DIE-M", 2100},
      {"DIE-S", "DIE-L", 2400},
      {"DIE-L", "DIE-S", 2400},
      {"PERS", "PERS-2", 1200},       // 换程序
      {"PERS-2", "PERS", 1200},
      {"INSP", "INSP", 300},          // 检测换规格 (同族也要对版)
      {"PACK", "PACK", 240},
  });

  // ---- 工艺路线 ----
  auto mk = [](const char* code, const char* name, const char* fam,
               std::vector<MachineOption> opts) -> OperationDef {
    OperationDef d;
    d.code = code;
    d.name = name;
    d.family = fam;
    d.options = std::move(opts);
    return d;
  };

  {
    Routing r;
    r.product = "CARD-STD";
    r.ops.push_back(mk("PRINT", "印刷", "PRINT-A", {{P1, 0.36}, {P2, 0.36}, {P3, 0.60}}));
    r.ops.push_back(mk("LAM", "层压", "LAM-STD", {{L1, 0.60}, {L2, 0.60}}));
    r.ops.push_back(mk("DIE", "冲切", "DIE-M", {{D1, 0.72}, {D2, 0.72}, {D3, 0.80}}));
    r.ops.push_back(mk("PERS", "写卡", "PERS", {{W1, 0.20}, {W2, 0.20}}));
    r.ops.push_back(mk("INSP", "检测", "INSP", {{Q1, 0.90}, {Q2, 0.90}}));
    r.ops.push_back(mk("PACK", "包装", "PACK", {{K1, 0.45}, {K2, 0.45}}));
    p.routings.push_back(r);
  }
  {
    Routing r;
    r.product = "CARD-PREM";
    r.ops.push_back(mk("PRINT", "印刷", "PRINT-B", {{P1, 0.42}, {P2, 0.42}}));
    r.ops.push_back(mk("LAM", "层压", "LAM-PREM", {{L1, 0.75}}));  // 只有 L1 能做高档
    r.ops.push_back(mk("DIE", "冲切", "DIE-L", {{D3, 0.95}, {D2, 1.05}}));
    r.ops.push_back(mk("PERS", "写卡", "PERS-2", {{W1, 0.28}, {W2, 0.28}}));
    r.ops.push_back(mk("INSP", "检测", "INSP", {{Q1, 1.10}, {Q2, 1.10}}));
    r.ops.push_back(mk("PACK", "包装", "PACK", {{K1, 0.55}, {K2, 0.55}}));
    p.routings.push_back(r);
  }
  {
    Routing r;
    r.product = "CARD-MINI";
    r.ops.push_back(mk("PRINT", "印刷", "PRINT-C", {{P3, 0.40}, {P1, 0.34}}));
    r.ops.push_back(mk("DIE", "冲切", "DIE-S", {{D1, 0.50}, {D2, 0.55}}));
    r.ops.push_back(mk("PERS", "写卡", "PERS", {{W1, 0.16}, {W2, 0.16}}));
    r.ops.push_back(mk("INSP", "检测", "INSP", {{Q1, 0.70}, {Q2, 0.70}}));
    r.ops.push_back(mk("PACK", "包装", "PACK", {{K1, 0.38}, {K2, 0.38}}));
    p.routings.push_back(r);
  }

  // ---- 订单 ----
  Rng rng(cfg.seed);
  const char* products[3] = {"CARD-STD", "CARD-PREM", "CARD-MINI"};
  const int weights[3] = {55, 25, 20};

  int n = std::max(1, cfg.orderCount);
  for (int i = 0; i < n; ++i) {
    int roll = rng.nextInt(100);
    int pi = roll < weights[0] ? 0 : (roll < weights[0] + weights[1] ? 1 : 2);

    Order o;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "SO-%04d", 1001 + i);
    o.code = buf;
    o.product = products[pi];
    o.qty = 1500 + rng.nextInt(6500);  // 1500 ~ 8000 张 (按瓶颈工序检测台的产能反推的合理批量)
    o.release = today + rng.nextInt(4) * 86400 + rng.nextInt(8) * 3600;
    int leadDays = 5 + rng.nextInt(11);
    o.due = timeutil::addDays(today, leadDays) + 17 * 3600;  // 当天 17:00 前发货

    // 8% 急单: 优先级 5, 交期压到 2~4 天
    if (rng.chance(0.08)) {
      o.urgent = true;
      o.priority = 5;
      o.due = timeutil::addDays(today, 2 + rng.nextInt(3)) + 17 * 3600;
    } else {
      o.priority = 1 + rng.nextInt(4);
    }
    if (o.due <= o.release) o.due = o.release + 2 * 86400;
    p.orders.push_back(o);
  }

  p.rebuild();
  p.buildCalendars();
  return p;
}

/// @brief 生成任意规模的合成算例 (性能压测用, 不追求业务真实感)。
///
/// 单产品、每单 opsPerOrder 道工序, 每道工序随机挂 1~3 台设备; 跨族换型默认 20 分钟;
/// 订单数量与交期随机。固定 seed 时算例完全可复现, 所以 bench 不同天的耗时可以直接对比。
///
/// @param orders      订单数 (需 > 0)
/// @param opsPerOrder 每个订单的工序数 (常数, 不像演示算例那样按产品区分)
/// @param machines    设备总数, 每道工序的候选设备从这里随机挑
/// @param seed        随机种子; 相同 seed 生成完全相同的算例
/// @return            构造好的算例 (工序已展开、日历已编译)
Problem makeSynthetic(int orders, int opsPerOrder, int machines, std::uint64_t seed) {
  Problem p;
  p.name = "synthetic-" + std::to_string(orders) + "x" + std::to_string(opsPerOrder) + "x" +
           std::to_string(machines);
  const Seconds today = timeutil::dayStart(timeutil::now());
  p.horizonStart = today;
  p.horizonEnd = timeutil::addDays(today, 30);
  p.calendar.workdays = {1, 2, 3, 4, 5, 6};
  p.calendar.shifts = {{8, 12}, {13, 17}, {18, 22}};

  Rng rng(seed);
  machines = std::max(1, machines);
  opsPerOrder = std::max(1, opsPerOrder);

  for (int m = 0; m < machines; ++m) {
    Machine mm;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "M%02d", m + 1);
    mm.code = buf;
    mm.group = "G";
    mm.name = buf;
    p.machines.push_back(mm);
  }

  std::vector<std::string> families;
  for (int f = 0; f < 6; ++f) families.push_back("F" + std::to_string(f));
  p.setup.init(families, {});
  // 默认跨族 20 分钟
  {
    std::vector<std::vector<Seconds>> t(families.size(), std::vector<Seconds>(families.size(), 1200));
    for (size_t i = 0; i < families.size(); ++i) t[i][i] = 0;
    p.setup.init(families, t);
  }

  Routing r;
  r.product = "P";
  for (int k = 0; k < opsPerOrder; ++k) {
    OperationDef d;
    d.code = "OP" + std::to_string(k);
    d.name = "OP" + std::to_string(k);
    d.family = families[static_cast<size_t>(rng.nextInt(static_cast<int>(families.size())))];
    // 每道工序 1~3 台可选设备, 工时随机
    int cnt = 1 + rng.nextInt(3);
    std::vector<int> picked;
    while (static_cast<int>(picked.size()) < cnt) {
      int m = rng.nextInt(machines);
      if (std::find(picked.begin(), picked.end(), m) == picked.end()) picked.push_back(m);
    }
    std::sort(picked.begin(), picked.end());
    for (int m : picked) d.options.push_back({m, 0.5 + rng.nextDouble() * 2.0});
    r.ops.push_back(d);
  }
  p.routings.push_back(r);

  int n = std::max(1, orders);
  for (int i = 0; i < n; ++i) {
    Order o;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "SO-%05d", i + 1);
    o.code = buf;
    o.product = "P";
    o.qty = 200 + rng.nextInt(1800);
    o.release = today + rng.nextInt(3) * 86400;
    o.due = timeutil::addDays(today, 5 + rng.nextInt(20)) + 17 * 3600;
    o.priority = 1 + rng.nextInt(5);
    o.urgent = rng.chance(0.1);
    p.orders.push_back(o);
  }

  p.rebuild();
  p.buildCalendars();
  return p;
}

}  // namespace aps

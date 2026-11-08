// ===========================================================================
// engine/ga_scheduler.hpp -- L2 元启发式 (遗传算法)
//
// 编码 (经典 FJSP 双段编码, 不自己发明):
//   OS 段: 长度 = 工序总数, 值为订单号, 第 k 次出现代表该订单第 k 道工序
//          -> 天然保证同一订单的工艺顺序, 任意排列都是合法序列
//   MS 段: 长度 = 工序总数, 值为该工序使用第几个可选设备
//
// 遗传算子:
//   OS: POX (Precedence Operation Crossover) -- 保持工序先后关系
//   MS: 均匀交叉
//   变异: OS 交换/插入, MS 换设备
//   选择: 锦标赛
//
// 种子: 第 0 号个体 = L1 规则最优解原样反编码 (含设备选择 + 真实派工顺序),
//       因此 GA 结果保证不劣于规则解 —— 这是"GA 不退化"的结构性下界。
//
// 并行: 多岛模型 (island model), 每个岛独立演化, 最后取全局最优。
//       比"单种群并行评估"更适合 FJSP, 且天然可复现 (每岛独立种子)。
//
// 收尾: 对最优解做"关键工序重定位"局部搜索 (换设备 + 前移), 通常再降 3~8%。
// ===========================================================================
#pragma once

#include <cstdint>

#include "aps/engine/ischeduler.hpp"

namespace aps {

/// GA 参数 (都有默认值, 一般只调 population / islands)。
struct GaConfig {
  int population = 40;        ///< 种群规模 (每岛); 大算例会自动降到 28~34
  int elite = 2;              ///< 精英保留数 (直接进入下一代, 保证单调不退化)
  int tournament = 3;         ///< 锦标赛选择的参赛个体数, 越大选择压力越强
  double crossoverRate = 0.9; ///< 交叉概率 [0,1]
  double mutationRate = 0.2;  ///< 基础变异概率; 停滞时会自适应爬升 (上限 0.6)
  int maxGenerations = 4000;  ///< 最大代数上限 (实际通常由时间预算先到)
  int stagnationLimit = 400;  ///< 连续多少代无改进就提前收敛退出
  int islands = 0;            ///< 岛数量; 0 = 自动取 min(4, 硬件线程数)
  bool localSearch = true;    ///< 是否对最优个体做关键工序局部搜索
  double localSearchRatio = 0.15;  ///< 时间预算里留给局部搜索的比例 [0, 0.4]
};

/// L2 遗传算法求解器。
class GaScheduler : public IScheduler {
 public:
  /// @return 求解器标签 ("ga"), 用于事件流水与结果追溯
  std::string name() const override { return "ga"; }

  /// 按 SolveOptions 求解 (IScheduler 接口)。
  /// 内部: 种群规模随工序数自适应 -> solveWith()。
  /// @param p   问题实例 (只读)
  /// @param opt 时间预算/随机种子/线程数等
  /// @return 最优解; 时间预算从"种子构造"到"局部搜索结束"全程受控
  Solution solve(const Problem& p, const SolveOptions& opt) override;

  /// 完整参数版求解 (基准测试 / 调参入口)。
  ///
  /// @param p           问题实例 (只读)
  /// @param cfg         GA 参数
  /// @param timeLimitMs 总时间预算(毫秒), 含"规则解种子"构造时间, 会先扣再分给 GA 与局部搜索
  /// @param seed        随机种子; 各岛种子 = seed + 岛号*质数, 可复现
  /// @param detailed    true=最终解回填全量 KPI; false=只保证目标值正确
  /// @return 最优解 (不劣于规则解种子)
  Solution solveWith(const Problem& p, const GaConfig& cfg, double timeLimitMs, std::uint64_t seed,
                     bool detailed);
};

}  // namespace aps

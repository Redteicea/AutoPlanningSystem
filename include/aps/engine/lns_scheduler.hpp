// ===========================================================================
// engine/lns_scheduler.hpp -- L2' 大邻域搜索 (LNS / Adaptive LNS)
//
// 为什么要有这一层 (定位):
//   实测发现 GA 的岛内演化在大算例上几乎不动 (2400 工序 2s 预算下改进 0%),
//   真正在起作用的是收尾那段贪心局部搜索 —— 但它只会"换设备"和"单道工序前移",
//   一次只动一步, 跨不过局部最优。LNS 把这段搜索系统化:
//   一次拆掉 10%~15% 的工序, 再用解码器整块重建, 单步能跨越好几个局部最优。
//
// 与 GA 的关系 (不是替代, 是互补):
//   * 同一套 OS+MS 表达 (order = op 下标序列, ms = 设备选择下标);
//   * 同一个 Decoder 评价, 目标函数完全一致, 结果可直接对比;
//   * 同样以规则解为初始解 -> 结果恒不劣于规则解 (best 从种子解出发, 只被改进替换);
//   * 分工: 小算例 GA 的种群搜索更强 (一次评价的信息量大), 大算例把预算交给 LNS
//     (大算例上 GA 的交叉几乎不产生有效新结构, 而 LNS 每轮必然重排 10% 的工序)。
//
// 一次迭代 = 破坏 (destroy) + 重建 (repair) + 解码评价:
//   destroy: 四个算子轮流用 ——
//            Random  (随机拆 k 道)
//            Order   (整单拆: 拆掉若干订单的全部/部分工序, 治交付维度的局部最优)
//            Machine (按设备拆: 拆掉某台设备上的 k 道, 治设备拥挤/换型堆叠)
//            Tail    (拆派工顺序末尾 k 道, 治 makespan/关键路径尾部)
//   repair:  按"工艺链位置"把拆掉的工序插回合法区间 [前驱之后, 后继之前],
//            设备按"累计负荷 + 本工序工时"最小者重选 —— 一次重建就能改 OS 和 MS 两个维度。
//   accept:  模拟退火准则 (几何降温)。比纯爬山更容易接受暂时变差的解, 但 best 单独保存,
//            所以最终结果仍单调不劣。
//
// 并行: 多链 (multi-chain) 各自独立跑满预算, 最后取最优 —— 与 GA 的多岛同构,
//       每条链独立种子, 结果可复现。
// ===========================================================================
#pragma once

#include <cstdint>
#include <string>

#include "aps/engine/ischeduler.hpp"

namespace aps {

/// @brief LNS 参数 (都有默认值, 一般只调 destroyRatio / chains)。
struct LnsConfig {
  /// 每轮破坏的工序比例 (占总数)。太小跨不出局部最优, 太大重建质量差 ——
  /// 文献与实测的甜点区都在 10%~15% 之间。
  double destroyRatio = 0.12;
  /// 破坏比例的下限保护 (工序数, 保证小算例也至少动几道)
  int minDestroy = 3;
  /// 破坏比例的上限 (占总数)，避免超大比例导致重建退化为随机重排
  double maxDestroyRatio = 0.30;
  /// 最大迭代轮数上限; 0 = 只受时间预算约束 (一般都用 0: 迭代次数由预算决定)
  int maxIterations = 0;
  /// 连续多少轮无改进就提前退出; 0 = 自动 (按工序数取 150~400)
  int stagnationLimit = 0;
  /// 初始温度 = 种子解目标值 x 该比例。越大越敢接受差解 (探索), 越小越接近纯爬山。
  double startTempRatio = 0.006;
  /// 终止温度比例。几何降温到该比例时已经接近纯贪心。
  double endTempRatio = 0.0002;
  /// 并行链数; 0 = min(4, 硬件线程数)
  int chains = 0;
  /// true = 重建时插入位置偏向工艺区间的前段 (尽早派工, 压交期);
  /// false = 区间内均匀随机 (多样性更强)。默认混合使用 (见 lns_scheduler.cpp)。
  bool timeGuidedRepair = true;
};

/// @brief L2' 大邻域搜索求解器 (与 GaScheduler 并列, 由上层按 algo 选择)。
class LnsScheduler : public IScheduler {
 public:
  /// @return 求解器标签 ("lns"), 用于事件流水与结果追溯
  std::string name() const override { return "lns"; }

  /// @brief 按 SolveOptions 求解 (IScheduler 接口)。
  /// @param p   问题实例 (只读)
  /// @param opt 求解选项: timeLimitMs 预算 / seed 随机种子 / threads 并行链数 / detailedKpi
  /// @return    最优解; 恒不劣于规则法种子
  Solution solve(const Problem& p, const SolveOptions& opt) override;

  /// @brief 完整参数版求解 (基准测试 / 调参入口)。
  /// @param p           问题实例 (只读)
  /// @param cfg         LNS 参数
  /// @param timeLimitMs 总时间预算(毫秒), **含**规则解种子构造时间, 内部会先扣除
  /// @param seed        随机种子; 各链种子 = seed + 链号 x 质数, 可复现
  /// @param detailed    true=最终解回填全量 KPI (含逐机利用率); false=只保证目标值正确
  /// @return            最优解 (不劣于规则解种子)
  Solution solveWith(const Problem& p, const LnsConfig& cfg, double timeLimitMs, std::uint64_t seed,
                     bool detailed);
};

}  // namespace aps

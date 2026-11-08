// ===========================================================================
// reschedule/rescheduler.hpp -- 动态重排
//
// 静态排产是作业, 动态重排才是现场。三类事件:
//   * 插单   急单来了, 只能"往空隙里塞", 不能全表重排
//   * 故障   某台设备停了, 把它未开工的活迁到同组其他设备
//   * 滚动   时间往前走, 已开工的冻结, 窗口往后滚
//
// 关键设计: **冻结 + 局部修复**
//   已开工 (start <= now) 的工序一律不动; 其余工序保留原设备分配, 只重新定位时间。
//   重排结果额外输出"稳定性代价" (计划漂移量), 这是工业 APS 与教科书排产的分水岭:
//   计划大幅变动会让备料白做、工人返工, 所以"少动"本身就是一个目标。
//
// 实现共性: 三个入口都先"复放原解的派工顺序 (Solution::opOrder)"建立基线,
// 再只对受影响的工序做插入/迁移 + relayout 连锁顺延 —— 漂移天然有界。
// ===========================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aps/engine/types.hpp"

namespace aps {

/// @brief 重排参数。
struct RescheduleOptions {
  Seconds now = 0;           ///< "当前时刻": start <= now 的工序视为已开工/冻结
  Seconds windowEnd = 0;     ///< 只重排这个时间窗内、且未开工的工序; 窗口外不碰
  double timeLimitMs = 400.0;  ///< 重排计算预算 (毫秒), 目前用于内部限时
  std::uint64_t seed = 20261105;  ///< 随机种子 (局部搜索/打散用, 可复现)
};

/// @brief 重排结果报告: 新解 + 漂移量化指标 (给前端/事件流水展示"动了多少")。
struct RescheduleReport {
  bool ok = false;          ///< 是否成功产出新解
  std::string message;      ///< 人读结论 (如 "设备 D2 停机, 迁移 5/5 道工序")

  Solution solution;        ///< 重排后的完整解 (结构完整, 待 validate 复核)
  int frozenOps = 0;        ///< 冻结(已开工)工序数
  int movedOps = 0;         ///< 开工时间变动 > 60s 的工序数 (计划漂移量)
  int machineChanges = 0;   ///< 被迫换设备的工序数
  Seconds totalShiftSec = 0;  ///< 所有工序开工时间偏移的绝对值总和 (秒)
  double stabilityCost = 0.0;  ///< 稳定性代价 = 时间偏移 + 换机惩罚 (加权后)

  double objectiveBefore = 0.0;  ///< 重排前目标值 (对照)
  double objectiveAfter = 0.0;   ///< 重排后目标值
  double explainMs = 0.0;        ///< 重排耗时 (毫秒)
};

/// @brief 动态重排入口 (全部为静态方法, 无状态)。
class Rescheduler {
 public:
  /// @brief 急单插队: 把新订单的工序追加进问题, 再沿原派工顺序局部插入。
  ///        已开工工序不动; 只在未冻结时段里找空隙, 必要时局部顺延。
  /// @param p        问题实例 (会写入新订单, 调用方需持有)
  /// @param baseline 当前生效的排产解 (冻结标记来自它的 assign)
  /// @param order    急单 (release/due/priority 已填好)
  /// @param opt      重排参数
  /// @return         报告; ok=false 且 solution 为空表示插入失败 (如窗口内无产能)
  static RescheduleReport insertOrder(Problem& p, const Solution& baseline, const Order& order,
                                      const RescheduleOptions& opt);

  /// @brief 设备故障: 目标设备日历从 now 起整段清空 (不改工艺路线, 已开工工序仍合法),
  ///        其上未开工的工序迁移到同组其他设备 + 连锁顺延。
  /// @param p          问题实例 (会写入停机标记, 调用方需持有)
  /// @param baseline   当前生效的排产解
  /// @param machineIdx 故障设备下标 (0..numMachines-1)
  /// @param opt        重排参数
  /// @return           报告 (含迁移数/漂移数/稳定性代价)
  static RescheduleReport machineDown(Problem& p, const Solution& baseline, int machineIdx,
                                      const RescheduleOptions& opt);

  /// @brief 设备恢复上线: 重建该设备日历、清掉停机标记。
  ///        注意: 只恢复能力, 不自动重排 —— 调用方应提示用户重新排产。
  /// @param p          问题实例 (会被修改)
  /// @param machineIdx 恢复的设备下标
  static void machineUp(Problem& p, int machineIdx);

  /// @brief 滚动时域: 时间前进后把基线"搬"到新时间轴 ——
  ///        已开工的冻结, 未开工的按原设备分配重新落到时间轴上 (从 now 或更晚起)。
  /// @param p        问题实例 (只读使用)
  /// @param baseline 当前生效的排产解
  /// @param opt      重排参数
  /// @return         报告 (含漂移统计)
  static RescheduleReport rollForward(Problem& p, const Solution& baseline,
                                      const RescheduleOptions& opt);
};

}  // namespace aps

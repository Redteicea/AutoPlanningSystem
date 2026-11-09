// ===========================================================================
// kpi/metrics.hpp -- 指标计算与目标函数
//
// 一个 Solution 的所有评价都在这里出, 求解器不自己算指标。
// 好处: 换算法时 KPI 口径不变, 前端/报表不需要改。
//
// 目标函数 (加权求和, 量纲统一到"小时/分钟"):
//   cost = w_tardiness * Σ(延期小时 x 优先级权重)
//        + w_setup     * Σ(换型分钟)
//        + w_makespan  * makespan小时
//
// 口径注意: 统计一律用 Assignment::work() (纯加工秒) 而不是 end-start (时钟跨度,
// 含跨班次空闲), 否则利用率/换型统计会虚高。
// ===========================================================================
#pragma once

#include "aps/engine/types.hpp"

namespace aps {

/// @brief KPI 计算器。线程安全: 只读 Problem, 可多线程各持一份。
class Metrics {
 public:
 
  /// @param p 问题实例 (只读引用, 提供权重/订单交期/设备日历等口径)
  explicit Metrics(const Problem& p) : p_(p) {}

  /// @brief 按安排表计算全部 KPI。
  /// @param assign   工序 -> 安排, 下标即 op id
  /// @param detailed false=跳过利用率/设备负荷/在制峰值等展示型指标
  ///                 (GA 内循环每代要评几十个解, 这一项省 60%+ 时间);
  ///                 true=全量 (最终结果给前端)
  /// @return         KPI 结构, objective 字段 = cost() 结果
  KPI compute(const std::vector<Assignment>& assign, bool detailed = true) const;

  /// @brief 按 Problem::weights 把 KPI 折算成单一目标值 (越小越好)。
  /// @param k 已算好的 KPI
  /// @return  目标函数值; 三项权重全为 0 时返回 0
  double cost(const KPI& k) const;

  /// @brief 便捷组合: compute + cost 一步完成, 并把结果写回 assign 对应的 KPI。
  /// @param assign   工序安排表 (本函数不修改其内容, 只用非 const 引用是为了接口对称)
  /// @param detailed 是否算展示型指标
  /// @return         目标函数值
  double costOf(std::vector<Assignment>& assign, bool detailed = false) const;

 private:
  const Problem& p_;  ///< 口径来源 (权重/交期/优先级)
};

}  // namespace aps

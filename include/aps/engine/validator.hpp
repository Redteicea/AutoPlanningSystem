// ===========================================================================
// engine/validator.hpp -- 可行性校验器
//
// 排产系统最危险的 bug 不是"排得不好", 而是"排出来不可行却没人发现"。
// 所以每个求解器的输出都要独立过一遍这里, 只信校验结果, 不信算法自述。
//
// 校验项:
//   V1 完整性    每道工序恰好一个 Assignment, 设备/时间合法
//   V2 设备能力  所选设备确实在该工序的可选集合里
//   V3 日历      占用区间 [start-setup, end] 全部落在设备可用时段内
//   V4 设备互斥  同设备上相邻工序不重叠 (后道准备不得早于前道结束)
//   V5 工艺顺序  同订单内 start 顺序与 seq 一致, 且后道不早于前道结束
//   V6 发布时刻  不早于订单发布时间
//
// 独立性: 本文件不 include 解码器, 与求解器零共享代码 —— 才能起到交叉验证作用。
// ===========================================================================
#pragma once

#include <string>
#include <vector>

#include "aps/engine/types.hpp"

namespace aps {

/// 一条违规记录。
struct Violation {
  std::string type;     ///< 违规类型代码, 如 "V4_machine_overlap"
  int op = -1;          ///< 涉及的工序下标; 全局性问题为 -1
  std::string detail;   ///< 人读的细节描述 (含设备/时刻等)
};

/// 校验结果汇总。
struct ValidationResult {
  bool ok = true;                       ///< 全部通过为 true, 任何违规为 false
  int checkedOps = 0;                   ///< 实际检查的工序数
  std::vector<Violation> violations;    ///< 违规明细 (按检查顺序)

  /// 生成人读摘要。
  /// @param maxItems 最多列出的违规条数, 超出部分以 "...共N条" 收尾
  /// @return 如 "PASS" 或 "V4_machine_overlap x3: D2@11-06 09:00 ... (共 5 条)"
  std::string summary(size_t maxItems = 8) const;
};

/// 校验一份工序安排表 (V1~V6 全查)。
/// @param p      问题实例 (只读)
/// @param assign 工序 -> 安排, 下标即 op id; 允许含 op<0 的占位项 (跳过)
/// @return       校验结果; assign 大小不足 numOps 时直接 V1_incomplete
ValidationResult validate(const Problem& p, const std::vector<Assignment>& assign);

/// 便捷重载: 校验整个 Solution (内部取 sol.assign)。
/// @param p   问题实例 (只读)
/// @param sol 待校验解
/// @return    校验结果
ValidationResult validate(const Problem& p, const Solution& sol);

}  // namespace aps

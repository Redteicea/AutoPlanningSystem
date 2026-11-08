// ===========================================================================
// engine/rule_scheduler.hpp -- L1 规则启发式
//
// 角色:
//   1) 毫秒级兜底 (前端"快速预览"按钮)
//   2) 给 GA 当初始解 / 对照组
//   3) 出问题时的安全网: 任何规模都能在近线性时间内给一份可行解
//
// 算法: 列表调度 (list scheduling)
//   每一步从"各订单当前待排工序"里挑一道最该先干的, 交给插入式解码器落到设备上。
//   只决定"订单之间的先后", 设备由解码器选最早完工的那台 (贪心设备分配)。
// ===========================================================================
#pragma once

#include <vector>
#include "aps/engine/ischeduler.hpp"

namespace aps {

/// @brief L1 规则调度器: 分数最高/最优先的规则解, 也是 GA 的种子来源。
class RuleScheduler : public IScheduler {
 public:
  /// @return 求解器标签 ("rule")
  std::string name() const override { return "rule"; }

  /// @brief 按规则生成派工顺序并解码。
  /// @param p   问题实例 (只读)
  /// @param opt rule 字段:
  ///            - "AUTO" 或空: 七条规则 (EDD/SPT/WSPT/PRIORITY/FIFO/CR + 默认) 全跑,
  ///              取目标函数最优的解 (代价 = 7 次解码)
  ///            - 单条规则名: 只跑该规则
  /// @return   最优规则解; feasible=true 表示通过结构检查, 最终可行性仍以 validate() 为准
  Solution solve(const Problem& p, const SolveOptions& opt) override;

  /// @brief 生成单条规则的工序派工顺序 (不解码)。
  /// @param p    问题实例 (只读)
  /// @param rule 规则名: "EDD" / "SPT" / "WSPT" / "PRIORITY" / "FIFO" / "CR";
  ///             其他值按 EDD 处理
  /// @return     op 下标序列, 每道工序恰好出现一次, 同订单内保持工艺链顺序;
  ///             供单测/基准复用, 也是重排"复放原顺序"的来源
  std::vector<int> buildOrder(const Problem& p, const std::string& rule) const;
};

}  // namespace aps

// ---------------------------------------------------------------------------
// engine/ischeduler.hpp -- 统一求解接口
//
// 三层求解器实现同一个接口, 所以:
//   * 前端一个下拉框就能切换算法, 返回格式不变
//   * 基准测试能对同一算例跑多算法并直接比 KPI
//   * 以后接 CP-SAT / 商用求解器只需要新增一个实现, 不动上层
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

#include "aps/engine/types.hpp"

namespace aps {

/// @brief 一次求解的通用参数 (时间预算 / 种子 / 线程 / 规则名 / 是否算全量指标)。
///
/// 由上层填充后传给 IScheduler::solve; 具体求解器只取自己关心的字段
/// (规则法看 rule, GA 看 timeLimitMs/seed/threads), 其余字段忽略。
struct SolveOptions {
  double timeLimitMs = 2000.0;  // 求解时间预算
  std::uint64_t seed = 20261105;
  int threads = 0;              // 0 = 硬件并发
  std::string rule = "AUTO";    // 规则解: EDD/SPT/WSPT/PRIORITY/FIFO/CR/AUTO
  bool detailedKpi = true;
};

/// @brief 求解器基类
class IScheduler {
 public:
  virtual ~IScheduler() = default;
  virtual std::string name() const = 0;

  /// @brief 返回的解一定结构完整 (每道工序都有 Assignment); 可行性用 validate() 单独校验
  virtual Solution solve(const Problem& p, const SolveOptions& opt) = 0;
};

}  // namespace aps

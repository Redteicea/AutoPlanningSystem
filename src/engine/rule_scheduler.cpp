// ===========================================================================
// engine/rule_scheduler.cpp -- L1 规则启发式实现
//
// buildOrder: 调度步进循环, 每步从"各订单的下一道待排工序"里按规则打分选最优。
//             打分统一为"越大越优先" (val)。
// solve:      按需跑单条或全部 6 条规则, 共享解码器放置, 取目标值最优的解。
// ===========================================================================
#include "aps/engine/rule_scheduler.hpp"

#include <algorithm>
#include <chrono>

#include "aps/engine/decoder.hpp"
#include "aps/kpi/metrics.hpp"

namespace aps {
namespace {

/// @brief 计算工序在所有可选设备上的最短加工时长 (供 SPT/CR 等规则打分)。
///
/// 取各可选设备 "产量 × 单件秒数" 的最小值; 无可选设备 (options 为空) 时返回 0。
///
/// @param op 工序实例, 含 qty 与 options (加工设备与单件耗时)
/// @return   最短加工秒数; 无 options 时返回 0
double minDuration(const OpInst& op) {
  double best = 1e18;
  for (const auto& mo : op.options) {
    double d = static_cast<double>(op.qty) * static_cast<double>(mo.unitSeconds);
    best = std::min(best, d);
  }
  return best <= 0 ? 0.0 : best;
}

/// 参与竞争的规则名单 (solve 的 AUTO 模式会全部跑一遍)。
const char* kAllRules[] = {"EDD", "SPT", "WSPT", "PRIORITY", "FIFO", "CR"};

}  // namespace

/// @brief 按指定规则生成一条派工顺序 (只是顺序, 不含时间表)。
///
/// 调度步进循环: 每轮从"各订单的下一道待排工序"里按规则打分, 选 val 最大的那道排出,
/// 然后把该订单的工艺链指针推进一格。打分统一换算成"越大越优先":
///   EDD 取 -due、SPT 取 -工时、WSPT 取 权重/工时、PRIORITY 取优先级、
///   FIFO 取 -release、CR 取 -临界比 (交期剩余 / 剩余工时); 急单在多数规则里额外加权。
///
/// 复杂度 O(工序数 x 订单数): 每排一道工序都要扫一遍所有订单的当前待排工序 ——
/// 订单数上千时会占掉一部分时间, 但相比解码仍可忽略。
///
/// @param p    问题实例
/// @param rule 规则名 (EDD/SPT/WSPT/PRIORITY/FIFO/CR); 未知名字会退化成 EDD 行为
/// @return     工序全局下标数组, 顺序即派工顺序, 长度 == numOps()
std::vector<int> RuleScheduler::buildOrder(const Problem& p, const std::string& rule) const {
  const int nOrders = p.numOrders();
  std::vector<int> next(static_cast<size_t>(nOrders), 0);
  std::vector<double> remain(static_cast<size_t>(nOrders), 0.0);

  for (int o = 0; o < nOrders; ++o) {
    double sum = 0;
    for (int opIdx : p.orderOps[static_cast<size_t>(o)]) sum += minDuration(p.ops[static_cast<size_t>(opIdx)]);
    remain[static_cast<size_t>(o)] = sum;
  }

  std::vector<int> out;
  out.reserve(static_cast<size_t>(p.numOps()));

  for (int step = 0; step < p.numOps(); ++step) {
    int bestO = -1;
    double bestVal = -1e300;

    for (int o = 0; o < nOrders; ++o) {
      const auto& chain = p.orderOps[static_cast<size_t>(o)];
      if (next[static_cast<size_t>(o)] >= static_cast<int>(chain.size())) continue;
      const OpInst& op = p.ops[static_cast<size_t>(chain[static_cast<size_t>(next[static_cast<size_t>(o)])])];
      const Order& ord = p.orders[static_cast<size_t>(o)];

      const double proc = std::max(1.0, minDuration(op));
      const double due = static_cast<double>(ord.due);
      const double release = static_cast<double>(ord.release);
      const double prio = std::min(5, std::max(1, ord.priority));
      const double w = p.weights.priorityWeight[static_cast<int>(prio)] *
                       (ord.urgent ? 1.5 : 1.0);

      double val = 0.0;
      if (rule == "SPT") {
        val = -proc;
      } else if (rule == "WSPT") {
        val = w / proc;
      } else if (rule == "PRIORITY") {
        val = prio * 1e12 - due;
      } else if (rule == "FIFO") {
        val = -release;
      } else if (rule == "CR") {
        // 临界比: 剩余可用时间 / 剩余工时, 越小越急
        val = -(due - release) / std::max(1.0, remain[static_cast<size_t>(o)]);
      } else {  // EDD 及默认
        val = -due - prio * 60.0;  // 同交期时急单优先
      }

      if (val > bestVal) {
        bestVal = val;
        bestO = o;
      }
    }

    if (bestO < 0) break;
    int opIdx = p.orderOps[static_cast<size_t>(bestO)][static_cast<size_t>(next[static_cast<size_t>(bestO)]++)];
    out.push_back(opIdx);
    remain[static_cast<size_t>(bestO)] -= minDuration(p.ops[static_cast<size_t>(opIdx)]);
    if (remain[static_cast<size_t>(bestO)] < 0) remain[static_cast<size_t>(bestO)] = 0;
  }
  return out;
}

/// @brief 规则法求解入口: 生成派工顺序 -> 插入式解码 -> 取目标值最优的那条。
///
/// AUTO (或 rule 为空) 时把 6 条规则全跑一遍取最优, 否则只跑指定的一条。
/// 所有规则共用同一个解码器, 每道工序都在候选设备里挑完工最早的那台 (chooseBestMachine=true),
/// 所以规则之间的差异纯粹来自"先排谁"。典型耗时: 80 单约 8ms, 2400 工序约 80ms。
///
/// 注意: 因为解码器自带顺序自愈, 这里返回的解恒可行; 只有工序排不进排产窗口时
/// 才会 truncated 并标记 feasible=false。
///
/// @param p   问题实例
/// @param opt 求解选项: rule 指定规则名 (AUTO/EDD/SPT/WSPT/PRIORITY/FIFO/CR),
///            detailedKpi=true 时补算设备利用率等展示用 KPI (不影响目标值)
/// @return    最优解; solver 字段形如 "rule:EDD", kpi.solveMs 是本次实际耗时
Solution RuleScheduler::solve(const Problem& p, const SolveOptions& opt) {
  const auto t0 = std::chrono::steady_clock::now();
  Decoder decoder(p);
  const std::vector<int> noMs;

  std::vector<std::string> rules;
  if (opt.rule == "AUTO" || opt.rule.empty()) {
    for (const char* r : kAllRules) rules.emplace_back(r);
  } else {
    rules.push_back(opt.rule);
  }

  Solution best;
  best.kpi.objective = 1e300;

  for (const auto& r : rules) {
    std::vector<int> order = buildOrder(p, r);
    Solution s = decoder.decodeSequence(order, noMs, /*chooseBestMachine=*/true, "rule:" + r);
    if (s.kpi.objective < best.kpi.objective) best = std::move(s);
  }

  auto t1 = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (opt.detailedKpi) {
    Metrics m(p);
    best.kpi = m.compute(best.assign, true);
  }
  best.kpi.solveMs = ms;
  best.feasible = !best.truncated;
  if (best.truncated) best.infeasibleReason = "部分工序排不进排产窗口 (horizon)";
  return best;
}

}  // namespace aps

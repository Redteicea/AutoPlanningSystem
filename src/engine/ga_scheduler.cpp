// ===========================================================================
// engine/ga_scheduler.cpp -- L2 遗传算法实现
//
// 结构:
//   Ind                     个体 = OS 编码 + MS 编码 + 适应度 (目标值)
//   makeRandom              随机个体 (OS 全排列, MS 以小概率贪心选最快设备)
//   makeSeededFromSolution  把已有解 (规则解/重排解) 原样反编码成个体 —— GA 种子
//   makeSeeded              按规则名生成一个规则解个体
//   runIsland               单岛主循环: 初始化 -> 锦标赛+POX/均匀交叉+变异 -> 精英保留
//   localSearch             关键工序局部搜索 (换设备 + OS 前移, 只接受改进)
//   solveWith               入口: 规则解种子 -> 多岛并行 -> 局部搜索 -> 全量 KPI
//
// 时间纪律: 种子构造时间从总预算中扣除; 各岛在种群初始化和每代之间都检查
// deadline —— 任何情况下总耗时 <= timeLimitMs + 少量收尾开销。
// ===========================================================================
#include "aps/engine/ga_scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "aps/engine/decoder.hpp"
#include "aps/engine/rule_scheduler.hpp"
#include "aps/kpi/metrics.hpp"
#include "aps/util/random.hpp"

namespace aps {
namespace {

using Clock = std::chrono::steady_clock;

/// 从计时起点到现在的毫秒数 (float 毫秒, 用于预算检查)。
double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/// GA 个体: OS 段 (订单序列) + MS 段 (设备选择) + 适应度。
struct Ind {
  std::vector<int> os;             ///< OS 编码: 订单号序列, 第 k 次出现 = 第 k 道工序
  std::vector<int> ms;             ///< MS 编码: ms[op] = 该工序选第几个可选设备
  double cost = 1e300;             ///< 适应度 = 解码目标值, 越小越好
  double bestCostSnapshot = 1e300; ///< 进入局部搜索前的代价快照 (搜索变差时回退用)
};

// ---------------------------------------------------------------------------
// 个体初始化: OS 随机排列; MS 以较大概率选最快设备 (让初代不至于太差)
/// 生成随机个体。
/// @param p            问题实例
/// @param rng          随机源 (每岛独立, 保证可复现)
/// @param greedyMachine true 时 MS 有 35% 概率直接选单件工时最短的设备
/// @return 新个体 (未评价, cost 为初始值)
// ---------------------------------------------------------------------------
Ind makeRandom(const Problem& p, Rng& rng, bool greedyMachine) {
  Ind ind;
  ind.os.reserve(static_cast<size_t>(p.numOps()));
  for (int o = 0; o < p.numOrders(); ++o) {
    const auto& chain = p.orderOps[static_cast<size_t>(o)];
    for (size_t k = 0; k < chain.size(); ++k) ind.os.push_back(o);
  }
  rng.shuffle(ind.os);

  ind.ms.resize(static_cast<size_t>(p.numOps()), 0);
  for (int i = 0; i < p.numOps(); ++i) {
    const OpInst& op = p.ops[static_cast<size_t>(i)];
    if (op.options.size() <= 1) {
      ind.ms[static_cast<size_t>(i)] = 0;
      continue;
    }
    if (greedyMachine && rng.chance(0.35)) {
      int best = 0;
      double bestDur = op.options[0].unitSeconds;
      for (size_t k = 1; k < op.options.size(); ++k) {
        if (op.options[k].unitSeconds < bestDur) {
          bestDur = op.options[k].unitSeconds;
          best = static_cast<int>(k);
        }
      }
      ind.ms[static_cast<size_t>(i)] = best;
    } else {
      ind.ms[static_cast<size_t>(i)] = rng.nextInt(static_cast<int>(op.options.size()));
    }
  }
  return ind;
}

// ---------------------------------------------------------------------------
// 从已有解反推种子个体 (含设备选择)
//   只拿规则解的"工序顺序"不够 —— 必须把它实际选中的设备一起继承,
//   否则 GA 从"最快设备"起步, 在产能紧张时会全挤到一台机床上, 比规则解差一个数量级。
/// 把已有解无损反编码成个体 (GA 的第 0 号种子)。
/// 设备选择取自 assign; 派工顺序取自 opOrder (与解码器放置顺序逐条一致,
/// 原样回放即可复现同一甘特图 —— 这是"GA 不劣于规则解"的关键)。
/// @param p   问题实例
/// @param sol 种子解 (通常是规则法 AUTO 的结果)
/// @return    解码后目标值 == sol 目标值的个体
// ---------------------------------------------------------------------------
Ind makeSeededFromSolution(const Problem& p, const Solution& sol) {
  Ind ind;
  ind.ms.assign(static_cast<size_t>(p.numOps()), 0);

  // 先继承设备选择
  for (int i = 0; i < p.numOps(); ++i) {
    const auto& a = sol.assign[static_cast<size_t>(i)];
    const auto& opts = p.ops[static_cast<size_t>(i)].options;
    if (a.op < 0 || a.machine < 0 || opts.empty()) {
      // 未排上的工序: 用最快设备兜底
      int best = 0;
      for (size_t k = 1; k < opts.size(); ++k)
        if (opts[k].unitSeconds < opts[static_cast<size_t>(best)].unitSeconds)
          best = static_cast<int>(k);
      ind.ms[static_cast<size_t>(i)] = best;
      continue;
    }
    for (size_t k = 0; k < opts.size(); ++k) {
      if (opts[k].machine == a.machine) {
        ind.ms[static_cast<size_t>(i)] = static_cast<int>(k);
        break;
      }
    }
  }

  // 再继承派工顺序。优先用解自带的 opOrder —— 它与解码器当时的放置顺序逐条一致,
  // 原样回放就能复现同一个甘特图; 按 start 排序在"同时刻并列"时会改变顺序,
  // 插入式解码对顺序敏感, 结果会明显变差 (实测 1200 工序下目标值差 50%)。
  std::vector<int> order;
  order.reserve(static_cast<size_t>(p.numOps()));
  std::vector<char> seen(static_cast<size_t>(p.numOps()), 0);
  for (int op : sol.opOrder) {
    if (op < 0 || op >= p.numOps() || seen[static_cast<size_t>(op)]) continue;
    seen[static_cast<size_t>(op)] = 1;
    order.push_back(op);
  }
  for (int i = 0; i < p.numOps(); ++i)
    if (!seen[static_cast<size_t>(i)]) order.push_back(i);

  ind.os.reserve(order.size());
  for (int op : order) ind.os.push_back(p.ops[static_cast<size_t>(op)].order);
  return ind;
}

// ---------------------------------------------------------------------------
// 用规则解做种子: 保证 GA 的起点不差于 L1, 避免"跑了几千代还不如 EDD"
/// 生成"某条规则的解"对应的个体。
/// @param p    问题实例
/// @param rule 规则名 (如 "WSPT"), 内部先 buildOrder 再解码后反编码
/// @return     规则解个体 (评价过, cost 即规则解目标值)
// ---------------------------------------------------------------------------
Ind makeSeeded(const Problem& p, const std::string& rule) {
  RuleScheduler rs;
  std::vector<int> order = rs.buildOrder(p, rule);
  Ind ind;
  ind.os.reserve(static_cast<size_t>(p.numOps()));
  for (int opIdx : order)
    if (opIdx >= 0 && opIdx < p.numOps()) ind.os.push_back(p.ops[static_cast<size_t>(opIdx)].order);
  // 补齐 (理论上不会缺)
  if (static_cast<int>(ind.os.size()) < p.numOps()) {
    for (int o = 0; o < p.numOrders(); ++o)
      for (size_t k = 0; k < p.orderOps[static_cast<size_t>(o)].size(); ++k) ind.os.push_back(o);
  }
  ind.ms.assign(static_cast<size_t>(p.numOps()), 0);
  for (int i = 0; i < p.numOps(); ++i) {
    const OpInst& op = p.ops[static_cast<size_t>(i)];
    int best = 0;
    for (size_t k = 1; k < op.options.size(); ++k)
      if (op.options[k].unitSeconds < op.options[static_cast<size_t>(best)].unitSeconds)
        best = static_cast<int>(k);
    ind.ms[static_cast<size_t>(i)] = best;
  }
  return ind;
}

// ---------------------------------------------------------------------------
// POX 交叉: 只继承父代的工序先后关系, 不破坏各订单内部顺序
/// POX (Precedence Operation Crossover) 交叉。
/// 随机选一半订单作为"保留集": 子代在保留集上的相对顺序继承父代 a,
/// 其余位置按父代 b 的顺序回填 —— 保证任意结果都是合法 OS。
/// @param a       父代 1 的 OS
/// @param b       父代 2 的 OS
/// @param nOrders 订单总数
/// @param rng     随机源
/// @return        子代 OS (长度同父代)
// ---------------------------------------------------------------------------
std::vector<int> pox(const std::vector<int>& a, const std::vector<int>& b, int nOrders, Rng& rng) {
  std::vector<char> inSet(static_cast<size_t>(nOrders), 0);
  for (int o = 0; o < nOrders; ++o) inSet[static_cast<size_t>(o)] = rng.chance(0.5) ? 1 : 0;
  // 至少留一组, 避免退化为完全复制
  bool any = false;
  for (char c : inSet) any = any || c;
  if (!any && nOrders > 0) inSet[0] = 1;

  std::vector<int> child = a;
  size_t write = 0;
  for (int x : b) {
    if (!inSet[static_cast<size_t>(x)]) continue;
    while (write < child.size() && inSet[static_cast<size_t>(child[write])]) ++write;
    if (write >= child.size()) break;
    child[write++] = x;
  }
  return child;
}

// ---------------------------------------------------------------------------
// 单岛演化
// ---------------------------------------------------------------------------
/// 一个岛的演化结果。
struct IslandState {
  Ind best;              ///< 本岛历史最优个体
  long long evals = 0;   ///< 解码评价次数 (搜索量指标)
  int generations = 0;   ///< 完成的代数
  bool hitTimeLimit = false;  ///< 是否因时间预算退出
};

/// 单岛主循环: 初始化种群 -> 反复 (精英保留 + 锦标赛选择 + POX/均匀交叉 + 变异)。
/// 初始化阶段与每代之间都检查 deadline; 停滞超过 stagnationLimit 提前收敛。
/// @param p           问题实例
/// @param cfg         GA 参数
/// @param rng         本岛独立随机源 (可复现)
/// @param deadlineMs  相对 t0 的演化截止毫秒数 (不含局部搜索阶段)
/// @param t0          计时起点 (与 solveWith 共用, 便于全局对表)
/// @param seedSol     非空时作为 0 号个体注入 (规则解种子)
/// @param seedWithRule true 时 1 号个体注入 WSPT 规则解
/// @return            本岛演化结果
IslandState runIsland(const Problem& p, const GaConfig& cfg, Rng& rng, double deadlineMs,
                      Clock::time_point t0, const Solution* seedSol, bool seedWithRule) {
  Decoder decoder(p);
  IslandState st;
  const int nOps = p.numOps();
  int popSize = std::max(6, cfg.population);
  int elite = std::min(std::max(1, cfg.elite), popSize / 2);

  auto evaluate = [&](Ind& ind) {
    Solution s = decoder.decodeOs(ind.os, ind.ms, /*chooseBestMachine=*/false,
                                  /*buildAssign=*/false);
    ind.cost = s.kpi.objective;
    ++st.evals;
  };

  std::vector<Ind> pop;
  pop.reserve(static_cast<size_t>(popSize));
  for (int i = 0; i < popSize; ++i) {
    if (i == 0 && seedSol != nullptr && !seedSol->assign.empty()) {
      pop.push_back(makeSeededFromSolution(p, *seedSol));  // 起点 = 规则解
    } else if (seedWithRule && i == 1) {
      pop.push_back(makeSeeded(p, "WSPT"));
    } else {
      pop.push_back(makeRandom(p, rng, false));
    }
    evaluate(pop.back());
    // 初始化本身就要跑 popSize 次解码, 大算例能吃掉半个预算 —— 到点就少放几只
    if (static_cast<int>(pop.size()) >= 6 && msSince(t0) > deadlineMs * 0.6) break;
  }
  popSize = static_cast<int>(pop.size());
  elite = std::min(elite, std::max(1, popSize / 2));
  std::sort(pop.begin(), pop.end(), [](const Ind& x, const Ind& y) { return x.cost < y.cost; });
  st.best = pop.front();

  int lastImprove = 0;
  double mutRate = cfg.mutationRate;

  for (int gen = 0; gen < cfg.maxGenerations; ++gen) {
    if (msSince(t0) > deadlineMs) {
      st.hitTimeLimit = true;
      break;
    }
    st.generations = gen + 1;

    std::vector<Ind> next;
    next.reserve(static_cast<size_t>(popSize));
    for (int e = 0; e < elite; ++e) next.push_back(pop[static_cast<size_t>(e)]);

    auto tournament = [&]() -> const Ind& {
      int bestIdx = rng.nextInt(popSize);
      for (int k = 1; k < cfg.tournament; ++k) {
        int idx = rng.nextInt(popSize);
        if (pop[static_cast<size_t>(idx)].cost < pop[static_cast<size_t>(bestIdx)].cost)
          bestIdx = idx;
      }
      return pop[static_cast<size_t>(bestIdx)];
    };

    while (static_cast<int>(next.size()) < popSize) {
      // 一轮代数 = popSize 次解码, 大算例可能上百毫秒; 到点就收工, 别让"最后一代"把
      // 时间预算撑爆 (实测 2400 工序下会超到 2 倍预算)。
      if (msSince(t0) > deadlineMs && static_cast<int>(next.size()) >= elite + 2) {
        st.hitTimeLimit = true;
        break;
      }
      const Ind& p1 = tournament();
      const Ind& p2 = tournament();
      Ind child;
      if (rng.chance(cfg.crossoverRate)) {
        child.os = pox(p1.os, p2.os, p.numOrders(), rng);
        child.ms.resize(static_cast<size_t>(nOps));
        for (int i = 0; i < nOps; ++i) {
          child.ms[static_cast<size_t>(i)] =
              rng.chance(0.5) ? p1.ms[static_cast<size_t>(i)] : p2.ms[static_cast<size_t>(i)];
        }
      } else {
        child = rng.chance(0.5) ? p1 : p2;
      }

      // ---- 变异 ----
      if (rng.chance(mutRate)) {
        int mode = rng.nextInt(2);
        if (mode == 0 && nOps > 1) {
          int i = rng.nextInt(nOps), j = rng.nextInt(nOps);
          if (i != j) std::swap(child.os[static_cast<size_t>(i)], child.os[static_cast<size_t>(j)]);
        } else if (nOps > 2) {
          int i = rng.nextInt(nOps), j = rng.nextInt(nOps);
          if (i != j) {
            int v = child.os[static_cast<size_t>(i)];
            child.os.erase(child.os.begin() + i);
            child.os.insert(child.os.begin() + j, v);
          }
        }
      }
      if (rng.chance(mutRate)) {
        for (int t = 0; t < 2; ++t) {
          int i = rng.nextInt(nOps);
          const OpInst& op = p.ops[static_cast<size_t>(i)];
          if (op.options.size() > 1)
            child.ms[static_cast<size_t>(i)] = rng.nextInt(static_cast<int>(op.options.size()));
        }
      }

      evaluate(child);
      next.push_back(std::move(child));
    }

    if (static_cast<int>(next.size()) < elite + 2) break;  // 这一代没做完, 保留上一代
    popSize = static_cast<int>(next.size());
    elite = std::min(elite, std::max(1, popSize / 2));
    pop.swap(next);
    std::sort(pop.begin(), pop.end(), [](const Ind& x, const Ind& y) { return x.cost < y.cost; });
    if (pop.front().cost < st.best.cost - 1e-9) {
      st.best = pop.front();
      lastImprove = gen;
      mutRate = cfg.mutationRate;
    } else if (gen - lastImprove > cfg.stagnationLimit / 3) {
      mutRate = std::min(0.6, mutRate * 1.05);  // 停滞 -> 提高变异, 跳出局部最优
    }
    if (gen - lastImprove > cfg.stagnationLimit) break;
  }
  return st;
}

// ---------------------------------------------------------------------------
// 局部搜索: 关键工序重定位
//   1) 逐工序尝试换设备 (MS 维度的直接下降)
//   2) 随机把工序前移 (OS 维度, 缩短关键路径)
//   只做"接受改进"的爬山, 不做模拟退火 —— 时间预算有限, 简单有效。
/// 关键工序局部搜索 (在 solveWith 的演化之后对全局最优个体执行)。
/// 搜索中途若代价超过进入时的快照, 回退到快照 —— 保证局部搜索不劣化。
/// @param p          问题实例
/// @param ind        待改进的个体 (原地修改; bestCostSnapshot 须已设置为当前代价)
/// @param rng        随机源
/// @param deadlineMs 相对 t0 的截止毫秒数 (总预算的最后一段)
/// @param t0         计时起点
/// @param evals      累计评价次数 (进出各 +1)
// ---------------------------------------------------------------------------
void localSearch(const Problem& p, Ind& ind, Rng& rng, double deadlineMs, Clock::time_point t0,
                 long long& evals) {
  Decoder decoder(p);
  const int nOps = p.numOps();
  if (nOps == 0) return;

  auto eval = [&](Ind& x) {
    Solution s = decoder.decodeOs(x.os, x.ms, false, false);
    x.cost = s.kpi.objective;
    ++evals;
  };

  int sweeps = 0;
  bool improved = true;
  while (improved && sweeps < 12) {
    improved = false;
    ++sweeps;

    // (1) 换设备
    for (int i = 0; i < nOps; ++i) {
      if (msSince(t0) > deadlineMs) return;
      const OpInst& op = p.ops[static_cast<size_t>(i)];
      if (op.options.size() < 2) continue;
      int orig = ind.ms[static_cast<size_t>(i)];
      for (size_t k = 0; k < op.options.size(); ++k) {
        if (static_cast<int>(k) == orig) continue;
        ind.ms[static_cast<size_t>(i)] = static_cast<int>(k);
        eval(ind);
        if (ind.cost < ind.bestCostSnapshot - 1e-9) {
          ind.bestCostSnapshot = ind.cost;
          improved = true;
          break;
        }
        ind.ms[static_cast<size_t>(i)] = orig;
        ind.cost = ind.bestCostSnapshot;
      }
    }

    // (2) 工序前移 (随机采样, 避免 O(n^2))
    int samples = std::min(120, nOps);
    for (int s = 0; s < samples; ++s) {
      if (msSince(t0) > deadlineMs) return;
      int from = rng.nextInt(nOps);
      int to = rng.nextInt(nOps);
      if (from <= to) continue;
      int v = ind.os[static_cast<size_t>(from)];
      ind.os.erase(ind.os.begin() + from);
      ind.os.insert(ind.os.begin() + to, v);
      eval(ind);
      if (ind.cost < ind.bestCostSnapshot - 1e-9) {
        ind.bestCostSnapshot = ind.cost;
        improved = true;
      } else {
        ind.os.erase(ind.os.begin() + to);
        ind.os.insert(ind.os.begin() + from, v);
        ind.cost = ind.bestCostSnapshot;
      }
    }
  }
}

}  // namespace

/// GA 完整求解流程 (见头文件注释)。
/// 步骤: 规则解种子 (AUTO, 超大算例退化为 EDD) -> 扣除种子耗时 -> 多岛并行演化
///      -> 取全局最优 -> 关键工序局部搜索 -> 完整解码 + 全量 KPI 收尾。
Solution GaScheduler::solveWith(const Problem& p, const GaConfig& cfg, double timeLimitMs,
                                std::uint64_t seed, bool detailed) {
  const auto t0 = Clock::now();
  const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  int islands = cfg.islands > 0 ? cfg.islands : std::min(4, hw);

  double totalMs = std::max(50.0, timeLimitMs);
  double lsRatio = cfg.localSearch ? std::min(0.4, std::max(0.0, cfg.localSearchRatio)) : 0.0;

  // 先用规则法快速拿一个"可行且设备选得合理"的解当种子。
  // 用 AUTO (7 条规则全试取最优) 而不是单条 EDD: 这样 GA 的起点 == 前端看到的规则基准,
  // "GA 相对规则法的提升" 才是可比的; 超大算例只跑 EDD, 避免种子本身吃掉预算。
  Solution seedSol;
  {
    SolveOptions ro;
    ro.rule = p.numOps() <= 3000 ? "AUTO" : "EDD";
    ro.detailedKpi = false;
    RuleScheduler rs;
    seedSol = rs.solve(p, ro);
  }

  // 种子解构造也花时间 (超大算例 AUTO 7 条规则要几百毫秒), 必须从总预算里扣掉,
  // 否则 "timeLimitMs=800" 实际跑 2 秒, 对外承诺的时限就是假的。
  const double seedMs = msSince(t0);
  totalMs = std::max(20.0, totalMs - seedMs);
  const double gaMs = totalMs * (1.0 - lsRatio);

  std::vector<IslandState> results(static_cast<size_t>(islands));
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(islands));

  for (int i = 0; i < islands; ++i) {
    threads.emplace_back([&, i]() {
      Rng rng(seed + static_cast<std::uint64_t>(i) * 1000003ULL + 0x9E3779B9ULL);
      results[static_cast<size_t>(i)] =
          runIsland(p, cfg, rng, gaMs, t0, i == 0 ? &seedSol : nullptr, i == 1);
    });
  }
  for (auto& t : threads) t.join();

  Ind best;
  best.cost = 1e300;
  long long evals = 0;
  int generations = 0;
  for (const auto& r : results) {
    evals += r.evals;
    generations = std::max(generations, r.generations);
    if (r.best.cost < best.cost) best = r.best;
  }

  if (cfg.localSearch && p.numOps() > 0) {
    best.bestCostSnapshot = best.cost;
    Rng rng(seed ^ 0xDEADBEEFULL);
    localSearch(p, best, rng, totalMs, t0, evals);
  }

  // 用完整解码 + 全量 KPI 收尾
  Decoder decoder(p);
  Solution sol = decoder.decodeOs(best.os, best.ms, false, "ga");
  Metrics m(p);
  sol.kpi = m.compute(sol.assign, detailed);
  sol.kpi.solveMs = msSince(t0);
  sol.kpi.evaluations = evals;
  sol.feasible = !sol.truncated;
  if (sol.truncated) sol.infeasibleReason = "部分工序排不进排产窗口 (horizon)";
  (void)generations;
  return sol;
}

/// IScheduler 接口实现: 按规模自适应种群后转 solveWith。
/// @param p   问题实例
/// @param opt 时间预算/种子/线程数 (>0 时覆盖岛数)
/// @return 最优解
Solution GaScheduler::solve(const Problem& p, const SolveOptions& opt) {
  GaConfig cfg;
  int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  cfg.islands = opt.threads > 0 ? opt.threads : std::min(4, hw);
  // 规模大时适当缩小种群, 保证代数够用
  if (p.numOps() > 1200) cfg.population = 28;
  else if (p.numOps() > 600) cfg.population = 34;
  return solveWith(p, cfg, opt.timeLimitMs, opt.seed, opt.detailedKpi);
}

}  // namespace aps

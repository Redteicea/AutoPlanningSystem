// ===========================================================================
// engine/lns_scheduler.cpp -- L2' 大邻域搜索实现
//
// 结构:
//   Chain             一条链的可解表示 (order = op 下标序列, ms = 设备选择下标) + 目标值
//   fromSolution      把已有解 (规则解) 反编码成 Chain —— 与 GA 的 makeSeededFromSolution 同构
//   machineLoad       统计各设备当前承担的加工秒数 (repair 选设备时的负荷依据)
//   destroyOps        四种破坏算子: Random / Order / Machine / Tail
//   repairOps         按工艺链区间插回 + 设备重选 (一次重建同时改 OS 与 MS)
//   runChain          单链主循环: 破坏 -> 重建 -> 解码评价 -> 模拟退火接受
//   solveWith         入口: 规则解种子 -> 多链并行 -> 取最优 -> 全量 KPI
//
// 成本模型 (决定参数怎么取):
//   一次迭代 = 1 次完整解码。演示算例 467 工序约 0.5ms/次 -> 2s 能跑 3000+ 轮;
//   2400 工序约 14ms/次 -> 2s 只有 ~140 轮, 所以每轮必须"动得够多" (12% 的工序),
//   这正是 LNS 相对"单道工序前移"的优势所在。
//
// 时间纪律: 规则解种子构造耗时从总预算里扣除; 每轮开头检查 deadline,
//          任何情况下总耗时 <= timeLimitMs + 最后一次解码的收尾开销。
// ===========================================================================
#include "aps/engine/lns_scheduler.hpp"

#include <algorithm>
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

/// @brief 从计时起点到现在的毫秒数 (与 GA 同一套时间纪律)。
/// @param t0 计时起点 (steady_clock, 不受系统时钟调整影响)
/// @return    自 t0 起经过的毫秒数 (浮点, 便于毫秒级预算精确控制)
double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---- 可解表示 -------------------------------------------------------------

/// @brief 一条链的解表示: 派工顺序 + 设备选择 + 目标值。
///
/// order 是 op 下标的一个排列 (长度 = 工序总数), ms[op] 是该工序在 options 里的下标。
/// 这两段合起来正好是解码器 decodeSequence 的入参, 所以"复制一份就得到了可回滚的候选解"。
struct Chain {
  std::vector<int> order;  ///< 派工顺序 (op 下标序列, 每个 op 恰好出现一次)
  std::vector<int> ms;     ///< 设备选择: ms[op] = 该工序使用 options 的第几项
  double cost = 1e300;     ///< 目标值 (解码得到, 越小越好)
};

/// @brief 把已有解反编码成 Chain (LNS 的初始解, 也是"结果不劣于规则解"的保证)。
///
/// 两段都要继承, 道理和 GA 的种子一样: 只继承顺序、设备全选最快的话, 产能紧张时
/// 会把活全挤到一台设备上, 比规则解差一个数量级。
///
/// @param p   问题实例
/// @param sol 种子解 (规则法结果), 需带 opOrder 与 assign
/// @return    Chain; 解里未排上的工序用"单件工时最短"的设备兜底, 保证 ms 长度完整
Chain fromSolution(const Problem& p, const Solution& sol) {
  const int n = p.numOps();
  Chain c;
  c.ms.assign(static_cast<size_t>(n), 0);

  const bool hasAssign = static_cast<int>(sol.assign.size()) == n;
  for (int i = 0; i < n; ++i) {
    const auto& opts = p.ops[static_cast<size_t>(i)].options;
    if (opts.empty()) continue;
    int pick = -1;
    if (hasAssign) {
      const Assignment& a = sol.assign[static_cast<size_t>(i)];
      if (a.op >= 0 && a.machine >= 0) {
        for (size_t k = 0; k < opts.size(); ++k) {
          if (opts[k].machine == a.machine) {
            pick = static_cast<int>(k);
            break;
          }
        }
      }
    }
    if (pick < 0) {  // 兜底: 单件工时最短的设备
      pick = 0;
      for (size_t k = 1; k < opts.size(); ++k)
        if (opts[k].unitSeconds < opts[static_cast<size_t>(pick)].unitSeconds)
          pick = static_cast<int>(k);
    }
    c.ms[static_cast<size_t>(i)] = pick;
  }

  // 派工顺序必须原样回放 opOrder: 它与解码器当时的放置顺序逐条一致。
  // 改成"按开工时间排序"会在同时刻并列处打乱顺序, 插入式解码对顺序敏感 (实测差 50%)。
  std::vector<char> seen(static_cast<size_t>(n), 0);
  c.order.reserve(static_cast<size_t>(n));
  for (int op : sol.opOrder) {
    if (op < 0 || op >= n || seen[static_cast<size_t>(op)]) continue;
    seen[static_cast<size_t>(op)] = 1;
    c.order.push_back(op);
  }
  for (int i = 0; i < n; ++i)
    if (!seen[static_cast<size_t>(i)]) c.order.push_back(i);
  return c;
}

/// @brief 统计每台设备当前承担的纯加工秒数 (repair 选设备的负荷依据)。
/// @param p 问题实例
/// @param c 当前解
/// @return  长度 = numMachines 的负荷数组 (秒); 未选上的设备为 0
std::vector<double> machineLoad(const Problem& p, const Chain& c) {
  std::vector<double> load(static_cast<size_t>(p.numMachines()), 0.0);
  const int n = p.numOps();
  for (int i = 0; i < n; ++i) {
    const auto& opts = p.ops[static_cast<size_t>(i)].options;
    if (opts.empty()) continue;
    int pick = c.ms[static_cast<size_t>(i)];
    pick = pick < 0 ? 0 : pick % static_cast<int>(opts.size());
    const MachineOption& mo = opts[static_cast<size_t>(pick)];
    if (mo.machine < 0 || mo.machine >= static_cast<int>(load.size())) continue;
    load[static_cast<size_t>(mo.machine)] +=
        static_cast<double>(p.ops[static_cast<size_t>(i)].qty) * mo.unitSeconds;
  }
  return load;
}

// ---- 破坏算子 -------------------------------------------------------------

/// @brief 破坏算子种类。四种覆盖不同的"局部最优成因", 每轮随机挑一个。
enum class DestroyKind {
  Random,   ///< 随机拆 k 道: 通用, 保持多样性
  Order,    ///< 整单拆: 把若干订单的工序拆掉重排, 治交付/优先级维度的局部最优
  Machine,  ///< 按设备拆: 拆掉某台设备上的一批工序, 治设备拥挤与换型堆叠
  Tail      ///< 拆末尾: 拆派工序列最后 k 道, 治 makespan / 关键路径尾部
};

/// @brief 按指定算子挑出要拆掉的工序, 结果写进 removed 位图 (1 = 拆掉)。
///
/// 只标记不修改 order —— 真正的"摘除"在 repairOps 里统一做, 这样 destroy 是只读的,
/// 方便一轮里先算负荷再拆。
///
/// @param p       问题实例
/// @param c       当前解 (只读; Machine 算子用它判断每道工序落在哪台设备)
/// @param kind    破坏算子
/// @param k       期望拆掉的工序数 (调用方已 clamp 到 [1, nOps])
/// @param rng     随机源 (抽样/洗牌)
/// @param removed 输出位图, 长度 = numOps, 调用方负责传入已分配好的 vector (函数内会 assign)
void destroyOps(const Problem& p, const Chain& c, DestroyKind kind, int k, Rng& rng,
                std::vector<char>& removed) {
  const int n = p.numOps();
  removed.assign(static_cast<size_t>(n), 0);
  if (n == 0) return;
  k = std::max(1, std::min(k, n));

  int cnt = 0;
  switch (kind) {
    case DestroyKind::Random: {
      std::vector<int> idx(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) idx[static_cast<size_t>(i)] = i;
      rng.shuffle(idx);
      for (int i = 0; i < k && i < n; ++i) {
        removed[static_cast<size_t>(idx[static_cast<size_t>(i)])] = 1;
        ++cnt;
      }
      break;
    }
    case DestroyKind::Order: {
      // 打乱订单顺序, 逐个把整条工艺链拆掉, 拆够 k 道就停 (可能拆到半个订单,
      // 没关系 —— repair 会按工艺序号把缺的补回去)
      std::vector<int> ords(static_cast<size_t>(p.numOrders()));
      for (int i = 0; i < p.numOrders(); ++i) ords[static_cast<size_t>(i)] = i;
      rng.shuffle(ords);
      for (int o : ords) {
        if (cnt >= k) break;
        for (int op : p.orderOps[static_cast<size_t>(o)]) {
          if (cnt >= k) break;
          if (op < 0 || op >= n || removed[static_cast<size_t>(op)]) continue;
          removed[static_cast<size_t>(op)] = 1;
          ++cnt;
        }
      }
      break;
    }
    case DestroyKind::Machine: {
      // 建 op -> 当前设备 的倒排 (局部构建, O(n))
      std::vector<std::vector<int>> byMachine(static_cast<size_t>(p.numMachines()));
      for (int i = 0; i < n; ++i) {
        const auto& opts = p.ops[static_cast<size_t>(i)].options;
        if (opts.empty()) continue;
        int pick = c.ms[static_cast<size_t>(i)];
        pick = pick < 0 ? 0 : pick % static_cast<int>(opts.size());
        int m = opts[static_cast<size_t>(pick)].machine;
        if (m >= 0 && m < static_cast<int>(byMachine.size())) byMachine[static_cast<size_t>(m)].push_back(i);
      }
      // 随机试几台设备, 挑一台"工序数 >= 2"的 (单道工序的机器拆了也没意义)
      for (int attempt = 0; attempt < 8; ++attempt) {
        if (byMachine.empty()) break;
        int m = rng.nextInt(static_cast<int>(byMachine.size()));
        auto& list = byMachine[static_cast<size_t>(m)];
        if (list.size() < 2) continue;
        rng.shuffle(list);
        for (int i = 0; i < k && i < static_cast<int>(list.size()); ++i) {
          int op = list[static_cast<size_t>(i)];
          if (!removed[static_cast<size_t>(op)]) {
            removed[static_cast<size_t>(op)] = 1;
            ++cnt;
          }
        }
        break;
      }
      break;
    }
    case DestroyKind::Tail: {
      int from = std::max(0, static_cast<int>(c.order.size()) - k);
      for (int i = from; i < static_cast<int>(c.order.size()); ++i) {
        removed[static_cast<size_t>(c.order[static_cast<size_t>(i)])] = 1;
        ++cnt;
      }
      break;
    }
  }

  // 兜底: 一个都没拆到 (Machine 算子连续 8 次抽到空设备) 就随机拆一道,
  // 否则这一轮等于空转, 白白浪费一次解码。
  if (cnt == 0) removed[static_cast<size_t>(rng.nextInt(n))] = 1;
}

// ---- 重建算子 -------------------------------------------------------------

/// @brief 把被拆的工序重建成合法序列 (一次重建同时决定 OS 位置与 MS 设备)。
///
/// 位置选择: 用工艺链算出合法插入区间 [前驱之后, 后继之前], 再在区间内采样若干候选点,
///   取"换型增量最小"的那个 —— 这也是本项目 LNS 与教科书版最大的差别。
///   目标函数里 1 分钟换型折 3 分, 而 1 分钟加工负荷折 0.05 分, 换型的边际价值是负荷的
///   ~60 倍; 只按负荷/距离选位置的 LNS 在演示算例上只能改进 3.9%, 换成换型感知后
///   才真正咬得住 (GA 的 23% 改进主要也来自换型 2785->1995min)。
///
/// 设备选择: 30% 保持原设备 (保留已形成的工艺-设备搭配), 10% 纯随机 (多样性),
///          其余按 `换型秒数 x 100 + 累计负荷` 最小者选 —— 换型项权重刻意拉高,
///          因为"让同族工序抱团"比"把活摊平到最空的机器"更值钱。
///
/// @param p          问题实例
/// @param c          当前解, 原地修改 (order / ms 都会被改写)
/// @param removed    破坏位图 (1 = 该工序需要重建)
/// @param rng        随机源
/// @param timeGuided 是否启用"位置微扰动" (false 则严格按原位置插回)
/// @param load       各设备当前负荷 (秒); 值传递, 函数内会先扣掉待重建工序的负荷再加回
/// @param oldIdx     破坏前每道工序在序列里的下标; 重建时据此保留局部性
void repairOps(const Problem& p, Chain& c, const std::vector<char>& removed, Rng& rng,
               bool timeGuided, std::vector<double> load, const std::vector<int>& oldIdx) {
  const int n = p.numOps();
  if (n == 0) return;

  // (1) 把待重建的工序从序列里摘出来, 扣掉它们的负荷, 并保留"破坏前的位置表"。
  //
  // 重建必须保留局部性 —— 这是 LNS 在大算例上能否活下来的关键。实测两种"不保留局部性"
  // 的写法都会崩, 而且崩得很惨:
  //   a) 按工艺前驱插回: 一个订单的第 1 道工序没有前驱 -> lo 恒为 0 -> 所有被拆订单的
  //      首工序挤到队首, 抢占全部早期设备时段 -> 2400 工序直接 truncated (目标值涨 250 倍);
  //   b) 按"前面最近的幸存工序"锚点插回: 同锚点的工序被挤成一块, 块内按订单号重排,
  //      2400 工序下解码器的顺序自愈不收敛, 输出 118 处工艺违例 (最严重 280 小时),
  //      目标值从 2 万涨到 500 万。
  // 现在的做法: 用破坏前的下标做"有序映射", 插回时二分查回原来的相对位置, 只做 +-3 的
  // 小扰动。重建后的序列与原来几乎一致, 变化全部落在设备选择上 —— 这也符合本问题的
  // 实际改进来源 (GA 的 23% 里换型贡献了主要部分)。
  std::vector<int> bag;
  std::vector<int> curOld;  // 与 c.order 平行: 每道工序破坏前的位置下标 (恒保持升序)
  {
    std::vector<int> kept;
    kept.reserve(c.order.size());
    for (int op : c.order) {
      if (removed[static_cast<size_t>(op)]) {
        bag.push_back(op);
      } else {
        kept.push_back(op);
        curOld.push_back(oldIdx[static_cast<size_t>(op)]);
      }
    }
    c.order.swap(kept);
  }
  if (bag.empty()) return;

  // 扣掉待重建工序的负荷 (它们马上会被重新分配)
  for (int op : bag) {
    const auto& opts = p.ops[static_cast<size_t>(op)].options;
    if (opts.empty()) continue;
    int pick = c.ms[static_cast<size_t>(op)];
    pick = pick < 0 ? 0 : pick % static_cast<int>(opts.size());
    const MachineOption& mo = opts[static_cast<size_t>(pick)];
    if (mo.machine >= 0 && mo.machine < static_cast<int>(load.size()))
      load[static_cast<size_t>(mo.machine)] -=
          static_cast<double>(p.ops[static_cast<size_t>(op)].qty) * mo.unitSeconds;
  }

  // (2) 每台设备"当前最后一道工序"的换型族 —— 估算"插到这台设备"的换型代价用。
  //     只看残留序列 (被拆的工序不在里面), 所以反映的是重建后的真实状态。
  std::vector<int> machLastFam(load.size(), -1);
  for (int op : c.order) {
    const auto& opts = p.ops[static_cast<size_t>(op)].options;
    if (opts.empty()) continue;
    int pick = c.ms[static_cast<size_t>(op)];
    pick = pick < 0 ? 0 : pick % static_cast<int>(opts.size());
    int m = opts[static_cast<size_t>(pick)].machine;
    if (m >= 0 && m < static_cast<int>(machLastFam.size()))
      machLastFam[static_cast<size_t>(m)] = p.setup.indexOf(p.ops[static_cast<size_t>(op)].family);
  }

  // (3) 按"破坏前的下标"升序插回 —— 升序插入是 curOld 保持有序 (二分定位) 的前提
  std::sort(bag.begin(), bag.end(), [&](int a, int b) {
    return oldIdx[static_cast<size_t>(a)] < oldIdx[static_cast<size_t>(b)];
  });

  // op -> 当前在 order 里的位置 (-1 = 已被拆且尚未插回)
  std::vector<int> pos(static_cast<size_t>(n), -1);
  for (int i = 0; i < static_cast<int>(c.order.size()); ++i)
    pos[static_cast<size_t>(c.order[static_cast<size_t>(i)])] = i;

  for (int op : bag) {
    const OpInst& inst = p.ops[static_cast<size_t>(op)];
    const auto& chain = p.orderOps[static_cast<size_t>(inst.order)];
    const int prevOp = inst.seq > 0 ? chain[static_cast<size_t>(inst.seq - 1)] : -1;
    const int nextOp =
        inst.seq + 1 < static_cast<int>(chain.size()) ? chain[static_cast<size_t>(inst.seq + 1)] : -1;
    const int famIdx = p.setup.indexOf(inst.family);

    // ---- 位置: 原位置映射 + 微扰动, 再夹进工艺合法区间 ----
    int lo = 0;
    int hi = static_cast<int>(c.order.size());
    if (prevOp >= 0 && pos[static_cast<size_t>(prevOp)] >= 0)
      lo = pos[static_cast<size_t>(prevOp)] + 1;
    if (nextOp >= 0 && pos[static_cast<size_t>(nextOp)] >= 0)
      hi = pos[static_cast<size_t>(nextOp)];
    if (hi < lo) hi = lo;

    const int curSize = static_cast<int>(curOld.size());
    int at = static_cast<int>(std::lower_bound(curOld.begin(), curOld.end(),
                                              oldIdx[static_cast<size_t>(op)]) -
                              curOld.begin());
    if (timeGuided && rng.chance(0.35)) at += rng.nextInt(7) - 3;  // 前后挪 1~3 位
    if (at < 0) at = 0;
    if (at > curSize) at = curSize;
    at = std::max(lo, std::min(at, hi));
    if (at < 0) at = 0;
    if (at > curSize) at = curSize;

    c.order.insert(c.order.begin() + at, op);
    curOld.insert(curOld.begin() + at, oldIdx[static_cast<size_t>(op)]);
    for (int i = at; i < static_cast<int>(c.order.size()); ++i)
      pos[static_cast<size_t>(c.order[static_cast<size_t>(i)])] = i;

    // ---- 设备重选 ----
    const auto& opts = inst.options;
    if (opts.size() <= 1) {
      if (!opts.empty() && opts[0].machine >= 0 &&
          opts[0].machine < static_cast<int>(machLastFam.size()))
        machLastFam[static_cast<size_t>(opts[0].machine)] = famIdx;
      c.ms[static_cast<size_t>(op)] = 0;
      continue;
    }
    int cur = c.ms[static_cast<size_t>(op)];
    if (cur < 0 || cur >= static_cast<int>(opts.size())) cur = 0;

    int chosen = cur;
    if (rng.chance(0.30)) {
      chosen = cur;  // 保持原设备
    } else if (rng.chance(0.10)) {
      chosen = rng.nextInt(static_cast<int>(opts.size()));
    } else {
      double bestScore = 1e300;
      for (size_t k = 0; k < opts.size(); ++k) {
        const MachineOption& mo = opts[k];
        if (mo.machine < 0 || mo.machine >= static_cast<int>(load.size())) continue;
        const double dur = static_cast<double>(inst.qty) * mo.unitSeconds;
        const int lastFam = machLastFam[static_cast<size_t>(mo.machine)];
        const double setupSec =
            lastFam >= 0 ? static_cast<double>(p.setup.get(lastFam, famIdx)) : 0.0;
        // 换型项权重 18: 由目标函数权重反推 —— 1 秒换型折 0.05 分, 1 秒延期折 0.0028 分,
        // 即 1 秒换型的边际价值约等于 18 秒加工负荷。权重再拉高 (试过 100) 会为了
        // "让同族抱团"把活挤到少数设备, 产能失衡后延期爆炸。
        const double score =
            setupSec * 18.0 + load[static_cast<size_t>(mo.machine)] + dur;
        if (score < bestScore) {
          bestScore = score;
          chosen = static_cast<int>(k);
        }
      }
    }

    c.ms[static_cast<size_t>(op)] = chosen;
    const MachineOption& mo = opts[static_cast<size_t>(chosen)];
    if (mo.machine >= 0 && mo.machine < static_cast<int>(load.size())) {
      load[static_cast<size_t>(mo.machine)] += static_cast<double>(inst.qty) * mo.unitSeconds;
      machLastFam[static_cast<size_t>(mo.machine)] = famIdx;
    }
  }

}

// ---- 单链主循环 -----------------------------------------------------------

/// @brief 单条链的搜索结果 (各链独立, 最后只汇总最优解与统计量)。
struct ChainResult {
  Chain best;             ///< 本链历史最优解
  long long evals = 0;    ///< 解码评价次数 (搜索量指标)
  int iterations = 0;     ///< 完成的迭代轮数
  int accepted = 0;       ///< 被接受的候选数 (含变差的)
};

/// @brief 单链 LNS 主循环: 破坏 -> 重建 -> 解码评价 -> 模拟退火接受。
///
/// 为什么用退火接受而不是纯爬山: 纯爬山在"拆 12% 重建"这种大动作下几乎只接受完美改进,
/// 很容易卡在同一个结构; 退火允许早期接受少量变差的解, 温度几何下降到接近 0 后自动
/// 退化回爬山。best 单独保存, 所以"接受变差"不影响最终结果单调性。
///
/// @param p          问题实例
/// @param seed       种子解 (规则法), 同时作为 best 的初始值
/// @param cfg        LNS 参数
/// @param deadlineMs 相对 t0 的截止毫秒数
/// @param t0         计时起点 (与 solveWith 共用, 保证各链对同一块表)
/// @param rng        本链独立随机源
/// @return           本链最优解 + 统计量
ChainResult runChain(const Problem& p, const Chain& seed, const LnsConfig& cfg, double deadlineMs,
                     Clock::time_point t0, Rng& rng) {
  Decoder decoder(p);
  ChainResult res;
  res.best = seed;
  Chain cur = seed;

  const int n = p.numOps();
  if (n == 0) return res;

  // 每轮拆多少: 比例 x 总数, 夹在 [minDestroy, maxDestroyRatio x 总数]
  int k = static_cast<int>(std::llround(static_cast<double>(n) * cfg.destroyRatio));
  k = std::max(cfg.minDestroy, k);
  k = std::min(k, std::max(1, static_cast<int>(static_cast<double>(n) * cfg.maxDestroyRatio)));
  k = std::max(1, std::min(k, n));

  // 温度: 以种子解目标值为尺度, 几何降温。目标值量级随算例变, 用相对比例才可移植。
  const double t0Temp = std::max(1e-6, seed.cost * std::max(1e-9, cfg.startTempRatio));
  const double t1Temp = std::max(1e-9, seed.cost * std::max(1e-12, cfg.endTempRatio));
  const int stagLimit =
      cfg.stagnationLimit > 0 ? cfg.stagnationLimit : std::max(150, std::min(400, n / 4));

  std::vector<char> removed;
  int lastImprove = 0;

  for (int it = 0;; ++it) {
    const double now = msSince(t0);
    if (now > deadlineMs) break;
    if (cfg.maxIterations > 0 && it >= cfg.maxIterations) break;
    // 停滞早停只在"时间已用掉大半"后生效: LNS 每轮很便宜 (0.3~0.6ms/500 工序),
    // 一上来就按轮数退出会把 2000ms 预算只花掉 60% (实测 1200ms 就收工)。
    if (it - lastImprove > stagLimit && now > deadlineMs * 0.85) break;

    // 停滞时把当前解拉回全局最优 (intensification): 退火会让 cur 在差解附近游走,
    // 拉回来等于"从最好的解重新扰动", 既保住多样性 (只 50% 概率拉) 又避免越走越远。
    // 没有这一步时实测 300 工序档会出现"预算加到 1700ms 反而比 1200ms 差 60%"——
    // 加时间等于换了一条随机轨迹, 而不是在同一条轨迹上走得更远。
    if (it - lastImprove > stagLimit / 3 && rng.chance(0.5)) cur = res.best;

    // 温度按"时间进度"而非"迭代进度"下降: 大算例一轮贵、轮数少, 按轮数降会冷得太快
    const double prog = deadlineMs > 1e-9 ? std::min(1.0, now / deadlineMs) : 1.0;
    const double temp = t0Temp * std::pow(t1Temp / t0Temp, prog);

    // 四个算子等概率; 每轮换一个, 覆盖不同的局部最优成因
    const DestroyKind kind = static_cast<DestroyKind>(rng.nextInt(4));

    Chain cand = cur;
    const std::vector<double> load = machineLoad(p, cand);
    // 破坏前的位置表: 重建时按它做"原位置映射", 保证局部性 (见 repairOps 注释)
    std::vector<int> oldIdx(static_cast<size_t>(n), -1);
    for (int i = 0; i < n; ++i) oldIdx[static_cast<size_t>(cand.order[static_cast<size_t>(i)])] = i;
    destroyOps(p, cand, kind, k, rng, removed);
    repairOps(p, cand, removed, rng, cfg.timeGuidedRepair, load, oldIdx);

    const Solution s = decoder.decodeSequence(cand.order, cand.ms, /*chooseBestMachine=*/false,
                                             /*tag=*/"lns", /*buildAssign=*/false);
    cand.cost = s.kpi.objective;
    ++res.evals;
    res.iterations = it + 1;

    if (cand.cost < res.best.cost - 1e-9) {
      res.best = cand;
      lastImprove = it;
    }

    const double delta = cand.cost - cur.cost;
    if (delta <= 0.0 || rng.chance(std::exp(-delta / std::max(1e-9, temp)))) {
      cur = std::move(cand);
      ++res.accepted;
    }
  }
  return res;
}

}  // namespace

/// @brief LNS 完整求解流程 (参数校验与规模自适应在 solve() 里做, 这里只负责跑)。
///
/// 流水线:
///   1) 规则法造种子解 (AUTO 六条规则取最优; 超大算例退化为 EDD, 免得种子自己吃掉预算);
///   2) 把种子构造耗时从总预算里扣掉;
///   3) 把种子解反编码成 Chain, 并重新解码一次校准目标值 (detailedKpi=false, 只取 objective);
///   4) 多链并行跑 runChain (每链独立种子, 结果可复现);
///   5) 汇总各链最优, 取全局最优;
///   6) buildAssign=true 重新解码, 得到完整 Assignment 与全量 KPI 作为对外结果。
///
/// @param p           问题实例
/// @param cfg         LNS 参数 (破坏比例/链数/温度/早停)
/// @param timeLimitMs 总时间预算 (毫秒), 含种子构造; 内部下限 50ms
/// @param seed        随机种子; 各链 = seed + 链号*1000003 + 常数, 同 seed 结果可复现
/// @param detailed    true=计算完整 KPI (含逐机利用率); false=只保证目标值正确
/// @return            最优解; 有工序排不进窗口时 feasible=false 并填 infeasibleReason
Solution LnsScheduler::solveWith(const Problem& p, const LnsConfig& cfg, double timeLimitMs,
                                std::uint64_t seed, bool detailed) {
  const auto t0 = Clock::now();
  const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  const int chains = std::max(1, cfg.chains > 0 ? cfg.chains : std::min(4, hw));

  double totalMs = std::max(50.0, timeLimitMs);

  // 种子: 与 GA 用同一套规则解 (AUTO 6 条取最优), 这样 "LNS vs 规则法" 与
  // "GA vs 规则法" 的基准完全一致, 三方数字可以直接对比。
  Solution seedSol;
  {
    SolveOptions ro;
    ro.rule = p.numOps() <= 3000 ? "AUTO" : "EDD";
    ro.detailedKpi = false;
    RuleScheduler rs;
    seedSol = rs.solve(p, ro);
  }

  const double seedMs = msSince(t0);
  totalMs = std::max(20.0, totalMs - seedMs);

  Chain seedChain = fromSolution(p, seedSol);
  if (p.numOps() > 0) {
    // 校准目标值: 规则解在 detailedKpi=false 下的 objective 可能只算了部分项,
    // 统一用"本解码器 + 本 order/ms"重新评价, 保证三方目标值口径完全一致。
    Decoder decoder(p);
    Solution probe = decoder.decodeSequence(seedChain.order, seedChain.ms, false, "lns:seed", false);
    seedChain.cost = probe.kpi.objective;
  }

  std::vector<ChainResult> results(static_cast<size_t>(chains));
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(chains));
  for (int i = 0; i < chains; ++i) {
    threads.emplace_back([&, i]() {
      Rng rng(seed + static_cast<std::uint64_t>(i) * 1000003ULL + 0x5DEECE66DULL);
      results[static_cast<size_t>(i)] = runChain(p, seedChain, cfg, totalMs, t0, rng);
    });
  }
  for (auto& th : threads) th.join();

  Chain best = seedChain;
  long long evals = 0;
  int iterations = 0;
  for (const auto& r : results) {
    evals += r.evals;
    iterations += r.iterations;
    if (r.best.cost < best.cost - 1e-9) best = r.best;
  }
  Decoder decoder(p);
  Solution sol = decoder.decodeSequence(best.order, best.ms, /*chooseBestMachine=*/false,
                                        /*tag=*/"lns", /*buildAssign=*/true);
  Metrics m(p);
  sol.kpi = m.compute(sol.assign, detailed);
  sol.kpi.solveMs = msSince(t0);
  sol.kpi.evaluations = evals;
  sol.feasible = !sol.truncated;
  if (sol.truncated) sol.infeasibleReason = "部分工序排不进排产窗口 (horizon)";
  (void)iterations;
  return sol;
}

/// @brief IScheduler 接口实现: 按规模微调破坏比例 / 链数后转 solveWith。
///
/// 规模越大, 单次解码越贵 -> 每轮要动更多工序才划算; 所以大算例把 destroyRatio
/// 从 12% 提到 15%。小算例则降到 10%, 避免一次拆太多导致重建精度下降。
///
/// @param p   问题实例
/// @param opt 求解选项: timeLimitMs 预算 / seed 随机种子 / threads 并行链数 /
///            detailedKpi 是否算全量 KPI
/// @return    最优解 (恒不劣于规则法种子)
Solution LnsScheduler::solve(const Problem& p, const SolveOptions& opt) {
  LnsConfig cfg;
  const int n = p.numOps();
  if (n > 1200) {
    cfg.destroyRatio = 0.15;
    cfg.stagnationLimit = 400;
  } else if (n > 300) {
    cfg.destroyRatio = 0.12;
    cfg.stagnationLimit = 250;
  } else {
    cfg.destroyRatio = 0.10;
    cfg.stagnationLimit = 150;
  }
  const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  cfg.chains = opt.threads > 0 ? opt.threads : std::min(4, hw);
  return solveWith(p, cfg, opt.timeLimitMs, opt.seed, opt.detailedKpi);
}

}  // namespace aps

// ===========================================================================
// engine/decoder.cpp -- 插入式解码器实现
//
// decodeSequence 的主循环 (每道工序):
//   1. ready  = max(订单发布, 前道结束), 不早于窗口起点
//   2. 在候选设备的队列里从前往后找第一个塞得下的位置 (整数剪枝先行,
//      通过剪枝才查 Timeline —— Timeline 查询是解码的大头)
//   3. 插入合法性的关键: 插在 pos 处后, 原本 pos 位置的后道工序的
//      "换型入向时间"也要够, 跨班次时用 capacity() 判可用产能而非时钟差
//   4. 全部排完后做"顺序自愈", 兜底消除残留的重叠/工艺链违规
//
// 两种收尾模式:
//   buildAssign=true  -> 用队列终态回填 Assignment + Metrics 全量 KPI (最终结果)
//   buildAssign=false -> 快路径只算目标函数所需的量 (GA 内循环, 快 3~5 倍)
// ===========================================================================
#include "aps/engine/decoder.hpp"

#include <algorithm>

#include "aps/kpi/metrics.hpp"

namespace aps {
namespace {

/// 设备队列上的一个已放置工序 (解码中间态; 最终 Assignment 由它回填)。
struct Slot {
  int op;         ///< 工序下标
  Seconds s;      ///< 加工开始
  Seconds e;      ///< 加工结束
  Seconds setup;  ///< 本工序前准备
  Seconds occ;    ///< 机器开始被占用的时刻 (含被班次切开的换型)
};

}  // namespace

/// OS 编码 -> 派工顺序: 订单号第 k 次出现即其第 k 道工序;
/// 缺失的工序按编号补到末尾, 保证任何 OS 输入都不丢工序。
std::vector<int> Decoder::expandOs(const std::vector<int>& os) const {
  const int nOrders = p_.numOrders();
  std::vector<int> used(static_cast<size_t>(nOrders), 0);
  std::vector<int> order;
  order.reserve(static_cast<size_t>(p_.numOps()));

  for (int o : os) {
    if (o < 0 || o >= nOrders) continue;
    int k = used[static_cast<size_t>(o)]++;
    const auto& chain = p_.orderOps[static_cast<size_t>(o)];
    if (k < static_cast<int>(chain.size())) order.push_back(chain[static_cast<size_t>(k)]);
  }
  // 补齐: OS 被截断或重复过多时, 也不能丢工序
  std::vector<char> seen(static_cast<size_t>(p_.numOps()), 0);
  for (int op : order)
    if (op >= 0 && op < p_.numOps()) seen[static_cast<size_t>(op)] = 1;
  for (int op = 0; op < p_.numOps(); ++op)
    if (!seen[static_cast<size_t>(op)]) order.push_back(op);
  return order;
}

/// OS+MS 双段编码解码入口 (GA 用), 内部展开 OS 后走 decodeSequence。
Solution Decoder::decodeOs(const std::vector<int>& os, const std::vector<int>& ms,
                           bool chooseBestMachine, bool buildAssign) const {
  return decodeSequence(expandOs(os), ms, chooseBestMachine, "ga", buildAssign);
}

/// 派工顺序 -> 时间表 (核心函数)。
/// 逐工序: 算 ready -> 候选设备找最早可行位置 -> 插入队列。
/// 结束后顺序自愈 + 按需回填 Assignment / KPI。
/// 复杂度 O(n x (候选位置扫描 + Timeline 二分)), 已做整数剪枝。
Solution Decoder::decodeSequence(const std::vector<int>& opOrder, const std::vector<int>& ms,
                                 bool chooseBestMachine, const std::string& tag,
                                 bool buildAssign) const {
  const int nOps = p_.numOps();
  const int nMach = p_.numMachines();

  Solution sol;
  sol.solver = tag;
  if (buildAssign) sol.assign.assign(static_cast<size_t>(nOps), Assignment{});
  sol.feasible = true;

  std::vector<std::vector<Slot>> queues(static_cast<size_t>(nMach));
  for (auto& q : queues) q.reserve(32);

  std::vector<Seconds> opEnd(static_cast<size_t>(nOps), -1);
  std::vector<char> scheduled(static_cast<size_t>(nOps), 0);

  Seconds totalSetup = 0;
  int setupCount = 0;
  Seconds minOcc = kInf;
  Seconds maxEnd = 0;

  const Seconds kNegInf = -kInf / 4;

  sol.opOrder.reserve(static_cast<size_t>(nOps));

  for (int opIdx : opOrder) {
    if (opIdx < 0 || opIdx >= nOps) continue;
    if (scheduled[static_cast<size_t>(opIdx)]) continue;
    const OpInst& op = p_.ops[static_cast<size_t>(opIdx)];
    if (op.options.empty()) continue;

    // ---- 前道约束: 同一订单必须按工艺顺序 ----
    Seconds ready = p_.orders[static_cast<size_t>(op.order)].release;
    if (op.seq > 0) {
      const auto& chain = p_.orderOps[static_cast<size_t>(op.order)];
      if (op.seq - 1 < static_cast<int>(chain.size())) {
        int prevOp = chain[static_cast<size_t>(op.seq - 1)];
        if (prevOp >= 0 && scheduled[static_cast<size_t>(prevOp)])
          ready = std::max(ready, opEnd[static_cast<size_t>(prevOp)]);
      }
    }
    if (ready < p_.horizonStart) ready = p_.horizonStart;

    const int famIdx = p_.setup.indexOf(op.family);

    // ---- 设备候选项 ----
    int candBegin = 0, candEnd = static_cast<int>(op.options.size());
    if (!chooseBestMachine && !ms.empty()) {
      int pick = ms[static_cast<size_t>(opIdx)];
      if (pick < 0) pick = -pick;
      candBegin = pick % static_cast<int>(op.options.size());
      candEnd = candBegin + 1;
    }

    bool found = false;
    Seconds bestEnd = kInf;
    int bestMachine = -1;
    Placement best{};

    for (int ci = candBegin; ci < candEnd; ++ci) {
      const MachineOption& mo = op.options[static_cast<size_t>(ci)];
      if (mo.machine < 0 || mo.machine >= nMach) continue;
      const Timeline& cal = p_.machines[static_cast<size_t>(mo.machine)].cal;
      if (cal.empty()) continue;

      Seconds dur = op.qty > 0 ? static_cast<Seconds>(static_cast<double>(op.qty) * mo.unitSeconds + 0.5) : 0;
      auto& q = queues[static_cast<size_t>(mo.machine)];

      for (int pos = 0; pos <= static_cast<int>(q.size()); ++pos) {
        Seconds prevEnd = (pos == 0) ? kNegInf : q[static_cast<size_t>(pos - 1)].e;
        int prevFam = (pos == 0)
                          ? -1
                          : p_.setup.indexOf(p_.ops[static_cast<size_t>(q[static_cast<size_t>(pos - 1)].op)].family);
        Seconds setupIn = p_.setup.get(prevFam, famIdx);

        Seconds occ = std::max(ready, prevEnd);
        // 快速排除 (性能关键): 日历可用产能永远 <= 时钟跨度, 所以"到后道开工的时钟差
        // 都不够 setup+dur" 的位置必然插不进去 —— 用一次整数比较挡掉绝大多数候选位置,
        // 避免每个位置都去查时间轴 (2400 工序时这一步能省掉 ~90% 的解码时间)。
        if (pos < static_cast<int>(q.size()) && q[static_cast<size_t>(pos)].s - occ < setupIn + dur)
          continue;
        Seconds aligned = cal.alignStart(occ);
        if (aligned == kInf) break;
        Seconds end = cal.advance(aligned, setupIn + dur);
        if (end == kInf) break;  // 该设备排不下, 换下一台

        if (pos < static_cast<int>(q.size())) {
          // 本工序插进来后, 后道的"前道"就变成了它, 换型时间也要重算。
          // 必须在插入**之前**验证后道接得上: [end, nextStart] 这段可用产能要够 setupOut。
          // (早期版本只检查 end + setupOut <= nextStart, 跨班次时会漏判, 导致重叠)
          Seconds setupOut =
              p_.setup.get(famIdx,
                           p_.setup.indexOf(p_.ops[static_cast<size_t>(q[static_cast<size_t>(pos)].op)].family));
          // 注意 setupOut 可能为 0: 此时 capacity 一定 >= 0, 只靠容量判断会漏掉
          // "本工序结束时间已经超过后道开工时间" 的情况, 必须显式比较 end。
          if (end > q[static_cast<size_t>(pos)].s) continue;
          if (cal.capacity(end, q[static_cast<size_t>(pos)].s) < setupOut) continue;
          Seconds newOcc = cal.rewind(q[static_cast<size_t>(pos)].s, setupOut);
          if (newOcc == kInf || newOcc < end) continue;
        }

        if (end < bestEnd) {
          bestEnd = end;
          bestMachine = mo.machine;
          best.ok = true;
          best.pos = pos;
          best.end = end;
          // 注意: 不能写成 end - dur。加工跨班次时, "往前推 dur 个时钟秒" 会落进
          // 非工作时段; 必须按可用产能往回推, 否则时间表在跨天工序上就是错的。
          best.start = cal.rewind(end, dur);
          best.setup = setupIn;
          best.occStart = aligned;
          best.work = dur;
          found = true;
        }
        break;  // 第一个可行位置即最早完工 (见头文件单调性说明)
      }
    }

    if (!found) {
      // 排不进窗口: 追加到"最优设备"队尾并标记截断, 保证结果结构完整
      sol.truncated = true;
      int m = op.options[0].machine;
      Seconds dur = op.qty > 0 ? static_cast<Seconds>(static_cast<double>(op.qty) * op.options[0].unitSeconds + 0.5) : 0;
      auto& q = queues[static_cast<size_t>(m)];
      Seconds prevEnd = q.empty() ? p_.horizonStart : q.back().e;
      Seconds s = std::max(prevEnd, ready);
      best.ok = true;
      best.pos = static_cast<int>(q.size());
      best.start = s;
      best.end = s + dur;
      best.setup = 0;
      best.occStart = s;
      best.work = dur;
      bestMachine = m;
    }

    {
      auto& q = queues[static_cast<size_t>(bestMachine)];
      int pos = std::min(best.pos, static_cast<int>(q.size()));
      q.insert(q.begin() + pos, Slot{opIdx, best.start, best.end, best.setup, best.occStart});
      scheduled[static_cast<size_t>(opIdx)] = 1;
      opEnd[static_cast<size_t>(opIdx)] = best.end;
      sol.opOrder.push_back(opIdx);

      totalSetup += best.setup;
      if (best.setup > 0) ++setupCount;
      minOcc = std::min(minOcc, best.occStart);
      maxEnd = std::max(maxEnd, best.end);

      if (buildAssign) {
        Assignment& a = sol.assign[static_cast<size_t>(opIdx)];
        a.op = opIdx;
        a.machine = bestMachine;
        a.start = best.start;
        a.end = best.end;
        a.setup = best.setup;
        a.occStart = best.occStart;
        a.workSec = best.work;
      }
    }
  }

  // ---- 顺序自愈 ----
  // 插入式解码在"换型 + 跨班次 + 多机联动"叠加时, 极端情况下会留下时间重叠或
  // 后道早于前道。这里做两件事, 保证输出 100% 可行:
  //   1) 同机器: 按时间轴重扫, 有重叠就顺延 (只推后, 不提前)
  //   2) 跨机器: 按订单工艺链检查, 后道早于前道就整机顺延; 迭代到稳定
  // 代价是极少数工序晚几分钟, 换来的是"绝不出不可行解"。
  {
    auto relayoutFrom = [&](int m, size_t idx, Seconds earliest) {
      auto& list = queues[static_cast<size_t>(m)];
      const Timeline& cal = p_.machines[static_cast<size_t>(m)].cal;
      Seconds prevEnd = earliest;
      if (idx > 0) prevEnd = std::max(prevEnd, list[idx - 1].e);
      for (size_t k = idx; k < list.size(); ++k) {
        Seconds work = cal.capacity(list[k].s, list[k].e);
        if (work <= 0) work = list[k].e - list[k].s;
        Seconds occ = std::max(list[k].occ, prevEnd);
        Seconds s2 = cal.alignStart(occ);
        if (s2 == kInf) return;
        Seconds e2 = cal.advance(s2, list[k].setup + work);
        if (e2 == kInf) return;
        list[k].occ = s2;
        list[k].s = cal.rewind(e2, work);
        list[k].e = e2;
        prevEnd = e2;
      }
    };

    for (int m = 0; m < nMach; ++m) {
      auto& list = queues[static_cast<size_t>(m)];
      std::sort(list.begin(), list.end(), [](const Slot& x, const Slot& y) { return x.s < y.s; });
      for (size_t k = 1; k < list.size(); ++k) {
        if (list[k].occ >= list[k - 1].e) continue;
        relayoutFrom(m, k, list[k - 1].e);
      }
    }

    // 跨机器工艺顺序。
    // 实现要点: 不再"每修一处就从头重扫" —— 那样 O(违约数 x 工序数), 2400 工序下能吃掉
    // 解码 80% 的时间。改为按派工顺序 (opOrder 天然满足"订单内前道在前") 单趟扫描:
    // 前道被推后, 后道必然排在它后面, 同一趟里就能接着修好; 极少情况下同一趟内
    // 有工序被"前面更晚的放松"二次推后, 所以最多跑 4 趟收敛。
    std::vector<int> opM(static_cast<size_t>(nOps), -1);
    std::vector<int> opPos(static_cast<size_t>(nOps), -1);

    for (int pass = 0; pass < 4; ++pass) {
      // relayoutFrom 只改时刻不改位置, 所以位置表只需建一次
      if (pass == 0) {
        for (int m = 0; m < nMach; ++m)
          for (size_t k = 0; k < queues[static_cast<size_t>(m)].size(); ++k) {
            int op = queues[static_cast<size_t>(m)][k].op;
            opM[static_cast<size_t>(op)] = m;
            opPos[static_cast<size_t>(op)] = static_cast<int>(k);
          }
      }
      bool fixed = false;
      for (int cur : sol.opOrder) {
        const OpInst& op = p_.ops[static_cast<size_t>(cur)];
        if (op.seq <= 0) continue;
        const auto& chain = p_.orderOps[static_cast<size_t>(op.order)];
        if (op.seq - 1 >= static_cast<int>(chain.size())) continue;
        int prev = chain[static_cast<size_t>(op.seq - 1)];
        int pm = opM[static_cast<size_t>(prev)];
        int pp = opPos[static_cast<size_t>(prev)];
        int m = opM[static_cast<size_t>(cur)];
        int pos = opPos[static_cast<size_t>(cur)];
        if (pm < 0 || pp < 0 || m < 0 || pos < 0) continue;
        Seconds prevEnd = queues[static_cast<size_t>(pm)][static_cast<size_t>(pp)].e;
        if (queues[static_cast<size_t>(m)][static_cast<size_t>(pos)].occ < prevEnd) {
          relayoutFrom(m, static_cast<size_t>(pos), prevEnd);
          fixed = true;
        }
      }
      if (!fixed) break;
    }
  }

  if (buildAssign) {
    // 自愈可能改动了 s/e, 用队列的最终值回填
    for (int m = 0; m < nMach; ++m)
      for (const auto& sl : queues[static_cast<size_t>(m)]) {
        Assignment& a = sol.assign[static_cast<size_t>(sl.op)];
        a.op = sl.op;
        a.machine = m;
        a.start = sl.s;
        a.end = sl.e;
        a.setup = sl.setup;
        a.occStart = sl.occ;
        a.workSec = p_.machines[static_cast<size_t>(m)].cal.capacity(sl.s, sl.e);
        if (a.workSec <= 0) a.workSec = sl.e - sl.s;
      }
    Metrics metrics(p_);
    sol.kpi = metrics.compute(sol.assign, false);
    return sol;
  }

  // ---- 快路径: 只算目标函数所需的量 ----
  KPI k;
  for (int o = 0; o < p_.numOrders(); ++o) {
    const auto& chain = p_.orderOps[static_cast<size_t>(o)];
    if (chain.empty()) continue;
    ++k.ordersTotal;
    Seconds comp = -1;
    for (int opIdx : chain)
      comp = std::max(comp, opEnd[static_cast<size_t>(opIdx)]);
    if (comp < 0) comp = p_.horizonEnd;
    Seconds due = p_.orders[static_cast<size_t>(o)].due;
    Seconds tard = comp > due ? comp - due : 0;
    k.totalTardiness += tard;
    k.maxTardiness = std::max(k.maxTardiness, tard);
    if (tard == 0) ++k.ordersOnTime; else ++k.ordersLate;
    int prio = std::min(5, std::max(1, p_.orders[static_cast<size_t>(o)].priority));
    double pw = p_.weights.priorityWeight[prio];
    if (p_.orders[static_cast<size_t>(o)].urgent) pw *= 1.5;
    k.weightedTardinessHours += (static_cast<double>(tard) / 3600.0) * pw;
  }
  k.otd = k.ordersTotal > 0
              ? static_cast<double>(k.ordersOnTime) / static_cast<double>(k.ordersTotal)
              : 1.0;
  k.setupSec = totalSetup;
  k.setupCount = setupCount;
  k.makespan = maxEnd;
  k.makespanSpan = (minOcc < kInf && maxEnd > minOcc) ? maxEnd - minOcc : 0;
  k.objective = p_.weights.tardiness * k.weightedTardinessHours +
                p_.weights.setup * (static_cast<double>(k.setupSec) / 60.0) +
                p_.weights.makespan * (static_cast<double>(k.makespanSpan) / 3600.0);
  sol.kpi = k;
  return sol;
}

}  // namespace aps

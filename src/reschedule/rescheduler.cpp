// ===========================================================================
// reschedule/rescheduler.cpp -- 动态重排实现
//
// 共用工具:
//   Occ/Place        占用记录与放置结果 (与 Decoder 的 Slot/Placement 同构)
//   placeOn          与 Decoder 完全一致的"最早可行位置"插入规则
//   relayoutFrom     从某位置起整段顺延 (只推后不提前, 保稳定性)
//   rippleFix        级联修复: 前道被推后导致后道违规时迭代拉平
//   remap/keysOf     工序数量变化后按 (order,seq) 重映射旧解的 Assignment
//   loadQueues       基线解 -> 各设备占用队列 (并标记冻结/可动)
//
// 三个事件入口都遵循同一纪律: 已开工 (start<=now) 冻结; 其余只做局部修复;
// 输出报告量化"动了多少" (漂移工序数 / 换机数 / 稳定性代价)。
// ===========================================================================
#include "aps/reschedule/rescheduler.hpp"

#include <algorithm>
#include <chrono>
#include <map>

#include "aps/engine/decoder.hpp"
#include "aps/kpi/metrics.hpp"
#include "aps/util/random.hpp"

namespace aps {
namespace {

using Clock = std::chrono::steady_clock;

/// 距计时起点的毫秒数。
double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/// 重排内部的占用记录 (对应 Assignment, 加上纯加工秒数)。
struct Occ {
  int op;         ///< 工序下标
  Seconds s;      ///< 加工开始
  Seconds e;      ///< 加工结束
  Seconds setup;  ///< 前置准备
  Seconds occ;    ///< 机器开始占用
  Seconds work;   ///< 纯加工产能秒数
};

/// 一次放置尝试的结果 (语义同 Decoder::Placement)。
struct Place {
  bool ok = false;      ///< 是否找到可行位置
  int pos = -1;         ///< 插入位置 (队列下标)
  Seconds start = 0;    ///< 加工开始
  Seconds end = 0;      ///< 加工结束
  Seconds setup = 0;    ///< 换型秒数
  Seconds occStart = 0; ///< 占用起点
  Seconds work = 0;     ///< 纯加工秒数
};

// 与 Decoder 完全一致的插入规则: 找第一个塞得下的位置
/// 在指定设备的队列上找最早可行插入位置。
/// @param p          问题实例 (查换型矩阵与日历)
/// @param machineIdx 目标设备
/// @param q          该设备的占用队列 (按 s 升序)
/// @param opIdx      待插工序
/// @param ready      最早可开工时刻
/// @param dur        纯加工秒数
/// @param famIdx     工序换型族索引
/// @return           放置结果; ok=false 表示这台设备排不下
Place placeOn(const Problem& p, int machineIdx, std::vector<Occ>& q, int opIdx, Seconds ready,
              Seconds dur, int famIdx) {
  Place out;
  const Timeline& cal = p.machines[static_cast<size_t>(machineIdx)].cal;
  if (cal.empty()) return out;
  const Seconds kNegInf = -kInf / 4;

  for (int pos = 0; pos <= static_cast<int>(q.size()); ++pos) {
    Seconds prevEnd = (pos == 0) ? kNegInf : q[static_cast<size_t>(pos - 1)].e;
    int prevFam = (pos == 0) ? -1
                             : p.setup.indexOf(
                                   p.ops[static_cast<size_t>(q[static_cast<size_t>(pos - 1)].op)].family);
    Seconds setupIn = p.setup.get(prevFam, famIdx);

    Seconds occ = std::max(ready, prevEnd);
    Seconds aligned = cal.alignStart(occ);
    if (aligned == kInf) return out;
    Seconds end = cal.advance(aligned, setupIn + dur);
    if (end == kInf) return out;

    if (pos < static_cast<int>(q.size())) {
      Seconds setupOut = p.setup.get(
          famIdx, p.setup.indexOf(p.ops[static_cast<size_t>(q[static_cast<size_t>(pos)].op)].family));
      if (end > q[static_cast<size_t>(pos)].s) continue;
      if (cal.capacity(end, q[static_cast<size_t>(pos)].s) < setupOut) continue;
      Seconds newOcc = cal.rewind(q[static_cast<size_t>(pos)].s, setupOut);
      if (newOcc == kInf || newOcc < end) continue;
    }
    out.ok = true;
    out.pos = pos;
    out.end = end;
    out.start = cal.rewind(end, dur);  // 跨班次必须按可用产能回推
    out.setup = setupIn;
    out.occStart = aligned;
    out.work = dur;
    return out;
  }
  return out;
}

// 从队列 idx 位置起向后重排: 只推后, 不提前 (保计划稳定性)
/// 从队列 idx 位置起整段顺延到不早于 earliest。
/// @param p        问题实例 (查日历)
/// @param m        设备下标
/// @param q        该设备占用队列 (原地修改)
/// @param idx      起始位置
/// @param earliest 最早可占用时刻
void relayoutFrom(const Problem& p, int m, std::vector<Occ>& q, size_t idx, Seconds earliest) {
  const Timeline& cal = p.machines[static_cast<size_t>(m)].cal;
  Seconds prevEnd = earliest;
  if (idx > 0) prevEnd = std::max(prevEnd, q[idx - 1].e);
  for (size_t j = idx; j < q.size(); ++j) {
    Seconds occ = std::max(q[j].occ, prevEnd);
    Seconds s = cal.alignStart(occ);
    if (s == kInf) return;
    Seconds e = cal.advance(s, q[j].setup + q[j].work);
    if (e == kInf) return;
    q[j].occ = s;
    q[j].s = cal.rewind(e, q[j].work);
    q[j].e = e;
    prevEnd = e;
  }
}

// 级联顺延: 迁移/插入后, 同一订单的后道可能被前道甩在后面, 迭代拉到一致
/// 修复所有"后道早于前道结束"的违规, 直到无违规或达到轮数上限。
/// @param p         问题实例
/// @param q         全部设备的占用队列 (原地修改)
/// @param maxRounds 最大修复轮数 (兜底, 防死循环)
void rippleFix(const Problem& p, std::vector<std::vector<Occ>>& q, int maxRounds = 400) {
  const int nOps = p.numOps();
  std::vector<int> opMachine(static_cast<size_t>(nOps), -1);
  std::vector<int> opPos(static_cast<size_t>(nOps), -1);
  std::vector<Seconds> opEnd(static_cast<size_t>(nOps), -1);

  for (int round = 0; round < maxRounds; ++round) {
    std::fill(opMachine.begin(), opMachine.end(), -1);
    for (int m = 0; m < p.numMachines(); ++m)
      for (size_t j = 0; j < q[static_cast<size_t>(m)].size(); ++j) {
        int op = q[static_cast<size_t>(m)][j].op;
        opMachine[static_cast<size_t>(op)] = m;
        opPos[static_cast<size_t>(op)] = static_cast<int>(j);
        opEnd[static_cast<size_t>(op)] = q[static_cast<size_t>(m)][j].e;
      }

    bool fixed = false;
    for (int o = 0; o < p.numOrders() && !fixed; ++o) {
      const auto& chain = p.orderOps[static_cast<size_t>(o)];
      for (size_t k = 1; k < chain.size(); ++k) {
        int prev = chain[k - 1], cur = chain[k];
        if (opMachine[static_cast<size_t>(cur)] < 0) continue;
        if (opEnd[static_cast<size_t>(prev)] < 0) continue;
        int m = opMachine[static_cast<size_t>(cur)];
        int pos = opPos[static_cast<size_t>(cur)];
        if (q[static_cast<size_t>(m)][static_cast<size_t>(pos)].occ < opEnd[static_cast<size_t>(prev)]) {
          relayoutFrom(p, m, q[static_cast<size_t>(m)], static_cast<size_t>(pos),
                       opEnd[static_cast<size_t>(prev)]);
          fixed = true;
          break;
        }
      }
    }
    if (!fixed) return;
  }
}

/// 把工序插进队列 (位置越界时追加到队尾)。
void commit(std::vector<Occ>& q, int pos, Occ o) {
  pos = std::min(pos, static_cast<int>(q.size()));
  q.insert(q.begin() + pos, o);
}

// 生成 (order,seq) 键, 用于 rebuild 后重映射 op 索引
std::vector<std::pair<int, int>> keysOf(const Problem& p) {
  std::vector<std::pair<int, int>> k;
  k.reserve(static_cast<size_t>(p.numOps()));
  for (const auto& op : p.ops) k.emplace_back(op.order, op.seq);
  return k;
}

// rebuild 后按 (order,seq) 把旧 Solution 的 Assignment 搬到新索引上
/// 生成 (订单, 工艺序号) 键列表 —— 工序数量变化后用于新旧索引对齐。
Solution remap(const Problem& newP, const std::vector<std::pair<int, int>>& oldKeys,
               const Solution& old) {
  Solution s;
  s.solver = old.solver;
  s.assign.assign(static_cast<size_t>(newP.numOps()), Assignment{});

  std::map<std::pair<int, int>, int> newIdx;
  for (int i = 0; i < newP.numOps(); ++i) newIdx[{newP.ops[static_cast<size_t>(i)].order,
                                                  newP.ops[static_cast<size_t>(i)].seq}] = i;

  for (size_t i = 0; i < old.assign.size() && i < oldKeys.size(); ++i) {
    // 基准解里没排上的工序 (op < 0) 必须跳过, 否则会被当成"0 时刻的假工序"搬过去
    if (old.assign[i].op < 0) continue;
    auto it = newIdx.find(oldKeys[i]);
    if (it == newIdx.end()) continue;
    Assignment a = old.assign[i];
    a.op = it->second;
    s.assign[static_cast<size_t>(a.op)] = a;
  }
  return s;
}

// 把 baseline 装载进每台设备的占用队列
/// 把基准解装载进各设备占用队列, 并标记每道工序的状态。
/// scheduled 语义: 0=待定 1=可动 2=冻结 (start<=now, 已开工)。
/// @param onlyFirstN 只装载前 N 道工序 (插单场景: 新增工序还没排过, 不装)
/// @param frozen     [出参] 冻结工序计数
void loadQueues(const Problem& p, const Solution& sol, std::vector<std::vector<Occ>>& q,
                std::vector<Seconds>& opEnd, std::vector<char>& scheduled, int onlyFirstN,
                const RescheduleOptions& opt, int& frozen) {
  const int n = std::min(onlyFirstN, static_cast<int>(sol.assign.size()));
  for (int i = 0; i < n; ++i) {
    const Assignment& a = sol.assign[static_cast<size_t>(i)];
    if (a.op < 0 || a.machine < 0 || a.machine >= p.numMachines()) continue;
    q[static_cast<size_t>(a.machine)].push_back(
        Occ{a.op, a.start, a.end, a.setup, a.occupancyStart(), a.work()});
    opEnd[static_cast<size_t>(a.op)] = a.end;
    if (a.start <= opt.now) {
      scheduled[static_cast<size_t>(a.op)] = 2;  // 2 = 冻结
      ++frozen;
    } else {
      scheduled[static_cast<size_t>(a.op)] = 1;
    }
  }
  for (auto& list : q)
    std::sort(list.begin(), list.end(), [](const Occ& x, const Occ& y) { return x.s < y.s; });
}

/// 从所有设备队列里摘掉指定工序 (找不到时静默)。
void removeFromQueues(std::vector<std::vector<Occ>>& q, int op) {
  for (auto& list : q) {
    auto it = std::find_if(list.begin(), list.end(), [&](const Occ& o) { return o.op == op; });
    if (it != list.end()) {
      list.erase(it);
      return;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
/// 急单插队 (见头文件)。流程: 追加订单 -> remap 基线 -> 新工序逐道找最早空隙插入
/// -> rippleFix 拉平 -> 与基线对比生成漂移/稳定性统计。
RescheduleReport Rescheduler::insertOrder(Problem& p, const Solution& baseline, const Order& order,
                                          const RescheduleOptions& opt) {
  const auto t0 = Clock::now();
  RescheduleReport rep;
  if (baseline.assign.empty()) {
    rep.message = "尚无排产结果, 请先执行一次排产";
    return rep;
  }
  const int oldOps = p.numOps();
  auto oldKeys = keysOf(p);

  p.orders.push_back(order);
  p.rebuild();

  if (p.numOps() <= oldOps) {
    p.orders.pop_back();
    p.rebuild();
    rep.message = "该订单的产品没有匹配的工艺路线, 无法插入";
    return rep;
  }

  Solution base = remap(p, oldKeys, baseline);

  std::vector<std::vector<Occ>> q(static_cast<size_t>(p.numMachines()));
  std::vector<Seconds> opEnd(static_cast<size_t>(p.numOps()), -1);
  std::vector<char> scheduled(static_cast<size_t>(p.numOps()), 0);
  int frozen = 0;
  loadQueues(p, base, q, opEnd, scheduled, oldOps, opt, frozen);

  // 待插工序 = 新订单展开出来的工序, 按工艺顺序
  std::vector<int> incoming;
  for (int i = oldOps; i < p.numOps(); ++i) incoming.push_back(i);

  int inserted = 0;
  for (int opIdx : incoming) {
    const OpInst& op = p.ops[static_cast<size_t>(opIdx)];
    Seconds ready = p.orders[static_cast<size_t>(op.order)].release;
    if (op.seq > 0) {
      const auto& chain = p.orderOps[static_cast<size_t>(op.order)];
      if (op.seq - 1 < static_cast<int>(chain.size())) {
        int prev = chain[static_cast<size_t>(op.seq - 1)];
        if (opEnd[static_cast<size_t>(prev)] > 0)
          ready = std::max(ready, opEnd[static_cast<size_t>(prev)]);
      }
    }
    ready = std::max(ready, opt.now);
    const int famIdx = p.setup.indexOf(op.family);

    Place best;
    int bestM = -1;
    for (const auto& mo : op.options) {
      if (mo.machine < 0 || mo.machine >= p.numMachines()) continue;
      if (p.machines[static_cast<size_t>(mo.machine)].down) continue;
      Seconds dur = static_cast<Seconds>(static_cast<double>(op.qty) * mo.unitSeconds + 0.5);
      Place pl = placeOn(p, mo.machine, q[static_cast<size_t>(mo.machine)], opIdx, ready, dur,
                         famIdx);
      if (pl.ok && (!best.ok || pl.end < best.end)) {
        best = pl;
        bestM = mo.machine;
      }
    }
    if (!best.ok) continue;  // 窗口内塞不下, 留空 (前端会显示未排)

    auto& list = q[static_cast<size_t>(bestM)];
    int pos = best.pos;
    commit(list, pos, Occ{opIdx, best.start, best.end, best.setup, best.occStart, best.work});
    if (pos + 1 < static_cast<int>(list.size())) {
      auto& nxt = list[static_cast<size_t>(pos + 1)];
      int nextFam = p.setup.indexOf(p.ops[static_cast<size_t>(nxt.op)].family);
      nxt.setup = p.setup.get(famIdx, nextFam);
      nxt.occ = p.machines[static_cast<size_t>(bestM)].cal.rewind(nxt.s, nxt.setup);
    }
    opEnd[static_cast<size_t>(opIdx)] = best.end;
    scheduled[static_cast<size_t>(opIdx)] = 1;
    ++inserted;
  }

  rippleFix(p, q);

  // 汇总
  Solution out;
  out.solver = "reschedule:insert";
  out.assign.assign(static_cast<size_t>(p.numOps()), Assignment{});
  for (int m = 0; m < p.numMachines(); ++m) {
    for (const auto& o : q[static_cast<size_t>(m)]) {
      Assignment& a = out.assign[static_cast<size_t>(o.op)];
      a.op = o.op;
      a.machine = m;
      a.start = o.s;
      a.end = o.e;
      a.setup = o.setup;
      a.occStart = o.occ;
      a.workSec = o.work;
      a.frozen = (scheduled[static_cast<size_t>(o.op)] == 2);
    }
  }

  Metrics metrics(p);
  out.kpi = metrics.compute(out.assign, true);
  out.feasible = !out.truncated;
  out.kpi.solveMs = msSince(t0);

  rep.ok = inserted == static_cast<int>(incoming.size());
  rep.message = rep.ok ? ("已插入 " + std::to_string(inserted) + " 道工序")
                       : ("部分工序插不进窗口: " + std::to_string(incoming.size() - inserted) + " 道");
  rep.solution = out;
  rep.frozenOps = frozen;
  rep.objectiveBefore = baseline.kpi.objective;
  rep.objectiveAfter = out.kpi.objective;
  rep.explainMs = msSince(t0);

  // 稳定性: 与基准对比
  Seconds shift = 0;
  int moved = 0, mach = 0;
  for (int i = 0; i < oldOps; ++i) {
    const Assignment& a = base.assign[static_cast<size_t>(i)];
    const Assignment& b = out.assign[static_cast<size_t>(i)];
    if (a.op < 0 || b.op < 0) continue;
    Seconds d = std::abs(b.start - a.start);
    if (d > 60) ++moved;
    shift += d;
    if (a.machine != b.machine) ++mach;
  }
  rep.movedOps = moved;
  rep.machineChanges = mach;
  rep.totalShiftSec = shift;
  rep.stabilityCost = static_cast<double>(shift) / 3600.0 * 0.5 + mach * 1.0;
  return rep;
}

// ---------------------------------------------------------------------------
/// 设备故障迁移 (见头文件)。三步: 日历挖空 -> 摘出未开工工序 -> 按 (订单,工艺序)
/// 升序逐道迁移到同组最早可完工设备 -> rippleFix 拉平。
RescheduleReport Rescheduler::machineDown(Problem& p, const Solution& baseline, int machineIdx,
                                          const RescheduleOptions& opt) {
  const auto t0 = Clock::now();
  RescheduleReport rep;
  if (baseline.assign.empty()) {
    rep.message = "尚无排产结果";
    return rep;
  }
  if (machineIdx < 0 || machineIdx >= p.numMachines()) {
    rep.message = "设备下标非法";
    return rep;
  }

  auto oldKeys = keysOf(p);
  Solution base = remap(p, oldKeys, baseline);

  // 1) 设备下线: 只把"从现在起"的日历挖空, 不动工艺路线。
  //    这样已开工(now 之前)的工序仍算合法占用, 不会被误判为"设备不支持";
  //    而未开工的活因为之后没有可用产能, 自然排不进去。
  p.machines[static_cast<size_t>(machineIdx)].down = true;
  {
    CalendarSpec spec = p.calendar;
    const Seconds far = p.horizonEnd + 30 * 86400;
    spec.downtime.push_back({std::max(opt.now, p.horizonStart), far});
    p.machines[static_cast<size_t>(machineIdx)].cal =
        Timeline::build(spec, p.horizonStart, far);
  }

  // 2) 建立占用队列, 剔除故障设备上未开工的工序
  std::vector<std::vector<Occ>> q(static_cast<size_t>(p.numMachines()));
  std::vector<Seconds> opEnd(static_cast<size_t>(p.numOps()), -1);
  std::vector<char> scheduled(static_cast<size_t>(p.numOps()), 0);
  int frozen = 0;
  loadQueues(p, base, q, opEnd, scheduled, p.numOps(), opt, frozen);

  std::vector<int> displaced;
  for (auto& o : q[static_cast<size_t>(machineIdx)]) {
    if (o.s <= opt.now) continue;  // 已开工的只能等修好, 不迁移
    if (scheduled[static_cast<size_t>(o.op)] == 2) continue;
    displaced.push_back(o.op);
  }
  {
    auto& list = q[static_cast<size_t>(machineIdx)];
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const Occ& o) {
                                return o.s > opt.now &&
                                       scheduled[static_cast<size_t>(o.op)] == 1;
                              }),
               list.end());
    for (int op : displaced) scheduled[static_cast<size_t>(op)] = 0;
  }

  // 3) 逐道重定位到其他设备
  //    必须按 (订单, 工序号) 升序处理: 前道先落位, 后道的 ready 才拿得到最新完工时刻,
  //    否则同一订单的印刷/层压都在故障机上时, 后道会排到前道前面去。
  std::sort(displaced.begin(), displaced.end(), [&](int x, int y) {
    if (p.ops[static_cast<size_t>(x)].order != p.ops[static_cast<size_t>(y)].order)
      return p.ops[static_cast<size_t>(x)].order < p.ops[static_cast<size_t>(y)].order;
    return p.ops[static_cast<size_t>(x)].seq < p.ops[static_cast<size_t>(y)].seq;
  });

  int moved = 0;
  for (int opIdx : displaced) {
    const OpInst& op = p.ops[static_cast<size_t>(opIdx)];
    if (op.options.empty()) continue;  // 该工序没有任何可选设备
    Seconds ready = p.orders[static_cast<size_t>(op.order)].release;
    if (op.seq > 0) {
      const auto& chain = p.orderOps[static_cast<size_t>(op.order)];
      if (op.seq - 1 < static_cast<int>(chain.size())) {
        int prev = chain[static_cast<size_t>(op.seq - 1)];
        if (opEnd[static_cast<size_t>(prev)] > 0)
          ready = std::max(ready, opEnd[static_cast<size_t>(prev)]);
      }
    }
    ready = std::max(ready, opt.now);
    const int famIdx = p.setup.indexOf(op.family);

    Place best;
    int bestM = -1;
    for (const auto& mo : op.options) {
      if (p.machines[static_cast<size_t>(mo.machine)].down) continue;
      Place pl = placeOn(p, mo.machine, q[static_cast<size_t>(mo.machine)], opIdx, ready,
                         static_cast<Seconds>(static_cast<double>(op.qty) * mo.unitSeconds + 0.5), famIdx);
      if (pl.ok && (!best.ok || pl.end < best.end)) {
        best = pl;
        bestM = mo.machine;
      }
    }
    if (!best.ok) continue;
    auto& list = q[static_cast<size_t>(bestM)];
    commit(list, best.pos,
           Occ{opIdx, best.start, best.end, best.setup, best.occStart, best.work});
    if (best.pos + 1 < static_cast<int>(list.size())) {
      auto& nxt = list[static_cast<size_t>(best.pos + 1)];
      int nextFam = p.setup.indexOf(p.ops[static_cast<size_t>(nxt.op)].family);
      nxt.setup = p.setup.get(famIdx, nextFam);
      nxt.occ = p.machines[static_cast<size_t>(bestM)].cal.rewind(nxt.s, nxt.setup);
    }
    opEnd[static_cast<size_t>(opIdx)] = best.end;
    scheduled[static_cast<size_t>(opIdx)] = 1;
    ++moved;
  }

  // 迁移会打乱"前道 -> 后道"的时间关系, 这里做级联顺延修复
  rippleFix(p, q);

  Solution out;
  out.solver = "reschedule:breakdown";
  out.assign.assign(static_cast<size_t>(p.numOps()), Assignment{});
  for (int m = 0; m < p.numMachines(); ++m) {
    for (const auto& o : q[static_cast<size_t>(m)]) {
      Assignment& a = out.assign[static_cast<size_t>(o.op)];
      a.op = o.op;
      a.machine = m;
      a.start = o.s;
      a.end = o.e;
      a.setup = o.setup;
      a.occStart = o.occ;
      a.workSec = o.work;
      a.frozen = (scheduled[static_cast<size_t>(o.op)] == 2);
    }
  }
  Metrics metrics(p);
  out.kpi = metrics.compute(out.assign, true);
  out.kpi.solveMs = msSince(t0);
  out.feasible = !out.truncated;

  rep.ok = moved == static_cast<int>(displaced.size());
  rep.message = "设备 " + p.machines[static_cast<size_t>(machineIdx)].code + " 停机, 迁移 " +
                std::to_string(moved) + "/" + std::to_string(displaced.size()) + " 道工序";
  rep.solution = out;
  rep.frozenOps = frozen;
  rep.objectiveBefore = baseline.kpi.objective;
  rep.objectiveAfter = out.kpi.objective;
  rep.explainMs = msSince(t0);

  Seconds shift = 0;
  int cnt = 0, mach = 0;
  for (int i = 0; i < p.numOps() && i < static_cast<int>(base.assign.size()); ++i) {
    const Assignment& a = base.assign[static_cast<size_t>(i)];
    const Assignment& b = out.assign[static_cast<size_t>(i)];
    if (a.op < 0 || b.op < 0) continue;
    Seconds d = std::abs(b.start - a.start);
    if (d > 60) ++cnt;
    shift += d;
    if (a.machine != b.machine) ++mach;
  }
  rep.movedOps = cnt;
  rep.machineChanges = mach;
  rep.totalShiftSec = shift;
  rep.stabilityCost = static_cast<double>(shift) / 3600.0 * 0.5 + mach * 1.0;
  return rep;
}

/// 设备恢复上线: 清停机标记 -> rebuild 恢复可选设备 -> 重编日历。
/// 不自动重排 —— 期间发生的故障迁移不会回滚, 需调用方重新排产。
void Rescheduler::machineUp(Problem& p, int machineIdx) {
  if (machineIdx < 0 || machineIdx >= p.numMachines()) return;
  p.machines[static_cast<size_t>(machineIdx)].down = false;
  // 从工艺路线重建, 恢复被剔除的可选设备
  p.rebuild();
  p.buildCalendars();
}

// ---------------------------------------------------------------------------
/// 滚动时域重排 (见头文件)。未冻结工序摘出后按原开工时间排序、原设备重新落位,
/// 保持相对次序以最小化计划漂移。
RescheduleReport Rescheduler::rollForward(Problem& p, const Solution& baseline,
                                          const RescheduleOptions& opt) {
  const auto t0 = Clock::now();
  RescheduleReport rep;
  if (baseline.assign.empty()) {
    rep.message = "尚无排产结果";
    return rep;
  }

  Solution base = baseline;
  std::vector<std::vector<Occ>> q(static_cast<size_t>(p.numMachines()));
  std::vector<Seconds> opEnd(static_cast<size_t>(p.numOps()), -1);
  std::vector<char> scheduled(static_cast<size_t>(p.numOps()), 0);
  int frozen = 0;
  loadQueues(p, base, q, opEnd, scheduled, p.numOps(), opt, frozen);

  // 未冻结且未开工的工序: 按原设备顺序重新落位 (可能前移)
  std::vector<int> movable;
  for (int i = 0; i < p.numOps(); ++i) {
    const Assignment& a = base.assign[static_cast<size_t>(i)];
    if (a.op < 0) continue;
    if (scheduled[static_cast<size_t>(i)] == 2) continue;
    movable.push_back(i);
  }

  // 按原开工时间排序, 保持相对次序 -> 计划漂移最小
  std::sort(movable.begin(), movable.end(), [&](int x, int y) {
    return base.assign[static_cast<size_t>(x)].start < base.assign[static_cast<size_t>(y)].start;
  });

  for (int opIdx : movable) {
    removeFromQueues(q, opIdx);
    scheduled[static_cast<size_t>(opIdx)] = 0;
  }
  // 重新插入
  for (int opIdx : movable) {
    const Assignment& a = base.assign[static_cast<size_t>(opIdx)];
    const OpInst& op = p.ops[static_cast<size_t>(opIdx)];
    Seconds ready = p.orders[static_cast<size_t>(op.order)].release;
    if (op.seq > 0) {
      const auto& chain = p.orderOps[static_cast<size_t>(op.order)];
      if (op.seq - 1 < static_cast<int>(chain.size())) {
        int prev = chain[static_cast<size_t>(op.seq - 1)];
        if (opEnd[static_cast<size_t>(prev)] > 0)
          ready = std::max(ready, opEnd[static_cast<size_t>(prev)]);
      }
    }
    ready = std::max(ready, opt.now);
    int famIdx = p.setup.indexOf(op.family);
    int m = a.machine;
    if (m < 0 || m >= p.numMachines() || p.machines[static_cast<size_t>(m)].down) m = op.options[0].machine;
    Seconds dur = a.work();
    Place pl = placeOn(p, m, q[static_cast<size_t>(m)], opIdx, ready, dur, famIdx);
    if (!pl.ok) {
      pl.ok = true;
      pl.pos = static_cast<int>(q[static_cast<size_t>(m)].size());
      Seconds prevEnd = q[static_cast<size_t>(m)].empty() ? ready : q[static_cast<size_t>(m)].back().e;
      pl.start = std::max(prevEnd, ready);
      pl.end = pl.start + dur;
      pl.setup = 0;
      pl.occStart = pl.start;
      pl.work = dur;
    }
    commit(q[static_cast<size_t>(m)], pl.pos, Occ{opIdx, pl.start, pl.end, pl.setup, pl.occStart, pl.work});
    if (pl.pos + 1 < static_cast<int>(q[static_cast<size_t>(m)].size())) {
      auto& nxt = q[static_cast<size_t>(m)][static_cast<size_t>(pl.pos + 1)];
      int nextFam = p.setup.indexOf(p.ops[static_cast<size_t>(nxt.op)].family);
      nxt.setup = p.setup.get(famIdx, nextFam);
      nxt.occ = p.machines[static_cast<size_t>(m)].cal.rewind(nxt.s, nxt.setup);
    }
    opEnd[static_cast<size_t>(opIdx)] = pl.end;
    scheduled[static_cast<size_t>(opIdx)] = 1;
  }

  Solution out;
  out.solver = "reschedule:roll";
  out.assign.assign(static_cast<size_t>(p.numOps()), Assignment{});
  for (int m = 0; m < p.numMachines(); ++m)
    for (const auto& o : q[static_cast<size_t>(m)]) {
      Assignment& a = out.assign[static_cast<size_t>(o.op)];
      a.op = o.op;
      a.machine = m;
      a.start = o.s;
      a.end = o.e;
      a.setup = o.setup;
      a.occStart = o.occ;
      a.workSec = o.work;
      a.frozen = (scheduled[static_cast<size_t>(o.op)] == 2);
    }

  Metrics metrics(p);
  out.kpi = metrics.compute(out.assign, true);
  out.kpi.solveMs = msSince(t0);
  out.feasible = !out.truncated;

  Seconds shift = 0;
  int moved = 0, mach = 0;
  for (int i = 0; i < p.numOps() && i < static_cast<int>(base.assign.size()); ++i) {
    const Assignment& a = base.assign[static_cast<size_t>(i)];
    const Assignment& b = out.assign[static_cast<size_t>(i)];
    if (a.op < 0 || b.op < 0) continue;
    Seconds d = std::abs(b.start - a.start);
    if (d > 60) ++moved;
    shift += d;
    if (a.machine != b.machine) ++mach;
  }

  rep.ok = true;
  rep.message = "滚动重排完成, 冻结 " + std::to_string(frozen) + " 道已开工工序";
  rep.solution = out;
  rep.frozenOps = frozen;
  rep.movedOps = moved;
  rep.machineChanges = mach;
  rep.totalShiftSec = shift;
  rep.stabilityCost = static_cast<double>(shift) / 3600.0 * 0.5 + mach * 1.0;
  rep.objectiveBefore = baseline.kpi.objective;
  rep.objectiveAfter = out.kpi.objective;
  rep.explainMs = msSince(t0);
  return rep;
}

}  // namespace aps

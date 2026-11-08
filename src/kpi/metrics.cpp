// ===========================================================================
// kpi/metrics.cpp -- KPI 与目标函数实现
//
// compute 分两段:
//   基础段 (必算)   : 交期/OTD/延期、换型统计、makespan —— 目标函数的全部输入
//   展示段 (detailed): 设备利用率 (占用/可用, 窗口到 makespan)、
//                      在制峰值 (开工+1/完工-1 的扫描线, 同时刻先减后加)
// ===========================================================================
#include "aps/kpi/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace aps {

/// 计算全部 KPI (口径见头文件)。未排完的订单按窗口末端保守估计延期。
KPI Metrics::compute(const std::vector<Assignment>& assign, bool detailed) const {
  KPI k;
  const int nOrders = p_.numOrders();

  std::vector<Seconds> completion(static_cast<size_t>(nOrders), -1);
  std::vector<int> doneCount(static_cast<size_t>(nOrders), 0);

  Seconds minStart = kInf;
  Seconds maxEnd = 0;
  Seconds setupSec = 0;
  int setupCount = 0;

  for (size_t i = 0; i < assign.size(); ++i) {
    const Assignment& a = assign[i];
    if (a.op < 0 || a.op >= p_.numOps() || a.machine < 0) continue;
    const OpInst& op = p_.ops[static_cast<size_t>(a.op)];
    completion[static_cast<size_t>(op.order)] =
        std::max(completion[static_cast<size_t>(op.order)], a.end);
    ++doneCount[static_cast<size_t>(op.order)];
    minStart = std::min(minStart, a.occupancyStart());
    maxEnd = std::max(maxEnd, a.end);
    if (a.setup > 0) {
      setupSec += a.setup;
      ++setupCount;
    }
  }

  // ---- 交期 ----
  for (int o = 0; o < nOrders; ++o) {
    if (p_.orderOps[static_cast<size_t>(o)].empty()) continue;  // 无可排工序, 不计
    ++k.ordersTotal;
    Seconds comp = completion[static_cast<size_t>(o)];
    if (doneCount[static_cast<size_t>(o)] <
        static_cast<int>(p_.orderOps[static_cast<size_t>(o)].size())) {
      comp = p_.horizonEnd;  // 没排完: 按窗口末端保守估计
    }
    Seconds due = p_.orders[static_cast<size_t>(o)].due;
    Seconds tard = comp > due ? comp - due : 0;
    k.totalTardiness += tard;
    k.maxTardiness = std::max(k.maxTardiness, tard);
    if (tard == 0) {
      ++k.ordersOnTime;
    } else {
      ++k.ordersLate;
    }
    int prio = std::min(5, std::max(1, p_.orders[static_cast<size_t>(o)].priority));
    double pw = p_.weights.priorityWeight[prio];
    if (p_.orders[static_cast<size_t>(o)].urgent) pw *= 1.5;  // 急单放大
    k.weightedTardinessHours += (static_cast<double>(tard) / 3600.0) * pw;
  }

  k.otd = k.ordersTotal > 0
              ? static_cast<double>(k.ordersOnTime) / static_cast<double>(k.ordersTotal)
              : 1.0;
  k.setupSec = setupSec;
  k.setupCount = setupCount;

  if (minStart < kInf && maxEnd > minStart) {
    k.makespan = maxEnd;
    k.makespanSpan = maxEnd - minStart;
  } else {
    k.makespan = 0;
    k.makespanSpan = 0;
  }

  k.objective = cost(k);

  if (!detailed) return k;

  // ---- 设备利用率 ----
  k.machineUtilization.assign(static_cast<size_t>(p_.numMachines()), 0.0);
  std::vector<Seconds> busy(static_cast<size_t>(p_.numMachines()), 0);
  for (const auto& a : assign) {
    if (a.machine < 0 || a.machine >= p_.numMachines()) continue;
    // 占用 = 换型 + 加工 (解码器保证都落在可用区间内)
    busy[static_cast<size_t>(a.machine)] += a.setup + a.work();
  }
  Seconds windowEnd = k.makespan > 0 ? k.makespan : p_.horizonEnd;
  double sum = 0;
  for (int m = 0; m < p_.numMachines(); ++m) {
    Seconds avail = p_.machines[static_cast<size_t>(m)].cal.capacity(p_.horizonStart, windowEnd);
    double util = avail > 0 ? static_cast<double>(busy[static_cast<size_t>(m)]) /
                                  static_cast<double>(avail)
                            : 0.0;
    k.machineUtilization[static_cast<size_t>(m)] = util;
    sum += util;
  }
  k.avgUtilization = p_.numMachines() > 0 ? sum / p_.numMachines() : 0.0;

  // ---- 在制峰值: 扫描线 (同一时刻先减后加) ----
  std::vector<std::pair<Seconds, int>> ev;
  ev.reserve(assign.size() * 2);
  for (const auto& a : assign) {
    if (a.op < 0) continue;
    ev.emplace_back(a.start, +1);
    ev.emplace_back(a.end, -1);
  }
  std::sort(ev.begin(), ev.end(), [](const auto& x, const auto& y) {
    if (x.first != y.first) return x.first < y.first;
    return x.second < y.second;
  });
  int cur = 0;
  for (const auto& e : ev) {
    cur += e.second;
    k.peakWip = std::max(k.peakWip, cur);
  }
  return k;
}

/// 目标函数 = 延期 x w_t + 换型分钟 x w_s + makespan小时 x w_m (权重来自 Problem::weights)。
double Metrics::cost(const KPI& k) const {
  const Weights& w = p_.weights;
  double c = 0.0;
  c += w.tardiness * k.weightedTardinessHours;
  c += w.setup * (static_cast<double>(k.setupSec) / 60.0);
  c += w.makespan * (static_cast<double>(k.makespanSpan) / 3600.0);
  return c;
}

/// 便捷组合: compute + cost (结果已在 k.objective 里, 直接返回)。
double Metrics::costOf(std::vector<Assignment>& assign, bool detailed) const {
  KPI k = compute(assign, detailed);
  (void)k;
  return k.objective;
}

}  // namespace aps

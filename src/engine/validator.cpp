// ===========================================================================
// engine/validator.cpp -- 可行性校验器实现 (V1~V6)
//
// 检查顺序: V1 完整性 -> V2 设备能力 -> V6 发布时刻 -> V3 日历 -> V4 互斥
//          -> V5 工艺顺序。同设备工序按占用起点排序后查相邻重叠。
// 与解码器零共享代码 —— 才能起到交叉验证的作用。
// ===========================================================================
#include "aps/engine/validator.hpp"

#include <algorithm>
#include <map>
#include <sstream>

#include "aps/util/time_util.hpp"

namespace aps {
namespace {

/// @brief 记录一条违规, 并将结果标记为不可行 (明细上限 200 条, 防病态输入撑爆内存)。
///
/// @param v 违规对象 (类型/工序号/描述)
/// @param r 校验结果, 输出; 标记 ok=false, 超出上限则丢弃本条
void add(Violation v, ValidationResult& r) {
  r.ok = false;
  if (r.violations.size() < 200) r.violations.push_back(std::move(v));
}

}  // namespace

/// @brief 生成人读摘要: 通过时返回 "OK", 否则列出前 maxItems 条违规。
///
/// 每条格式为 "  - [违规类型] 描述", 超出 maxItems 的部分用 "  ..." 截断。
///
/// @param maxItems 最多列出多少条明细 (明细本身在 add() 里已限 200 条)
/// @return         摘要文本; 校验通过时就是 "OK"
std::string ValidationResult::summary(size_t maxItems) const {
  if (ok) return "OK";
  std::ostringstream oss;
  oss << "发现 " << violations.size() << " 处违规";
  size_t n = std::min(maxItems, violations.size());
  for (size_t i = 0; i < n; ++i)
    oss << "\n  - [" << violations[i].type << "] " << violations[i].detail;
  if (violations.size() > n) oss << "\n  ...";
  return oss.str();
}

/// @brief 便捷重载: 直接校验整个 Solution (内部取其 assign 转调下面的实现)。
///
/// 只校验工序安排本身, 不校验 KPI 数值是否自洽 —— KPI 由 Metrics 独立计算。
///
/// @param p   问题实例 (提供工艺路线、设备能力、日历与交期)
/// @param sol 待校验的解; 只用到 sol.assign
/// @return    校验结果; ok=false 时 violations 里是具体违例
ValidationResult validate(const Problem& p, const Solution& sol) {
  return validate(p, sol.assign);
}

/// @brief 全量校验 V1~V6 (与解码器零共享代码, 所以能起到交叉验证的作用)。
///
/// 检查顺序: V1 完整性 -> V2 设备能力 -> V6 发布时刻 -> V3 日历 -> V4 同机互斥 -> V5 工艺顺序。
/// 提前返回不适用于任何一项 —— 六项全部独立跑完, 一次把问题全暴露出来。
///
/// @param p      问题实例
/// @param assign 工序安排表 (下标 = op id, 长度须 >= numOps())
/// @return       结果; 违规明细上限 200 条 (防病态输入撑爆内存)
ValidationResult validate(const Problem& p, const std::vector<Assignment>& assign) {
  ValidationResult r;
  const int nOps = p.numOps();
  const int nMach = p.numMachines();

  if (static_cast<int>(assign.size()) < nOps) {
    add({"V1_incomplete", -1,
         "Assignment 数量 " + std::to_string(assign.size()) + " < 工序数 " + std::to_string(nOps)},
        r);
    return r;
  }

  std::vector<std::vector<int>> byMachine(static_cast<size_t>(nMach));

  for (int i = 0; i < nOps; ++i) {
    const Assignment& a = assign[static_cast<size_t>(i)];
    const OpInst& op = p.ops[static_cast<size_t>(i)];
    ++r.checkedOps;

    if (a.op != i) {
      add({"V1_incomplete", i, "工序 #" + std::to_string(i) + " 的 Assignment 未填充"}, r);
      continue;
    }
    if (a.machine < 0 || a.machine >= nMach) {
      add({"V1_incomplete", i, "工序 #" + std::to_string(i) + " 设备下标非法"}, r);
      continue;
    }
    if (a.end < a.start || a.start < 0) {
      add({"V1_incomplete", i, "工序 #" + std::to_string(i) + " 时间非法"}, r);
      continue;
    }

    // V2 设备能力
    bool supported = false;
    Seconds dur = 0;
    for (const auto& mo : op.options) {
      if (mo.machine == a.machine) {
        supported = true;
        dur = static_cast<Seconds>(static_cast<double>(op.qty) * mo.unitSeconds + 0.5);
        break;
      }
    }
    if (!supported) {
      add({"V2_machine_not_supported", i,
           p.machines[static_cast<size_t>(a.machine)].code + " 不支持工序 " + op.name},
          r);
      continue;
    }
    if (dur != a.work()) {
      add({"V1_incomplete", i,
           "工序 " + op.name + " 加工时长不符: 期望 " + std::to_string(dur) + "s, 实际 " +
               std::to_string(a.work()) + "s"},
          r);
    }

    // V3 日历
    const Timeline& cal = p.machines[static_cast<size_t>(a.machine)].cal;
    Seconds need = a.setup + dur;
    Seconds got = cal.capacity(a.occupancyStart(), a.end);
    if (got + 1 < need) {
      add({"V3_outside_calendar", i,
           "工序 " + op.name + " 落窗外: occ=" + timeutil::format(a.occupancyStart()) +
               " start=" + timeutil::format(a.start) + " end=" + timeutil::format(a.end) +
               " setup=" + std::to_string(a.setup) + " (需 " + std::to_string(need) +
               "s, 实得 " + std::to_string(got) + "s)"},
          r);
    }

    // V6 发布时刻
    if (a.start < p.orders[static_cast<size_t>(op.order)].release) {
      add({"V6_release_violation", i,
           "订单 " + p.orders[static_cast<size_t>(op.order)].code + " 工序 " + op.name +
               " 早于订单发布时间"},
          r);
    }

    byMachine[static_cast<size_t>(a.machine)].push_back(i);
  }

  // V4 设备互斥
  for (int m = 0; m < nMach; ++m) {
    auto& list = byMachine[static_cast<size_t>(m)];
    std::sort(list.begin(), list.end(), [&](int x, int y) {
      return assign[static_cast<size_t>(x)].start < assign[static_cast<size_t>(y)].start;
    });
    for (size_t i = 1; i < list.size(); ++i) {
      const Assignment& prev = assign[static_cast<size_t>(list[i - 1])];
      const Assignment& cur = assign[static_cast<size_t>(list[i])];
      if (prev.end > cur.occupancyStart()) {
        add({"V4_machine_overlap", list[i],
             p.machines[static_cast<size_t>(m)].code + " 上 #" + std::to_string(prev.op) + " " +
                 p.ops[static_cast<size_t>(prev.op)].name +
                 "[" + timeutil::format(prev.occupancyStart(), false) + " occ " +
                 timeutil::format(prev.occupancyStart()) + " -> " +
                 timeutil::format(prev.end) + " setup=" + std::to_string(prev.setup) +
                 "] 与 #" + std::to_string(cur.op) + " " + p.ops[static_cast<size_t>(cur.op)].name + "[" +
                 timeutil::format(cur.occupancyStart()) + " -> " + timeutil::format(cur.end) +
                 " setup=" + std::to_string(cur.setup) + "] 重叠 " +
                 std::to_string(prev.end - cur.occupancyStart()) + "s"},
            r);
      }
    }
  }

  // V5 工艺顺序
  for (int o = 0; o < p.numOrders(); ++o) {
    const auto& chain = p.orderOps[static_cast<size_t>(o)];
    for (size_t k = 1; k < chain.size(); ++k) {
      const Assignment& prev = assign[static_cast<size_t>(chain[k - 1])];
      const Assignment& cur = assign[static_cast<size_t>(chain[k])];
      if (cur.start < prev.end) {
        add({"V5_precedence", chain[k],
             "订单 " + p.orders[static_cast<size_t>(o)].code + " 工序 " +
                 p.ops[static_cast<size_t>(cur.op)].name + " 早于前道结束 " +
                 std::to_string(prev.end - cur.start) + "s"},
            r);
      }
    }
  }

  return r;
}

}  // namespace aps

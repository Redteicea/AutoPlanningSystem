// ===========================================================================
// core/model.cpp -- 领域模型实现
//
// Timeline::build  日历编译: 班次/工作日 -> 原始区间 -> 合并 -> 扣停机
// Timeline 查询族  alignStart/advance/rewind/capacity, 全部二分定位起点
// SetupMatrix      换型族索引 + 方阵存取 (未初始化时 get() 恒 0)
// Problem          工序展开 rebuild() 与日历编译 buildCalendars()
// ===========================================================================
#include "aps/core/model.hpp"

#include <algorithm>
#include <unordered_map>

namespace aps {

// ---------------------------------------------------------------------------
// Timeline
// ---------------------------------------------------------------------------

/// 编译日历规格为可用区间表。
/// 流程: 逐天展开班次 (跨夜班 endHour+24) -> 裁剪到 [from,to] -> 排序合并
///      -> 逐个扣除停机窗口 (一段被扣除后可能裂成两段)。
/// @param spec 日历规格
/// @param from 编译起点
/// @param to   编译终点 (开区间)
/// @return 有序不重叠的 Timeline; 无班次/无工作日/范围无效时为空
Timeline Timeline::build(const CalendarSpec& spec, Seconds from, Seconds to) {
  std::vector<std::pair<Seconds, Seconds>> raw;
  if (spec.shifts.empty() || spec.workdays.empty() || to <= from) return Timeline{};

  for (Seconds day = timeutil::dayStart(from); day <= to; day = timeutil::addDays(day, 1)) {
    int wd = timeutil::weekday(day);
    if (std::find(spec.workdays.begin(), spec.workdays.end(), wd) == spec.workdays.end())
      continue;
    for (const auto& sh : spec.shifts) {
      int eh = sh.endHour;
      if (eh <= sh.startHour) eh += 24;  // 跨夜班
      Seconds s = day + static_cast<Seconds>(sh.startHour) * 3600;
      Seconds e = day + static_cast<Seconds>(eh) * 3600;
      s = std::max(s, from);
      e = std::min(e, to);
      if (e > s) raw.emplace_back(s, e);
    }
  }
  if (raw.empty()) return Timeline{};

  std::sort(raw.begin(), raw.end());
  std::vector<std::pair<Seconds, Seconds>> merged;
  merged.push_back(raw[0]);
  for (size_t i = 1; i < raw.size(); ++i) {
    if (raw[i].first <= merged.back().second) {
      merged.back().second = std::max(merged.back().second, raw[i].second);
    } else {
      merged.push_back(raw[i]);
    }
  }

  // 扣除停机窗口 (检修/保养)
  for (const auto& dt : spec.downtime) {
    std::vector<std::pair<Seconds, Seconds>> next;
    for (const auto& seg : merged) {
      if (dt.second <= seg.first || dt.first >= seg.second) {
        next.push_back(seg);
        continue;
      }
      if (dt.first > seg.first) next.emplace_back(seg.first, dt.first);
      if (dt.second < seg.second) next.emplace_back(dt.second, seg.second);
    }
    merged.swap(next);
  }
  return Timeline(std::move(merged));
}

/// 判定时刻是否落在可用时段内。O(log n)。
/// @param t 待判定时刻
/// @return true = t 在某个 [first, second) 内
bool Timeline::contains(Seconds t) const {
  auto it = std::upper_bound(segs_.begin(), segs_.end(), t,
                             [](Seconds v, const std::pair<Seconds, Seconds>& s) {
                               return v < s.first;
                             });
  if (it == segs_.begin()) return false;
  --it;
  return t >= it->first && t < it->second;
}

/// 对齐可开工时刻 (二分)。t 在时段内原样返回, 否则顺延到下一时段开头。
Seconds Timeline::alignStart(Seconds t) const {
  // 二分定位: 找到第一个 first > t 的时段, 它的前一段可能包含 t
  auto it = std::upper_bound(segs_.begin(), segs_.end(), t,
                             [](Seconds v, const std::pair<Seconds, Seconds>& s) {
                               return v < s.first;
                             });
  if (it != segs_.begin()) {
    auto prev = std::prev(it);
    if (t < prev->second) return t;  // t 落在前一段内部
  }
  // t 不在任何时段内 -> 顺延到下一个时段开头
  return it != segs_.end() ? it->first : kInf;
}

/// 从 t 起消耗 dur 秒可用产能 (自动跨休息时段), 返回完工时刻。
/// 先二分跳过结束时刻 <= t 的时段, 再逐段扣减剩余量。
Seconds Timeline::advance(Seconds t, Seconds dur) const {
  if (dur <= 0) return contains(t) ? t : alignStart(t);
  // 跳过所有结束时刻 <= t 的时段 (时段按 first 有序且不重叠, second 也有序)
  auto it = std::lower_bound(segs_.begin(), segs_.end(), t,
                             [](const std::pair<Seconds, Seconds>& s, Seconds v) {
                               return s.second <= v;
                             });
  Seconds remain = dur;
  Seconds cursor = t;
  for (; it != segs_.end(); ++it) {
    Seconds st = std::max(cursor, it->first);
    if (st >= it->second) continue;
    Seconds avail = it->second - st;
    if (avail >= remain) return st + remain;
    remain -= avail;
    cursor = it->second;
  }
  return kInf;
}

/// 从 t 往回推 dur 秒可用产能, 返回占用起点 (二分定位起点后倒着走)。
/// 用于"已知完工时刻反推开工时刻", 绝不能用 t-dur 的时钟减法 (跨班次必错)。
Seconds Timeline::rewind(Seconds t, Seconds dur) const {
  if (dur <= 0) return t;
  // 从最后一个 first < t 的时段倒着走 (二分定位起点)
  auto it = std::upper_bound(segs_.begin(), segs_.end(), t,
                             [](Seconds v, const std::pair<Seconds, Seconds>& s) {
                               return v < s.first;
                             });
  Seconds remain = dur;
  Seconds cursor = t;
  while (it != segs_.begin()) {
    --it;
    Seconds hi = std::min(cursor, it->second);
    Seconds lo = it->first;
    if (hi <= lo) continue;
    Seconds avail = hi - lo;
    if (avail >= remain) return hi - remain;
    remain -= avail;
    cursor = lo;
  }
  return kInf;
}

/// 计算 [a,b) 内实际可用秒数。只扫与区间相交的时段 (二分定位第一个相交段)。
Seconds Timeline::capacity(Seconds a, Seconds b) const {
  if (b <= a) return 0;
  // 只扫与 [a,b) 相交的时段: 第一个 second > a 的时段开始
  auto it = std::lower_bound(segs_.begin(), segs_.end(), a,
                             [](const std::pair<Seconds, Seconds>& s, Seconds v) {
                               return s.second <= v;
                             });
  Seconds sum = 0;
  for (; it != segs_.end() && it->first < b; ++it) {
    Seconds lo = std::max(a, it->first);
    Seconds hi = std::min(b, it->second);
    if (hi > lo) sum += hi - lo;
  }
  return sum;
}

// ---------------------------------------------------------------------------
// SetupMatrix
// ---------------------------------------------------------------------------

/// 初始化换型族与方阵; 行列不足的部分用 defaultSec_ 补齐保证方阵。
/// @param families 族名列表 (空 = 不建模换型)
/// @param times    换型秒数方阵 times_[from][to]
void SetupMatrix::init(const std::vector<std::string>& families,
                       const std::vector<std::vector<Seconds>>& times) {
  families_ = families;
  index_.clear();
  for (size_t i = 0; i < families_.size(); ++i) index_[families_[i]] = static_cast<int>(i);
  times_ = times;
  // 保证方阵
  for (auto& row : times_) row.resize(families_.size(), defaultSec_);
  times_.resize(families_.size(), std::vector<Seconds>(families_.size(), defaultSec_));
}

/// 把仍为 0 的矩阵项设为默认换型时间 (铺底; 显式设 0 的"同族 0"会被覆盖, 注意调用顺序)。
void SetupMatrix::setDefault(Seconds sec) {
  defaultSec_ = sec;
  for (auto& row : times_)
    for (auto& v : row)
      if (v == 0) v = sec;
}

/// 族名 -> 索引 (哈希查询); 未注册返回 -1。
int SetupMatrix::indexOf(const std::string& family) const {
  auto it = index_.find(family);
  return it == index_.end() ? -1 : it->second;
}

/// 查询换型时间; from/to 为 -1 (设备空闲/首次装夹) 时用同族时间近似; 越界返回 0。
Seconds SetupMatrix::get(int from, int to) const {
  if (times_.empty()) return 0;
  if (from < 0) from = to;  // 首次装夹: 用同族换型时间近似
  if (to < 0) to = from;
  if (from < 0 || to < 0) return 0;
  if (from >= static_cast<int>(times_.size()) || to >= static_cast<int>(times_.size())) return 0;
  return times_[from][to];
}

// ---------------------------------------------------------------------------
// Problem
// ---------------------------------------------------------------------------

/// 按产品编码查工艺路线 (线性扫描, 路线数量少, 不是热路径)。
const Routing* Problem::findRouting(const std::string& product) const {
  for (const auto& r : routings)
    if (r.product == product) return &r;
  return nullptr;
}

/// 按编码查设备下标; 未找到返回 -1。
int Problem::findMachine(const std::string& code) const {
  for (size_t i = 0; i < machines.size(); ++i)
    if (machines[i].code == code) return static_cast<int>(i);
  return -1;
}

/// 按订单号查下标; 未找到返回 -1。
int Problem::findOrder(const std::string& code) const {
  for (size_t i = 0; i < orders.size(); ++i)
    if (orders[i].code == code) return static_cast<int>(i);
  return -1;
}

/// 订单 x 工艺路线 -> 扁平工序数组。
/// 每张订单查它的工艺路线, 逐道展开成 OpInst; 指向宕机设备的选项被过滤,
/// 过滤后无可选设备的工序整道丢弃 (该订单工序链变短, 依然合法)。
/// 调用时机: orders / routings / machines.down 任何一项变化后。
void Problem::rebuild() {
  ops.clear();
  orderOps.assign(orders.size(), {});
  ops.reserve(orders.size() * 6);

  std::unordered_map<std::string, const Routing*> byProduct;
  for (const auto& r : routings) byProduct[r.product] = &r;

  for (size_t oi = 0; oi < orders.size(); ++oi) {
    const Order& o = orders[oi];
    auto it = byProduct.find(o.product);
    if (it == byProduct.end()) continue;  // 缺工艺路线 -> 该订单不参与排产
    const Routing* r = it->second;
    for (size_t k = 0; k < r->ops.size(); ++k) {
      const OperationDef& d = r->ops[k];
      // 过滤指向宕机设备的选项
      std::vector<MachineOption> opts;
      opts.reserve(d.options.size());
      for (const auto& mo : d.options) {
        if (mo.machine >= 0 && mo.machine < numMachines() && !machines[mo.machine].down)
          opts.push_back(mo);
      }
      if (opts.empty()) continue;
      OpInst inst;
      inst.order = static_cast<int>(oi);
      inst.seq = static_cast<int>(k);
      inst.code = d.code;
      inst.name = d.name;
      inst.family = d.family;
      inst.qty = o.qty;
      inst.options = std::move(opts);
      orderOps[oi].push_back(static_cast<int>(ops.size()));
      ops.push_back(std::move(inst));
    }
  }
}

/// 按厂级日历为每台设备生成 Timeline; 停机设备给空日历 (alignStart 直接 kInf)。
/// extraDays 的缓冲保证"跨过 horizon 末尾的长工序"也能在扩展日历里落地。
void Problem::buildCalendars(int extraDays) {
  Seconds to = horizonEnd + static_cast<Seconds>(extraDays) * 86400;
  for (auto& m : machines) {
    if (m.down) {
      m.cal = Timeline{};
    } else {
      m.cal = Timeline::build(calendar, horizonStart, to);
    }
  }
}

}  // namespace aps

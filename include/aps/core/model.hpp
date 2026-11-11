// ===========================================================================
// core/model.hpp -- 领域模型 (排产的输入)
//
// 建模对应关系 (FJSP, 柔性作业车间):
//   Order  订单      : 数量 / 发布时刻 / 交期 / 优先级  -- 排产需求
//   Routing 工艺路线  : 有序工序链, 由产品的 Routing 决定
//   OpInst 工序实例   : 订单 x 工艺步骤 展开后的最小排产单元 (本项目的"任务")
//   Machine 设备      : 工位/机台, 带日历 (班次/停机)
//   SetupMatrix 换型  : 相邻工序 family 变化时的准备时间
//
// 时间一律 Seconds (int64 Unix 秒)。
// 数据流向: 调用方填 orders/routings/machines/calendar -> rebuild() 展开工序
//          -> buildCalendars() 编译日历 -> 交给求解器。
// ===========================================================================
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "aps/util/time_util.hpp"

namespace aps {

// ---- 日历 ----------------------------------------------------------------

/// @brief 一个班次: 从 startHour 点到 endHour 点 (24 小时制)。
///        endHour <= startHour 表示跨夜班 (如 22 -> 6), 编译 Timeline 时会自动 +24h。
struct Shift {
  int startHour = 8;
  int endHour = 16;
};

/// @brief 厂级日历规格 (设备各自的最终可用区间由 Timeline::build 编译生成)。
struct CalendarSpec {
  /// 每周工作日, 0=周日 1=周一 ... 6=周六; 不在列表中的日子整日无产能
  std::vector<int> workdays{1, 2, 3, 4, 5};
  /// 每天的班次列表 (午休等断档用多个班次表达, 如 8-12 + 13-17)
  std::vector<Shift> shifts{{8, 12}, {13, 17}};
  /// 计划检修/停机窗口 [开始, 结束), 编译时从可用区间里扣除
  std::vector<std::pair<Seconds, Seconds>> downtime;
};

/// @brief 把"班次 + 工作日 + 停机"编译成一段段**有序不重叠**的可用区间 [first, second)。
/// 之后所有时间推算 (对齐/前进/回退/容量) 都是二分查找 —— 解码器里没有日期运算,
/// 只有区间游标, 这是排产能跑到千级工序的关键。
class Timeline {
 public:
  Timeline() = default;

  /// @brief 直接用预编译好的区间构造 (要求有序不重叠, 一般只由 build() 调用)。
  /// @param segs 可用区间列表, 语义为 [first, second) 左闭右开, 单位秒
  explicit Timeline(std::vector<std::pair<Seconds, Seconds>> segs) : segs_(std::move(segs)) {}

  /// @brief 把日历规格编译成 [from, to + extraDays) 范围内的可用区间表。
  /// @param spec 班次/工作日/停机定义
  /// @param from 编译起点 (Unix 秒, 通常为 horizonStart 所在当天 0 点)
  /// @param to   编译终点 (Unix 秒, 开区间)
  /// @return     有序不重叠的 Timeline; 规格为空或范围无效时返回空 Timeline
  static Timeline build(const CalendarSpec& spec, Seconds from, Seconds to);

  /// @return 是否没有任何可用时段 (空日历, 常见于停机设备)
  bool empty() const { return segs_.empty(); }

  /// @param t 待判定的时刻 (Unix 秒)
  /// @return t 是否落在某个可用时段内部
  bool contains(Seconds t) const;

  /// @brief 对齐到可开工时刻。
  /// @param t 期望开工时刻
  /// @return t 在时段内则原样返回; 否则顺延到下一个时段的开头; 再无时段返回 kInf
  Seconds alignStart(Seconds t) const;

  /// @brief 从 t 起连续消耗 dur 秒**可用产能** (自动跨过休息时段), 返回完工时刻。
  /// @param t   起点时刻 (不必在时段内, 会先对齐)
  /// @param dur 需要消耗的可用秒数 (换型+加工)
  /// @return    完工时刻 (Unix 秒); 产能不足以在 horizon 内完成返回 kInf
  /// @note 复杂度 O(log n + 跨越的时段数), n 为时段数
  Seconds advance(Seconds t, Seconds dur) const;

  /// 从 t 往回推 dur 秒可用产能, 返回真实占用起点。
  /// 用于"已知完工时刻反推开工时刻"—— 跨班次时绝不能用 `t - dur` 的时钟减法,
  /// 否则起点会落进非工作时段。
  /// @param t   完工时刻
  /// @param dur 需要回推的可用秒数
  /// @return    占用起点 (Unix 秒); 可用产能不足返回 kInf
  Seconds rewind(Seconds t, Seconds dur) const;

  /// @brief 计算区间内实际可用产能。
  /// @param a 起点 (含)
  /// @param b 终点 (不含)
  /// @return  [a,b) 与可用时段的交集总秒数; b<=a 时为 0
  Seconds capacity(Seconds a, Seconds b) const;

  /// @return 底层区间表 (只读), 供导出/调试
  const std::vector<std::pair<Seconds, Seconds>>& segments() const { return segs_; }

 private:
  /// @brief 可用区间表, 保证按 first 升序且互不重叠
  std::vector<std::pair<Seconds, Seconds>> segs_;
};

// ---- 换型矩阵 ------------------------------------------------------------

/// @brief 换型时间矩阵: (换型族A -> 换型族B) 的准备秒数, 对角线(同族)通常为 0。
class SetupMatrix {
 public:
  /// @brief 初始化换型族与完整方阵。
  /// @param families 换型族名列表 (顺序即索引); 为空表示"不建模换型", get() 恒为 0
  /// @param times    n x n 矩阵; 不足的行/列会用 defaultSec_ 补齐
  void init(const std::vector<std::string>& families,
            const std::vector<std::vector<Seconds>>& times);

  /// @brief 把矩阵里仍为 0 的项设为默认换型时间 (通常在 init 后调用, 用于铺底)。
  /// @param sec 默认换型秒数 (不同族之间的兜底值)
  void setDefault(Seconds sec);

  /// @param family 换型族名
  /// @return 族索引; 未注册返回 -1
  int indexOf(const std::string& family) const;

  /// 查询换型时间。
  /// @param from 前道工序的换型族索引; -1 表示设备空闲/首次装夹 (用同族时间近似)
  /// @param to   后道工序的换型族索引; -1 同上
  /// @return     准备秒数; 索引越界或矩阵未初始化返回 0
  Seconds get(int from, int to) const;

  /// @return 已注册的换型族数量
  int size() const { return static_cast<int>(families_.size()); }

  /// @return 换型族名列表 (顺序与索引一致)
  const std::vector<std::string>& families() const { return families_; }

 private:
  std::vector<std::string> families_;     ///< 族名, 下标即族索引
  std::map<std::string, int> index_;      ///< 族名 -> 索引, O(log n) 查询
  std::vector<std::vector<Seconds>> times_;  ///< 换型秒数方阵 times_[from][to]
  Seconds defaultSec_ = 0;                ///< init 补齐用的默认换型时间
};

// ---- 主数据 --------------------------------------------------------------

/// @brief 工序的一个可选设备项: "这道工序可以在这台设备上做, 单件要这么久"。
struct MachineOption {
  int machine = 0;           ///< machines 数组下标
  double unitSeconds = 0.0;  ///< 单件工时(秒/件), 允许小数 (如 0.36 秒/张卡片)
};

/// @brief 工艺路线里的一道工序定义 (模板, 排产时按订单展开成 OpInst)。
struct OperationDef {
  std::string code;    ///< 工序代码, 如 "PRINT"
  std::string name;    ///< 显示名, 如 "印刷"
  std::string family;  ///< 换型组, 如 "PRINT-A" / "DIE-M"; 决定换型时间
  std::vector<MachineOption> options;  ///< 可选设备组 (柔性所在, >=1 项)
};

/// @brief 工艺路线: 一个产品从投料到完工的完整有序工序链。
struct Routing {
  std::string product;                 ///< 产品编码 (订单通过它引用路线)
  std::vector<OperationDef> ops;       ///< 有序工序定义, seq 从 0 开始
};

/// @brief 销售订单: 排产的需求侧输入。
struct Order {
  std::string code;       ///< 订单号, 全局唯一 (如 "SO-1001")
  std::string product;    ///< 产品编码, 必须能找到对应 Routing
  long long qty = 0;      ///< 生产数量 (件)
  Seconds release = 0;    ///< 订单发布时间: 最早可开工时刻
  Seconds due = 0;        ///< 预期发货时间 (交期), 延期即 tardiness
  int priority = 3;       ///< 优先级 1..5, 5 最急; 映射到延期权重
  bool urgent = false;    ///< 急单标记: 权重 x1.5, 插队场景
  std::string note;       ///< 备注 (自由文本)
};

/// @brief 设备/工位。
struct Machine {
  std::string code;  ///< 设备编码, 全局唯一 (如 "P1")
  std::string group; ///< 设备组 (如 PRINT/DIE), 故障迁移时在同组内找替身
  std::string name;  ///< 显示名, 如 "四色印刷机 1"
  Timeline cal;      ///< 编译好的可用区间表 (buildCalendars 生成; down 时为空)
  bool down = false; ///< 停机标记: true 时日历清空, 新工序不会排上它
};

// ---- 问题实例 ------------------------------------------------------------

/// @brief 待排工序实例 = 订单 x 工艺步骤 展开后的最小排产单元。
/// @brief Problem::ops 是扁平数组 (下标即 op id), 解码器只看它, 缓存友好。
struct OpInst {
  int order = 0;   ///< 所属订单在 orders 里的下标
  int seq = 0;     ///< 工艺内序号 (0 开始), 与 orderOps[order] 的位置一致
  std::string code;    ///< 工序代码 (继承自 OperationDef)
  std::string name;    ///< 显示名
  std::string family;  ///< 换型组
  long long qty = 0;   ///< 本工序要处理的数量 (= 订单数量, 第一版不拆批)
  std::vector<MachineOption> options;  ///< 本订单可用的设备选项 (已过滤宕机设备)
};

/// @brief 目标函数与惩罚的权重配置。
struct Weights {
  double tardiness = 10.0;  ///< 延期小时权重 (交期 > 换型 > 总工期)
  double setup = 3.0;       ///< 换型分钟权重
  double makespan = 1.0;    ///< 完工跨度小时权重
  double stability = 0.0;   ///< 重排时的计划漂移惩罚权重 (0 = 只报数不进目标)
  /// @brief 优先级(1..5) -> 延期权重放大系数; 下标 0 弃用
  double priorityWeight[6] = {0.0, 0.6, 0.8, 1.0, 1.4, 2.0};
};

/// @brief 排产问题实例: 主数据 + 展开后的工序 + 日历 + 权重。
///        求解器只读它; 修改 orders/routings 后必须 rebuild(), 改日历/停机后必须 buildCalendars()。
struct Problem {
  std::string name;              ///< 算例名 (演示/导出时展示)
  std::vector<Order> orders;     ///< 订单列表
  std::vector<Machine> machines; ///< 设备列表
  std::vector<Routing> routings; ///< 工艺路线列表 (product 唯一)
  std::vector<OpInst> ops;       ///< 展开后的待排工序 (rebuild 生成)
  std::vector<std::vector<int>> orderOps;  ///< 订单下标 -> 该订单的 op 下标序列
  SetupMatrix setup;             ///< 换型矩阵
  Weights weights;               ///< 目标函数权重
  CalendarSpec calendar;         ///< 厂级默认日历 (编译进 Machine::cal)

  Seconds horizonStart = 0;      ///< 排产窗口起点 (Unix 秒)
  Seconds horizonEnd = 0;        ///< 排产窗口终点; 排不进窗口的工序标记 truncated

  /// 订单 -> 工艺 展开: 重建 ops 与 orderOps。
  /// 没有工艺路线的订单会被跳过; 选项指向宕机设备的工序选项会被过滤,
  /// 过滤后无任何可选设备的工序整道丢弃。
  void rebuild();

  /// 按厂级日历重新生成每台设备的 Timeline。
  /// @param extraDays 在 horizonEnd 之外再多编译的天数 (缓冲, 保证跨窗口工序也能落地)
  void buildCalendars(int extraDays = 30);

  /// @return 订单数量
  int numOrders() const { return static_cast<int>(orders.size()); }
  /// @return 待排工序总数 (rebuild 之后才有效)
  int numOps() const { return static_cast<int>(ops.size()); }
  /// @return 设备数量
  int numMachines() const { return static_cast<int>(machines.size()); }

  /// @param product 产品编码
  /// @return 对应工艺路线指针; 不存在返回 nullptr
  const Routing* findRouting(const std::string& product) const;

  /// @param code 设备编码
  /// @return 设备下标; 不存在返回 -1
  int findMachine(const std::string& code) const;

  /// @param code 订单号
  /// @return 订单下标; 不存在返回 -1
  int findOrder(const std::string& code) const;
};

}  // namespace aps

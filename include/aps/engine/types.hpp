// ===========================================================================
// engine/types.hpp -- 求解结果的数据结构
//
// Assignment 是"一道工序定在哪台设备、什么时候干"。
// Solution   是全体 Assignment + KPI + 可行性结论, 所有求解器都输出它,
//            这样规则解 / GA / 重排可以在同一张表里比较, 前端也只认这一种格式。
//
// 三对容易混淆的时间字段 (详见各字段注释):
//   start/end   : 时钟上看到的加工起止 (甘特图横轴)
//   occStart    : 机器真实被占用的起点 (含换型, 换型可跨班次)
//   workSec     : 纯加工产能秒数 (利用率/校验用)
// ===========================================================================
#pragma once

#include <string>
#include <vector>

#include "aps/core/model.hpp"

namespace aps {

/// 一道工序的最终安排: "哪台设备、哪个换型、什么时间干"。
struct Assignment {
  int op = -1;        ///< Problem::ops 下标; -1 表示未安排的占位项
  int machine = -1;   ///< Problem::machines 下标; -1 表示未选设备
  Seconds start = 0;  ///< 加工开始时刻 (换型结束后)
  Seconds end = 0;    ///< 加工结束时刻 (跨班次时 start = rewind(end, work), 不是简单减法)
  Seconds setup = 0;  ///< 本工序前的换型准备秒数
  /// 机器开始被占用的时刻。跨班次时 start-setup 只是"时钟回退", 并非真实占用起点
  /// (准备阶段可能被午休/夜班切开), 所以必须显式记录, 否则日历校验会误判。
  Seconds occStart = -1;
  /// 纯加工产能秒数 (= 数量 x 单件工时)。跨班次时 end-start 是时钟跨度, 会大于它,
  /// 甘特图按 start/end 画宽度, 指标统计必须用 workSec, 否则利用率会虚高。
  Seconds workSec = 0;
  bool frozen = false;  ///< 已开工/已锁定: 重排时时间与设备都不许动

  /// @return 机器真实占用起点; 未记录 occStart 时退回 start-setup (旧数据兼容)
  Seconds occupancyStart() const { return occStart >= 0 ? occStart : start - setup; }

  /// @return 时钟跨度 end-start (含跨班次空闲, 只用于展示, 别用于统计)
  Seconds duration() const { return end - start; }

  /// @return 纯加工秒数; 未记录 workSec 时退回 end-start (旧数据兼容)
  Seconds work() const { return workSec > 0 ? workSec : end - start; }
};

/// 排产评价指标集合 (口径统一由 Metrics 计算, 求解器不自己算)。
struct KPI {
  int ordersTotal = 0;   ///< 参与统计的订单总数
  int ordersOnTime = 0;  ///< 完工 <= 交期的订单数
  int ordersLate = 0;    ///< 延期订单数 (= ordersTotal - ordersOnTime)
  double otd = 1.0;      ///< 准时交付率 = ordersOnTime / ordersTotal, [0,1]

  Seconds totalTardiness = 0;  ///< Σ max(0, 完工-交期), 未加权
  Seconds maxTardiness = 0;    ///< 单笔最大延期秒数
  double weightedTardinessHours = 0.0;  ///< Σ 延期小时 x 优先级权重 (目标函数用)

  Seconds makespan = 0;      ///< 末工序结束时刻 (绝对时间)
  Seconds makespanSpan = 0;  ///< 首工序占用起点 -> 末工序结束的跨度 (进目标函数的口径)

  int setupCount = 0;    ///< 换型发生次数 (相邻工序换型族变化)
  Seconds setupSec = 0;  ///< 换型总秒数

  double avgUtilization = 0.0;          ///< 平均设备利用率 [0,1] = 纯加工秒/可用秒
  std::vector<double> machineUtilization;  ///< 每台设备的利用率, 下标与 machines 对齐

  int peakWip = 0;  ///< 任意时刻"已开工未完工"工序数的峰值

  double objective = 0.0;     ///< 目标函数值 (Metrics::cost, 越小越好)
  double solveMs = 0.0;       ///< 求解耗时 (毫秒)
  long long evaluations = 0;  ///< 解码次数 (GA 评价个体数, 衡量搜索量)
};

/// 求解结果: 所有求解器 (规则/GA/重排) 的统一输出格式。
struct Solution {
  std::string solver = "none";  ///< 求解器标签: "rule:EDD" / "ga" / "reschedule:insert" 等
  std::vector<Assignment> assign;  ///< 工序安排表, 下标 = op id
  /// 实际采用的派工顺序 (op 下标)。用于把"解"无损还原成编码:
  /// GA 以规则解为种子、重排时重建工序序列, 都靠它, 而不是"按开工时间重排"
  /// (那样在同时刻并列时会把顺序打乱, 插入式解码结果会明显变差, 实测差 50%)。
  std::vector<int> opOrder;
  KPI kpi;                      ///< 评价指标
  bool feasible = false;        ///< 可行性结论 (以 validate() 结果为准, 不由求解器自称)
  std::string infeasibleReason; ///< 不可行时的人读原因
  bool truncated = false;       ///< true = 有工序排不进排产窗口 (horizon 太小/产能耗尽)

  /// @return 是否还没有任何结果 (assign 为空)
  bool empty() const { return assign.empty(); }

  /// @return 已安排的工序数量
  int size() const { return static_cast<int>(assign.size()); }
};

}  // namespace aps

// ===========================================================================
// engine/decoder.hpp -- 解码器 (OS + MS -> 具体排产表)
//
// 这是整个系统最核心的一段代码。所有求解器最终都产出一个"工序优先序列",
// 由解码器翻译成时间表。规则解与 GA 共用它, 保证两者在同一评价标准下可比。
//
// 解码策略: 插入式 (insertion) + 换型感知
//   按序列顺序逐道工序处理, 在目标设备上从左往右找**第一个塞得下的位置**,
//   而不是简单追加到队尾。
//
//   为什么"第一个可行位置"就是最优?
//     位置 pos 越靠后, 该位置之前的占用结束时刻只会更晚 (prevEnd 单调不减),
//     所以工序能被安排的最早开工时刻随 pos 单调不减 => 第一个可行位置即最早完工。
//     这条单调性让插入变成 O(设备队列长度) 的一次扫描, 无需回溯。
//
// 与"追加式"的差距: 追加式会把工序排到设备队尾, 白白拉长交期;
// 插入式能填进前面的空隙, 解质量高一个档次。
//
// 附带保证: 输出前有一道"顺序自愈" (见 decoder.cpp), 任何编码进来都能得到
// 结构完整、无重叠、满足工艺链的排产表 —— 求解器不需要自己保证可行性。
// ===========================================================================
#pragma once

#include <vector>

#include "aps/engine/types.hpp"

namespace aps {

/// @brief 插入式解码器。线程安全: 只读 Problem, 多线程 GA 各自持有一份实例。
class Decoder {
 public:
  /// @brief 构造函数，必须显式构造
  /// @param p 问题实例 (只读引用, 生命周期必须覆盖解码器的使用期)
  explicit Decoder(const Problem& p) : p_(p) {}

  /// @brief 按 OS+MS 双段编码解码 (GA 主用)。
  /// @param os               OS 编码: os[i] = 订单下标, 第 k 次出现 = 该订单第 k 道工序
  /// @param ms               MS 编码: ms[op] = 该工序使用 options 里的第几个设备;
  ///                         允许越界/负值 (内部会取模/取绝对值兜底)
  /// @param chooseBestMachine true=忽略 ms, 每道工序遍历所有可选设备取最早完工
  ///                         (评价"设备选得好不好"时用); false=严格按 ms 放置
  /// @param buildAssign      true=回填 Assignment + 全量 KPI (最终结果);
  ///                         false=只算目标值 (GA 内循环热路径, 快 3~5 倍)
  /// @return 排产结果; buildAssign=false 时 assign 为空, 只有 kpi.objective 可信
  Solution decodeOs(const std::vector<int>& os, const std::vector<int>& ms,
                    bool chooseBestMachine = false, bool buildAssign = true) const;

  /// @brief 按工序下标顺序直接解码 (规则法主用, 重排复放也走这里)。
  /// @param opOrder          派工顺序: op 下标序列; 重复/越界项会被跳过,
  ///                         缺失项追加到末尾兜底, 任何输入都不丢工序
  /// @param ms               设备选择, 含义同 decodeOs; 空表 + chooseBestMachine=true
  ///                         表示让解码器自己选设备
  /// @param chooseBestMachine true=每工序遍历可选设备取最早完工
  /// @param tag              写进结果的求解器标签 (如 "rule:EDD" / "ga"), 便于追溯
  /// @param buildAssign      true=回填 Assignment + 全量 KPI; false=只算目标值
  /// @return 排产结果; opOrder 字段为实际放置顺序 (回放种子/重排复放都靠它)
  Solution decodeSequence(const std::vector<int>& opOrder, const std::vector<int>& ms,
                          bool chooseBestMachine, const std::string& tag,
                          bool buildAssign = true) const;

  /// @brief 把 OS 编码展开成完整工序顺序。
  /// @param os 订单号序列 (可有重复/缺失/越界)
  /// @return op 下标序列: 每个订单按出现次数展开其工艺链; 缺失的工序按编号补到末尾,
  ///         保证任何 OS 都不丢工序 (GA 交叉变异后不需要修补)
  std::vector<int> expandOs(const std::vector<int>& os) const;

  /// @brief 一次放置的计算结果 (在指定设备上找到的最早可行位置)。
  struct Placement {
    bool ok = false;      ///< 是否找到可行位置 (false = 该设备日历内排不下)
    int pos = -1;         ///< 插入到设备队列的下标 (队列按时间升序)
    Seconds start = 0;    ///< 加工开始时刻 (换型结束后)
    Seconds end = 0;      ///< 加工结束时刻
    Seconds setup = 0;    ///<(本位置需要的)换型准备秒数
    Seconds occStart = 0; ///< 机器开始被占用的时刻 (换型起点, 可能跨班次)
    Seconds work = 0;     ///< 纯加工秒数 (qty x unitSeconds)
  };

 private:
  const Problem& p_;  ///< 问题实例 (只读)
};

}  // namespace aps

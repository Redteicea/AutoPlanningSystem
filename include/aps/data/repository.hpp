// ---------------------------------------------------------------------------
// data/repository.hpp -- 服务端内存状态
//
// 一个 APS 后台服务要持有: 主数据 (订单/设备/工艺) + 最近一次排产结果 + 事件流。
// 这里用进程内内存 + 粗粒度锁。第一版不接数据库 (见 06-演进路线), 但所有
// 对外结构都走 JSON, 换成 SQLite/MySQL 时只改这一层。
//
// 线程模型: HTTP 工作线程共享 Repository, 调用方负责持锁 (lock()); 注意不要
// 在持锁期间做长时间求解 —— 求解用局部 Problem 副本。
// ---------------------------------------------------------------------------
//
// 内容概要:
//   - Event: 一条操作/事件记录 (时间/类型/详情)
//   - Repository: 进程内内存状态 (主数据 + 最近排产 + 事件) + 粗粒度锁
//   - problemToJson / problemFromJson: Problem 与 JSON 的导入导出
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "aps/data/demo_data.hpp"
#include "aps/engine/types.hpp"
#include "aps/util/json.hpp"

namespace aps {

/// @brief 一条操作/事件记录 (谁在什么时候对系统做了什么)。
///
/// 由 Repository::logEvent 追加, 前端"事件流"面板按时间倒序展示;
/// 也用于事后追查 (例如某次排产用了什么算法、设备何时停机)。
struct Event {
  Seconds at = 0;       // 发生时刻 (Unix 秒)
  std::string type;     // 类型 (init/solve/order/insert/breakdown/restore...)
  std::string detail;   // 人类可读详情
};

/// @brief 服务端唯一的共享状态容器: 当前主数据 + 最近一次排产结果 + 事件流。
///
/// 一个 Repository 实例被全部 HTTP 工作线程共享, 因此:
///   - 读写 problem()/latest()/events() 前必须自己持 mutex() (粗粒度锁, 简单优先);
///   - 绝不在持锁期间跑求解 (几秒的 GA 会把整站卡死), 正确做法是先拷一份 Problem
///     算完再持锁 setLatest()。
/// 内部不接数据库: 数据只在进程内存里, 重启即回到演示算例; 换成 SQLite 时只改本类。
class Repository {
 public:
  Repository() = default;

  /// @brief 用演示工厂重建主数据 (订单数/窗口天数/种子/是否夜班)。
  /// @param orderCount   订单数 (<=0 回退 80)
  /// @param horizonDays  排产窗口天数 (<=0 回退 14)
  /// @param seed         随机种子
  /// @param twoShifts    是否加夜班 (产能翻倍)
  void initDemo(int orderCount, int horizonDays, std::uint64_t seed, bool twoShifts);

  /// @brief 重载: 直接用生成参数重建主数据 (可携带配置文件的自定义日历)。
  /// @param cfg 生成参数 (orderCount/horizonDays 非法值按旧约定回退)
  void initDemo(const DemoConfig& cfg);

  /// @brief 读写访问当前主数据 (调用方需持锁)。
  Problem& problem() { return problem_; }
  /// @brief 只读访问当前主数据。
  const Problem& problem() const { return problem_; }

  /// @brief 读写访问最近一次排产结果 (调用方需持锁)。
  Solution& latest() { return latest_; }
  /// @brief 只读访问最近一次排产结果。
  const Solution& latest() const { return latest_; }
  /// @brief 写回排产结果并记录所用算法与写回时刻。
  void setLatest(Solution s, const std::string& algo);

  /// @brief 追加一条事件到事件流 (超过 200 条时裁剪旧的 100 条)。
  void logEvent(const std::string& type, const std::string& detail);
  /// @brief 只读事件流。
  const std::vector<Event>& events() const { return events_; }

  /// @brief 主数据版本号: 求解前记录, 写回前比对, 防止并发修改导致 op 索引错位。
  std::uint64_t generation() const { return generation_; }
  /// @brief 递增版本号 (主数据发生变化时调用)。
  void bump() { ++generation_; }

  // ---- 序列化给前端 ----
  /// @brief 主数据 (订单/设备/工艺/换型族/统计) 序列化为 JSON。
  json::Value problemJson() const;
  /// @brief 排产结果序列化为 JSON; includeTasks 控制是否展开工序级 tasks。
  json::Value solutionJson(bool includeTasks = true) const;
  /// @brief 事件流序列化为 JSON 数组。
  json::Value eventsJson() const;
  /// @brief 服务端摘要 (统计 + 最近算法 + 写回时刻) 序列化为 JSON。
  json::Value summaryJson() const;

  /// @brief 返回内部互斥锁, 供调用方在读写主数据/结果时加锁。
  std::mutex& mutex() { return mu_; }

 private:
  mutable std::mutex mu_;     // 保护下方全部状态的粗粒度锁
  Problem problem_;           // 当前主数据
  Solution latest_;           // 最近一次排产结果
  std::vector<Event> events_; // 事件流
  std::string lastAlgo_;      // 最近使用的算法名
  Seconds lastSolveAt_ = 0;   // 最近写回时刻
  std::uint64_t generation_ = 0; // 主数据版本号
};

/// @brief Problem 的 JSON 存取 (用于导入/导出算例)
/// @brief 将 Problem 完整序列化为 JSON (含订单/设备/工艺/换型族/统计)。
json::Value problemToJson(const Problem& p);
/// @brief 从 JSON 反序列化 Problem; 失败写入 err 并返回 false。
bool problemFromJson(const json::Value& v, Problem& p, std::string& err);

}  // namespace aps

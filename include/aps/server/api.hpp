// ---------------------------------------------------------------------------
// server/api.hpp -- REST 路由
//
// 端点一览 (详见 docs/03-API接口.md):
//   GET  /api/health                 健康检查
//   GET  /api/state                  主数据 + 统计
//   GET  /api/schedule               最近一次排产结果
//   POST /api/schedule               执行排产 {algo:"rule"|"ga", rule:"AUTO", timeLimitMs}
//   POST /api/demo/reset             重建演示算例
//   POST /api/orders                 追加订单
//   POST /api/reschedule/insert      急单插队
//   POST /api/reschedule/breakdown   设备故障
//   POST /api/machines/:code/restore 设备恢复
//   GET  /api/events                 事件流
//   POST /api/benchmark              规模-性能评测
//   GET  /api/export                 导出当前算例
// ---------------------------------------------------------------------------
//
//   - Api: 把各 REST 端点绑定到 HttpServer, 内部持有 Repository 引用
//   - 公开: registerRoutes (注册全部路由)
//   - 私有: 各端点的处理函数 (health/state/solve/reset/insert/breakdown/...)
//
#pragma once

#include "aps/data/repository.hpp"
#include "aps/server/http_server.hpp"

namespace aps {

class Api {
 public:
  /// 构造 API 处理器, 绑定共享的 Repository (不拥有其生命周期)。
  explicit Api(Repository& repo) : repo_(repo) {}
  /// 将全部 REST 端点注册到给定 HttpServer (在 listen 之前调用)。
  void registerRoutes(HttpServer& srv);

 private:
  /// 健康检查端点: 返回服务名/版本/服务器时间。
  HttpResponse health(const HttpRequest& req);
  /// 主数据 + 统计快照: 返回 summary 与 problem。
  HttpResponse state(const HttpRequest& req);
  /// 最近一次排产结果 (含工序级 tasks)。
  HttpResponse getSchedule(const HttpRequest& req);
  /// 执行排产 (rule/auto/ga) 并写回结果, 含独立可行性校验。
  HttpResponse solve(const HttpRequest& req);
  /// 按参数重建演示算例。
  HttpResponse resetDemo(const HttpRequest& req);
  /// 追加一个新订单 (含工艺/重复号校验)。
  HttpResponse addOrder(const HttpRequest& req);
  /// 急单插队重排 (冻结已开工工序)。
  HttpResponse insertOrder(const HttpRequest& req);
  /// 设备故障重排 (将该设备未开工工序迁出)。
  HttpResponse breakdown(const HttpRequest& req);
  /// 设备恢复上线 (仅置状态, 提示重新排产)。
  HttpResponse restore(const HttpRequest& req);
  /// 事件流列表。
  HttpResponse events(const HttpRequest& req);
  /// 规模-性能评测 (rule vs ga 多档对比)。
  HttpResponse benchmark(const HttpRequest& req);
  /// 导出当前算例为 JSON。
  HttpResponse exportProblem(const HttpRequest& req);

  Repository& repo_;
};

}  // namespace aps

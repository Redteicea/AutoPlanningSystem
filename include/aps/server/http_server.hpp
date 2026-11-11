// ---------------------------------------------------------------------------
// server/http_server.hpp -- 极简 HTTP/1.1 服务 + 线程池
//
// 路由 (含 :param 路径参数) / 线程池 / 静态文件 / keep-alive: close。
// 非目标: HTTPS / chunked / 文件上传 / WebSocket。
// ---------------------------------------------------------------------------
//
//   - HttpRequest: 一次请求 (方法/路径/query/body/头/路径参数/query参数)
//   - HttpResponse: 一次响应 (状态码/Content-Type/body) + 便捷构造
//   - HttpServer: 监听 + 线程池 + 路由表 + 静态目录服务
//
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace aps {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string query;
  std::string body;
  std::map<std::string, std::string> headers;
  std::map<std::string, std::string> params;  // 路径参数, 如 /api/machines/:code
  std::map<std::string, std::string> queryParams;

  /// 读取路径参数 (路由 :param 占位被实际值填充后存入 params)。
  ///
  /// @param k   参数名
  /// @param def 缺失时的默认值 (空串)
  /// @return    参数值或 def
  std::string param(const std::string& k, const std::string& def = "") const {
    auto it = params.find(k);
    return it == params.end() ? def : it->second;
  }
  /// 读取请求头, 按 key 大小写无关匹配 (HTTP 头名不区分大小写)。
  ///
  /// @param k 头名
  /// @return  头值; 未找到返回空串
  std::string header(const std::string& k) const {
    for (const auto& kv : headers) {
      // 大小写无关
      if (kv.first.size() != k.size()) continue;
      bool same = true;
      for (size_t i = 0; i < k.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(kv.first[i])) !=
            std::tolower(static_cast<unsigned char>(k[i]))) {
          same = false;
          break;
        }
      if (same) return kv.second;
    }
    return "";
  }
};

struct HttpResponse {
  int status = 200;
  std::string contentType = "application/json; charset=utf-8";
  std::string body;

  /// 构造 JSON 响应 (Content-Type 默认 application/json)。
  ///
  /// @param body    JSON 文本
  /// @param status  状态码 (默认 200)
  /// @return        构造好的 HttpResponse
  static HttpResponse json(const std::string& body, int status = 200) {
    HttpResponse r;
    r.status = status;
    r.contentType = "application/json; charset=utf-8";
    r.body = body;
    return r;
  }
  /// 构造纯文本响应 (Content-Type 默认 text/plain)。
  ///
  /// @param body   文本正文
  /// @param status 状态码 (默认 200)
  /// @return       构造好的 HttpResponse
  static HttpResponse text(const std::string& body, int status = 200) {
    HttpResponse r;
    r.status = status;
    r.contentType = "text/plain; charset=utf-8";
    r.body = body;
    return r;
  }
};

/// @brief 极简 HTTP/1.1 服务器: 监听 + 线程池 + 路由表 + 静态文件服务。
///
/// 生命周期: route()/setXXX() 配置 -> listen(port) 建套接字 -> run() 阻塞收连接
/// (每连接投进任务队列, 由 threads_ 个工作线程并发处理) -> stop() 收尾。
/// 只支持 "Connection: close" 短连接, 不做 HTTPS/chunked/上传/WebSocket —— 够用即止。
class HttpServer {
 public:
  /// @brief 路由处理函数类型: 入参是已解析的请求, 返回要发回给客户端的响应。
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  HttpServer() = default;
  /// 析构: 调用 stop() 释放监听套接字与工作线程。
  ~HttpServer();

  /// 注册一条路由; path 中 ":name" 段会被作为路径参数提取。
  ///
  /// @param method  HTTP 方法 (如 "GET"/"POST"/"*")
  /// @param path    路径模板 ("/api/machines/:code")
  /// @param h       处理函数
  void route(const std::string& method, const std::string& path, Handler h);
  /// 设置静态文件根目录 (空表示不提供静态文件)。
  void setStaticDir(const std::string& dir) { staticDir_ = dir; }
  /// 设置目录默认首页文件名 (默认 "index.html")。
  void setIndexFile(const std::string& f) { indexFile_ = f; }
  /// 设置工作线程数 (默认 8, 在 listen 之前设置生效)。
  void setThreads(int n) { threads_ = n; }

  /// 创建并绑定监听套接字, 启动监听; 设置 running_ = true。
  ///
  /// @param port     监听端口
  /// @param bindAddr 绑定地址, 空或 "0.0.0.0" 表示全部网卡
  /// @return         成功 true, 失败 (端口占用/权限不足) false
  bool listen(int port, const std::string& bindAddr = "0.0.0.0");
  /// 主循环: 接受连接并投入任务队列, 由工作线程消费; running_ 为假时退出。
  void run();
  /// 停止服务: 关闭监听、唤醒并 join 所有工作线程、清理 Winsock。
  void stop();

  /// 返回实际监听端口。
  int port() const { return port_; }

 private:
  /// @brief 一条已注册的路由: 方法 + 按 "/" 切分的路径模板 + 处理函数。
  ///
  /// 路径段里形如 ":code" 的段是占位符, 匹配时把实际值塞进 HttpRequest::params。
  struct Route {
    std::string method;
    std::vector<std::string> segments;  // 按 "/" 切分后的路径段
    Handler handler;
  };

  /// 工作线程循环: 阻塞等待任务队列, 取出连接后调用 handleClient。
  void workerLoop();
  /// 处理单个客户端连接: 解析请求 -> dispatch -> 发送响应 -> 关闭。
  void handleClient(uintptr_t sock);
  /// 请求分派: 按方法/路径段匹配路由 (含 :param), 否则尝试静态文件, 再否则 404。
  HttpResponse dispatch(HttpRequest& req);
  /// 尝试从静态目录读取文件并填入 out; 含 ".." 穿越防护与 SPA 兜底。
  bool serveStatic(const std::string& path, HttpResponse& out);

  std::vector<Route> routes_;
  std::string staticDir_;
  std::string indexFile_ = "index.html";

  uintptr_t listenSock_ = ~uintptr_t(0);
  int port_ = 0;
  std::atomic<bool> running_{false};
  int threads_ = 8;

  std::vector<std::thread> workers_;
  std::queue<uintptr_t> tasks_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::atomic<bool> quit_{false};
};

}  // namespace aps

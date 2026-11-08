#include "aps/server/http_server.hpp"

// ---------------------------------------------------------------------------
// http_server.cpp -- HttpServer 实现 + 一组文件/字符串工具 (匿名命名空间)
//
// 内容概要:
//   - 平台抽象: socket_t / closeSock / lastErr (Win/Linux 统一)
//   - 工具: statusText / urlDecode / splitPath / readFile / isFile / mimeOf
//   - 生命周期: listen -> run(线程池) -> workerLoop -> handleClient -> dispatch
//   - 静态文件: serveStatic (含路径穿越防护与 SPA 兜底)
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aps {
namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr uintptr_t kInvalidSock = static_cast<uintptr_t>(~0ull);
inline void closeSock(socket_t s) { closesocket(s); }
inline int lastErr() { return WSAGetLastError(); }
#else
using socket_t = int;
constexpr uintptr_t kInvalidSock = static_cast<uintptr_t>(-1);
inline void closeSock(socket_t s) { ::close(s); }
inline int lastErr() { return errno; }
#endif

// 把 uintptr_t 句柄还原为具体平台 socket 类型。
socket_t toSock(uintptr_t v) { return static_cast<socket_t>(v); }

// HTTP 状态码 -> 原因短语 (未知码回退 OK)。
const char* statusText(int code) {
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default: return "OK";
  }
}

// 对 URL 编码串解码: '+' -> 空格, "%XX" -> 字节 (其余原样)。
std::string urlDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out += ' ';
    } else if (s[i] == '%' && i + 2 < s.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
      } else {
        out += s[i];
      }
    } else {
      out += s[i];
    }
  }
  return out;
}

// 将路径按 '/' 切分为非空段 (忽略连续斜杠与首尾斜杠)。
std::vector<std::string> splitPath(const std::string& path) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : path) {
    if (c == '/') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// 二进制读取整个文件到 out; 失败返回 false。
bool readFile(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// 判断路径是否指向普通文件 (而非目录/不存在), 跨平台使用 stat。
bool isFile(const std::string& p) {
  struct stat st{};
  if (::stat(p.c_str(), &st) != 0) return false;
#ifdef _WIN32
  return (st.st_mode & _S_IFREG) != 0;
#else
  return S_ISREG(st.st_mode);
#endif
}

// 按文件扩展名返回 Content-Type; 未知类型回退 application/octet-stream。
const char* mimeOf(const std::string& path) {
  auto ends = [&](const char* ext) {
    size_t n = std::strlen(ext);
    return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
  };
  if (ends(".html") || ends(".htm")) return "text/html; charset=utf-8";
  if (ends(".js")) return "application/javascript; charset=utf-8";
  if (ends(".css")) return "text/css; charset=utf-8";
  if (ends(".json")) return "application/json; charset=utf-8";
  if (ends(".svg")) return "image/svg+xml";
  if (ends(".png")) return "image/png";
  if (ends(".ico")) return "image/x-icon";
  return "application/octet-stream";
}

}  // namespace

HttpServer::~HttpServer() { stop(); }

// 注册路由: 切分路径模板并保存方法/段/处理函数到 routes_。
void HttpServer::route(const std::string& method, const std::string& path, Handler h) {
  Route r;
  r.method = method;
  r.segments = splitPath(path);
  r.handler = std::move(h);
  routes_.push_back(std::move(r));
}

// 创建监听套接字: 初始化 Winsock(Windows) -> socket -> SO_REUSEADDR ->
// 绑定 bindAddr/port -> listen(128); 成功置 listenSock_/port_/running_。
bool HttpServer::listen(int port, const std::string& bindAddr) {
#ifdef _WIN32
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
  socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s == static_cast<socket_t>(kInvalidSock)) return false;

  int opt = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<unsigned short>(port));
  if (bindAddr.empty() || bindAddr == "0.0.0.0") {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else if (inet_pton(AF_INET, bindAddr.c_str(), &addr.sin_addr) != 1) {
    closeSock(s);
    return false;
  }

  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    closeSock(s);
    return false;
  }
  if (::listen(s, 128) != 0) {
    closeSock(s);
    return false;
  }

  listenSock_ = static_cast<uintptr_t>(s);
  port_ = port;
  running_ = true;
  return true;
}

// 启动线程池并进入主循环: 每接到一个连接就锁入任务队列并唤醒一个工作线程。
void HttpServer::run() {
  if (listenSock_ == kInvalidSock) return;

  workers_.reserve(static_cast<size_t>(threads_));
  for (int i = 0; i < threads_; ++i) workers_.emplace_back([this] { workerLoop(); });

  while (running_) {
    sockaddr_in cli{};
#ifdef _WIN32
    int len = sizeof(cli);
#else
    socklen_t len = sizeof(cli);
#endif
    socket_t c = ::accept(toSock(listenSock_), reinterpret_cast<sockaddr*>(&cli), &len);
    if (c == static_cast<socket_t>(kInvalidSock)) {
      if (!running_) break;
      continue;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      tasks_.push(static_cast<uintptr_t>(c));
    }
    cv_.notify_one();
  }
}

// 停止服务: 关闭监听套接字、置 quit_ 唤醒所有工作线程并 join; 清理 Winsock。
void HttpServer::stop() {
  if (!running_.exchange(false)) {
    // 仍然要唤醒线程
  }
  if (listenSock_ != kInvalidSock) {
    closeSock(toSock(listenSock_));
    listenSock_ = kInvalidSock;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    quit_ = true;
  }
  cv_.notify_all();
  for (auto& t : workers_)
    if (t.joinable()) t.join();
  workers_.clear();
#ifdef _WIN32
  WSACleanup();
#endif
}

// 工作线程: 等待 quit_ 或任务到来, 取一个连接套接字交给 handleClient 处理。
void HttpServer::workerLoop() {
  while (true) {
    uintptr_t task = 0;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return quit_ || !tasks_.empty(); });
      if (quit_ && tasks_.empty()) return;
      task = tasks_.front();
      tasks_.pop();
    }
    handleClient(task);
  }
}

// 处理单个连接: 接收请求头与 body -> 解析为 HttpRequest -> dispatch 得到
// 响应 -> 拼装 HTTP 报文并发送 -> 关闭套接字。解析/调度异常被捕获为 500。
//
// @param sockRaw 来自任务队列的客户端套接字 (uintptr_t 句柄)
void HttpServer::handleClient(uintptr_t sockRaw) {
  socket_t sock = toSock(sockRaw);

  // 读超时, 防止慢连接长期占用工作线程
#ifdef _WIN32
  DWORD tv = 8000;
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
  timeval tv{8, 0};
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

  std::string raw;
  raw.reserve(4096);
  char buf[8192];
  size_t headerEnd = std::string::npos;

  while (raw.size() < 1024 * 1024) {
    int n = ::recv(sock, buf, sizeof(buf), 0);
    if (n <= 0) break;
    raw.append(buf, static_cast<size_t>(n));
    headerEnd = raw.find("\r\n\r\n");
    if (headerEnd != std::string::npos) break;
  }

  if (headerEnd == std::string::npos) {
    closeSock(sock);
    return;
  }

  HttpRequest req;
  {
    std::istringstream hs(raw.substr(0, headerEnd));
    std::string line;
    std::getline(hs, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    {
      std::istringstream ls(line);
      std::string target;
      ls >> req.method >> target;
      size_t q = target.find('?');
      if (q != std::string::npos) {
        req.query = target.substr(q + 1);
        target = target.substr(0, q);
      }
      req.path = urlDecode(target);
    }
    while (std::getline(hs, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      size_t c = line.find(':');
      if (c == std::string::npos) continue;
      std::string k = line.substr(0, c);
      std::string v = line.substr(c + 1);
      while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
      req.headers[k] = v;
    }
  }

  // body
  size_t contentLen = 0;
  {
    std::string cl = req.header("Content-Length");
    if (!cl.empty()) contentLen = static_cast<size_t>(std::strtoull(cl.c_str(), nullptr, 10));
  }
  if (contentLen > 8 * 1024 * 1024) {
    std::string resp = "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    ::send(sock, resp.data(), static_cast<int>(resp.size()), 0);
    closeSock(sock);
    return;
  }
  std::string body = raw.substr(headerEnd + 4);
  while (body.size() < contentLen) {
    int n = ::recv(sock, buf, sizeof(buf), 0);
    if (n <= 0) break;
    body.append(buf, static_cast<size_t>(n));
  }
  req.body = body.substr(0, contentLen);

  // query 参数
  if (!req.query.empty()) {
    std::istringstream qs(req.query);
    std::string kv;
    while (std::getline(qs, kv, '&')) {
      size_t e = kv.find('=');
      if (e == std::string::npos) {
        req.queryParams[urlDecode(kv)] = "";
      } else {
        req.queryParams[urlDecode(kv.substr(0, e))] = urlDecode(kv.substr(e + 1));
      }
    }
  }

  HttpResponse resp;
  try {
    resp = dispatch(req);
  } catch (const std::exception& ex) {
    resp = HttpResponse::json(std::string("{\"error\":\"") + ex.what() + "\"}", 500);
  } catch (...) {
    resp = HttpResponse::json("{\"error\":\"unknown server error\"}", 500);
  }

  std::string out;
  out.reserve(resp.body.size() + 256);
  out += "HTTP/1.1 " + std::to_string(resp.status) + " " + statusText(resp.status) + "\r\n";
  out += "Content-Type: " + resp.contentType + "\r\n";
  out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
  out += "Access-Control-Allow-Origin: *\r\n";
  out += "Access-Control-Allow-Headers: Content-Type\r\n";
  out += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
  out += "Connection: close\r\n\r\n";
  out += resp.body;

  size_t sent = 0;
  while (sent < out.size()) {
    int n = ::send(sock, out.data() + sent, static_cast<int>(out.size() - sent), 0);
    if (n <= 0) break;
    sent += static_cast<size_t>(n);
  }
  closeSock(sock);
}

// 请求分派: 先处理 OPTIONS 预检返回 204; 再逐条路由匹配 (方法 + 段数 +
// :param 提取); 不匹配则交 serveStatic; 仍失败返回 404。
//
// @param req  已解析的请求 (可能写入 params 路径参数)
// @return     对应的 HttpResponse
HttpResponse HttpServer::dispatch(HttpRequest& req) {
  if (req.method == "OPTIONS") {
    HttpResponse r;
    r.status = 204;
    r.body.clear();
    return r;
  }

  auto reqSegs = splitPath(req.path);
  for (const auto& rt : routes_) {
    if (rt.method != req.method && rt.method != "*") continue;
    if (rt.segments.size() != reqSegs.size()) continue;
    std::map<std::string, std::string> params;
    bool ok = true;
    for (size_t i = 0; i < rt.segments.size(); ++i) {
      const std::string& pat = rt.segments[i];
      if (!pat.empty() && pat[0] == ':') {
        params[pat.substr(1)] = reqSegs[i];
      } else if (pat != reqSegs[i]) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;
    req.params = params;
    return rt.handler(req);
  }

  HttpResponse st;
  if (serveStatic(req.path, st)) return st;
  return HttpResponse::json("{\"error\":\"not found\"}", 404);
}

// 静态文件服务: 拒绝含 ".." 的路径穿越; 根路径回 index; 文件不存在且路径
// 无扩展名时按 SPA 回退到 index; 命中则读取内容并填 Content-Type。
//
// @param path  请求路径
// @param out   命中时写入的响应
// @return      是否成功命中并填充
bool HttpServer::serveStatic(const std::string& path, HttpResponse& out) {
  if (staticDir_.empty()) return false;
  if (path.find("..") != std::string::npos) return false;

  std::string rel = path == "/" ? ("/" + indexFile_) : path;
  std::string full = staticDir_ + rel;

  if (!isFile(full)) {
    // SPA 兜底: 未知路径回 index
    if (path.find('.') == std::string::npos) full = staticDir_ + "/" + indexFile_;
    if (!isFile(full)) return false;
  }

  std::string data;
  if (!readFile(full, data)) return false;
  out.status = 200;
  out.contentType = mimeOf(full);
  out.body = std::move(data);
  return true;
}

}  // namespace aps

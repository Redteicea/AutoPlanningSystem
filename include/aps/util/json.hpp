// ---------------------------------------------------------------------------
// util/json.hpp -- 极简 JSON 库 (解析 + 序列化)
//
// 为什么不用第三方库:
//   本项目定位是"可编译即跑"的作品集工程, 不希望使用者为了跑起来先装 vcpkg。
//   需求单一 (REST 的请求/响应), 一个 300 行的实现比 1 个外部依赖更可控。
//
// 支持: object / array / string / number / bool / null, \uXXXX 转 UTF-8。
// 不支持: 注释、NaN、超长数字精度(统一用 double, 整数出口走 int64 转换)。
// ---------------------------------------------------------------------------
//
// 内容概要:
//   - Value: 通用 JSON 值类型, 支持 object/array/string/number/bool/null 六型
//   - 读取: as* 系列带默认值, 便于服务层直接取值而无需 try/catch
//   - 路径: path("a.b[0].c") 链式定位, 不存在时返回静态 null 值
//   - 序列化: dump(indent) 输出文本; 解析: parse() 走递归下降 Parser
#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace aps {
namespace json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  /// 默认构造为 null 值。
  Value() : type_(Type::Null) {}
  /// 由 nullptr 构造为 null 值 (便于 Value v = nullptr 写法)。
  Value(std::nullptr_t) : type_(Type::Null) {}
  /// 由布尔值构造为 Bool 类型。
  Value(bool v) : type_(Type::Bool), bool_(v) {}

  // 整型统一走模板, 避免 int / long / long long / int64_t 之间重载歧义
  /// 由任意整数类型构造为 Number 类型 (内部统一存为 double)。
  template <typename T,
            typename std::enable_if<std::is_integral<T>::value &&
                                        !std::is_same<T, bool>::value,
                                    int>::type = 0>
  Value(T v) : type_(Type::Number), num_(static_cast<double>(v)) {}

  /// 由双精度浮点构造为 Number 类型。
  Value(double v) : type_(Type::Number), num_(v) {}
  /// 由单精度浮点构造为 Number 类型 (升格为 double 存储)。
  Value(float v) : type_(Type::Number), num_(static_cast<double>(v)) {}
  /// 由 C 字符串构造为 String 类型。
  Value(const char* s) : type_(Type::String), str_(s) {}
  /// 由 std::string 构造为 String 类型 (移动语义, 不拷贝)。
  Value(std::string s) : type_(Type::String), str_(std::move(s)) {}

  /// 创建空数组值 (type 置为 Array, 容器为空)。
  static Value array() { Value v; v.type_ = Type::Array; return v; }
  /// 创建空对象值 (type 置为 Object, 容器为空)。
  static Value object() { Value v; v.type_ = Type::Object; return v; }

  /// 返回当前值的类型枚举。
  Type type() const { return type_; }
  /// 是否为 null。
  bool isNull() const { return type_ == Type::Null; }
  /// 是否为数组。
  bool isArray() const { return type_ == Type::Array; }
  /// 是否为对象。
  bool isObject() const { return type_ == Type::Object; }
  /// 是否为数字。
  bool isNumber() const { return type_ == Type::Number; }
  /// 是否为字符串。
  bool isString() const { return type_ == Type::String; }
  /// 是否为布尔。
  bool isBool() const { return type_ == Type::Bool; }

  // ---- 读取 (带默认值, 服务层不做 try/catch 地狱) ----
  /// 读布尔; 类型不符时返回 def (默认 false)。
  bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
  /// 读双精度; 类型不符时返回 def (默认 0.0)。
  double asDouble(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
  /// 读整数 (从 double 截断转换); 类型不符返回 def (默认 0)。
  long long asInt(long long def = 0) const {
    return type_ == Type::Number ? static_cast<long long>(num_) : def;
  }
  /// 读字符串; 类型不符时返回 def (默认空串)。
  std::string asString(const std::string& def = "") const {
    return type_ == Type::String ? str_ : def;
  }

  /// 只读访问数组元素容器。
  const std::vector<Value>& items() const { return arr_; }
  /// 读写访问数组元素容器 (用于批量填充)。
  std::vector<Value>& items() { return arr_; }
  /// 只读访问对象字段表。
  const std::map<std::string, Value>& fields() const { return obj_; }

  /// 返回数组或对象中的元素/字段个数; 其它类型返回 0。
  size_t size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
  }

  /// 对象是否含有给定 key (非对象一律返回 false)。
  bool has(const std::string& k) const { return type_ == Type::Object && obj_.count(k) > 0; }

  // obj[key] -- 不存在时返回静态 null, 便于链式读取
  /// 按 key 读取对象字段; 非对象或缺失时返回静态 null (可继续链式访问)。
  const Value& operator[](const std::string& k) const {
    static const Value kNull;
    if (type_ != Type::Object) return kNull;
    auto it = obj_.find(k);
    return it == obj_.end() ? kNull : it->second;
  }
  /// 按下标读取数组元素; 越界或非数组时返回静态 null。
  const Value& operator[](size_t i) const {
    static const Value kNull;
    if (type_ != Type::Array || i >= arr_.size()) return kNull;
    return arr_[i];
  }
  /// 按 key 写入/新建对象字段 (自动将类型置为 Object)。
  Value& operator[](const std::string& k) {
    type_ = Type::Object;
    return obj_[k];
  }

  // path: "a.b.c" 或 "a.b[0].c"
  /// 按点号/下标路径链式定位子值, 例如 "a.b[0].c"; 任一段缺失返回静态 null。
  const Value& path(const std::string& p) const;

  // ---- 写入 ----
  /// 向数组追加一个元素 (非数组会自动转为数组并清空原有内容)。
  void push(Value v) {
    if (type_ != Type::Array) { type_ = Type::Array; arr_.clear(); }
    arr_.push_back(std::move(v));
  }
  /// 写入对象字段 key (自动将类型置为 Object; 同类字段直接覆盖)。
  void set(const std::string& k, Value v) {
    type_ = Type::Object;
    obj_[k] = std::move(v);
  }

  // ---- 序列化 ----
  /// 将本值序列化为 JSON 文本; indent<0 时为紧凑格式, 否则按层级缩进。
  std::string dump(int indent = -1) const;

  // ---- 解析 ----
  /// 解析整段 JSON 文本; 失败抛出 std::runtime_error (含出错位置)。
  static Value parse(const std::string& text);

 private:
  void dumpTo(std::string& out, int indent, int depth) const;

  Type type_;
  bool bool_ = false;
  double num_ = 0.0;
  std::string str_;
  std::vector<Value> arr_;
  std::map<std::string, Value> obj_;
};

// ---------------------------------------------------------------------------
// 解析实现 (header-only, 内部类)
// ---------------------------------------------------------------------------
namespace detail {

// 将 Unicode 码点 cp 按 UTF-8 变长编码写入 out (覆盖 1~4 字节)。
inline void appendUtf8(std::string& out, unsigned cp) {
  if (cp <= 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp <= 0x7FF) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

class Parser {
 public:
  /// 构造解析器, 持有待解析文本 s 的引用 (调用方需保证其生命周期)。
  explicit Parser(const std::string& s) : s_(s) {}

  /// 解析入口: 跳过前导空白 -> 解析一个值 -> 跳过尾部空白后返回。
  Value run() {
    skipWs();
    Value v = parseValue();
    skipWs();
    return v;
  }

 private:
  /// 跳过空白字符 (空格/制表/换行/回车)。
  void skipWs() {
    while (i_ < s_.size()) {
      char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
      else break;
    }
  }

  /// 抛出带当前位置的解析错误 (永不返回)。
  [[noreturn]] void fail(const std::string& why) {
    throw std::runtime_error("JSON parse error at " + std::to_string(i_) + ": " + why);
  }

  /// 查看但不消费当前字符; 已到末尾则报错。
  char peek() {
    if (i_ >= s_.size()) fail("unexpected end");
    return s_[i_];
  }

  /// 消费一个期望的字符 c, 不符则报错。
  void expect(char c) {
    if (i_ >= s_.size() || s_[i_] != c) fail(std::string("expected '") + c + "'");
    ++i_;
  }

  /// 根据首字符分派到对象/数组/字符串/字面量/数字的解析。
  Value parseValue() {
    skipWs();
    char c = peek();
    switch (c) {
      case '{': return parseObject();
      case '[': return parseArray();
      case '"': return Value(parseString());
      case 't': return parseLiteral("true", Value(true));
      case 'f': return parseLiteral("false", Value(false));
      case 'n': return parseLiteral("null", Value());
      default: return parseNumber();
    }
  }

  /// 匹配固定字面量 lit (如 true/false/null), 命中后返回对应值 v, 否则报错。
  Value parseLiteral(const char* lit, Value v) {
    size_t n = std::string(lit).size();
    if (s_.compare(i_, n, lit) != 0) fail("bad literal");
    i_ += n;
    return v;
  }

  /// 解析对象 "{ ... }": 依次解析 "key": value, 支持空对象。
  Value parseObject() {
    expect('{');
    Value obj = Value::object();
    skipWs();
    if (peek() == '}') { ++i_; return obj; }
    while (true) {
      skipWs();
      std::string key = parseString();
      skipWs();
      expect(':');
      obj.set(key, parseValue());
      skipWs();
      char c = peek();
      if (c == ',') { ++i_; continue; }
      if (c == '}') { ++i_; break; }
      fail("expected ',' or '}'");
    }
    return obj;
  }

  /// 解析数组 "[ ... ]": 依次解析元素, 支持空数组。
  Value parseArray() {
    expect('[');
    Value arr = Value::array();
    skipWs();
    if (peek() == ']') { ++i_; return arr; }
    while (true) {
      arr.push(parseValue());
      skipWs();
      char c = peek();
      if (c == ',') { ++i_; continue; }
      if (c == ']') { ++i_; break; }
      fail("expected ',' or ']'");
    }
    return arr;
  }

  /// 解析带引号的字符串, 处理转义 (\\ \" \/ \b \f \n \r \t \uXXXX, 含代理对)。
  std::string parseString() {
    expect('"');
    std::string out;
    while (true) {
      if (i_ >= s_.size()) fail("unterminated string");
      char c = s_[i_++];
      if (c == '"') break;
      if (c != '\\') { out += c; continue; }
      if (i_ >= s_.size()) fail("bad escape");
      char e = s_[i_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (i_ + 4 > s_.size()) fail("bad \\u");
          unsigned cp = static_cast<unsigned>(std::stoul(s_.substr(i_, 4), nullptr, 16));
          i_ += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() && s_[i_] == '\\' &&
              s_[i_ + 1] == 'u') {
            unsigned lo = static_cast<unsigned>(std::stoul(s_.substr(i_ + 2, 4), nullptr, 16));
            i_ += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          appendUtf8(out, cp);
          break;
        }
        default: fail("bad escape char");
      }
    }
    return out;
  }

  /// 解析数字 (整数/小数/指数/正负号), 以 std::stod 转 double; 空则报错。
  Value parseNumber() {
    size_t start = i_;
    if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
    while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
                              s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+'))
      ++i_;
    if (start == i_) fail("expected number");
    return Value(std::stod(s_.substr(start, i_ - start)));
  }

  const std::string& s_;
  size_t i_ = 0;
};

}  // namespace detail

// 按 "a.b.c" 或 "a.b[0].c" 路径逐步下钻; 任一段缺失/越界返回静态 null。
inline const Value& Value::path(const std::string& p) const {
  static const Value kNull;
  const Value* cur = this;
  size_t i = 0;
  while (i < p.size()) {
    if (p[i] == '.') { ++i; continue; }
    if (p[i] == '[') {
      size_t close = p.find(']', i);
      if (close == std::string::npos) return kNull;
      int idx = std::stoi(p.substr(i + 1, close - i - 1));
      cur = &(*cur)[static_cast<size_t>(idx)];
      i = close + 1;
      continue;
    }
    size_t end = p.find_first_of(".[", i);
    if (end == std::string::npos) end = p.size();
    cur = &(*cur)[p.substr(i, end - i)];
    i = end;
  }
  return *cur;
}

// 序列化入口: 预分配缓冲后调用 dumpTo 递归输出。
inline std::string Value::dump(int indent) const {
  std::string out;
  out.reserve(256);
  dumpTo(out, indent, 0);
  return out;
}

// 递归序列化: 按类型输出, indent>=0 时在每个层级插入换行与缩进空格。
inline void Value::dumpTo(std::string& out, int indent, int depth) const {
  auto newline = [&](int d) {
    if (indent < 0) return;
    out += '\n';
    out.append(static_cast<size_t>(indent * d), ' ');
  };
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: {
      char buf[64];
      double v = num_;
      if (std::abs(v) < 9.0e15 && v == std::floor(v)) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
      } else {
        std::snprintf(buf, sizeof(buf), "%.10g", v);
      }
      out += buf;
      break;
    }
    case Type::String: {
      out += '"';
      for (char c : str_) {
        switch (c) {
          case '"': out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\n': out += "\\n"; break;
          case '\r': out += "\\r"; break;
          case '\t': out += "\\t"; break;
          default:
            if (static_cast<unsigned char>(c) < 0x20) {
              char buf[8];
              std::snprintf(buf, sizeof(buf), "\\u%04x", c);
              out += buf;
            } else {
              out += c;
            }
        }
      }
      out += '"';
      break;
    }
    case Type::Array: {
      if (arr_.empty()) { out += "[]"; break; }
      out += '[';
      for (size_t i = 0; i < arr_.size(); ++i) {
        if (i) out += ',';
        newline(depth + 1);
        arr_[i].dumpTo(out, indent, depth + 1);
      }
      newline(depth);
      out += ']';
      break;
    }
    case Type::Object: {
      if (obj_.empty()) { out += "{}"; break; }
      out += '{';
      bool first = true;
      for (const auto& kv : obj_) {
        if (!first) out += ',';
        first = false;
        newline(depth + 1);
        out += '"';
        out += kv.first;
        out += "\":";
        if (indent >= 0) out += ' ';
        kv.second.dumpTo(out, indent, depth + 1);
      }
      newline(depth);
      out += '}';
      break;
    }
  }
}

// 解析入口: 构造临时 Parser 并 run() 得到根值。
inline Value Value::parse(const std::string& text) { return detail::Parser(text).run(); }

// 便捷入口: json::parse(text) 等价于 Value::parse(text)。
inline Value parse(const std::string& text) { return Value::parse(text); }

}  // namespace json
}  // namespace aps

// ---------------------------------------------------------------------------
// data/app_config.cpp -- 通用配置的 JSON 转换与文件读写实现
//
//   - defaultAppConfig: 内置默认值 (与旧版硬编码行为一致, 保证向后兼容)
//   - appConfigToJson/FromJson: AppConfig <-> JSON; FromJson 是"部分更新"语义,
//     缺字段沿用现值, 方便 POST /api/config 只传要改的部分
//   - load/saveAppConfig: 文本文件读写; load 时文件不存在以 err=="missing" 区分
//   - 校验规则: workdays 非空且 0..6; shifts 非空且 start 0..23、end 1..24;
//     orders/days/timeLimitMs 越界一律报错 (宁可直接失败, 不静默截断)
// ---------------------------------------------------------------------------

#include "aps/data/app_config.hpp"

#include <algorithm>
#include <fstream>
#include <set>

namespace aps {

/// @brief 内置默认配置 (升级/首次启动时的打底值, 与旧版硬编码行为保持一致)。
///
/// 日历: 周一至周五上班, 8-12 + 13-17 两班; 算例: 80 单 / 14 天窗口 / 固定种子;
/// 求解: GA + 2000ms。
///
/// @return 填好默认值的配置对象
AppConfig defaultAppConfig() {
  AppConfig c;
  c.calendar.workdays = {1, 2, 3, 4, 5};
  c.calendar.shifts = {{8, 12}, {13, 17}};
  c.orderCount = 80;
  c.horizonDays = 14;
  c.seed = 20261105;
  c.algo = "ga";
  c.timeLimitMs = 2000;
  return c;
}

/// @brief 把厂级日历序列化成 JSON 对象 (供 appConfigToJson 复用)。
///
/// 输出固定三个键: workdays (整数数组) / shifts (start,end 对象数组) /
/// downtime (start,end Unix 秒对象数组)。即使 downtime 为空也会输出空数组,
/// 保证落盘的模板结构完整、用户照着改不会缺字段。
///
/// @param cal 待序列化的日历 (工作日 / 班次 / 停机窗口)
/// @return     JSON 对象 {"workdays":[...],"shifts":[...],"downtime":[...]}
static json::Value calendarToJson(const CalendarSpec& cal) {
  json::Value c = json::Value::object();
  json::Value wd = json::Value::array();
  for (int d : cal.workdays) wd.push(d);
  c.set("workdays", wd);

  json::Value sh = json::Value::array();
  for (const auto& s : cal.shifts) {
    json::Value o = json::Value::object();
    o.set("start", s.startHour);
    o.set("end", s.endHour);
    sh.push(o);
  }
  c.set("shifts", sh);

  json::Value dt = json::Value::array();
  for (const auto& w : cal.downtime) {
    json::Value o = json::Value::object();
    o.set("start", static_cast<double>(w.first));
    o.set("end", static_cast<double>(w.second));
    dt.push(o);
  }
  c.set("downtime", dt);
  return c;
}

/// @brief 从 JSON 解析日历, 支持字段级部分更新 (只覆盖 JSON 里出现的键)。
///
/// 校验口径 (任一不满足都直接失败, 不做静默截断):
///   - workdays: 数组、元素 0..6、去重排序后不能为空;
///   - shifts:   数组、start 0..23、end 1..24、不能为空; end<=start 表示跨夜班;
///   - downtime: 数组、start < end (Unix 秒)。
/// 三个键都缺失时视为"不改日历", 直接返回 true。
///
/// @param v   日历 JSON 对象; 不是对象时三个键都取不到, 效果等同空操作
/// @param cal 输入输出参数: 解析成功的字段写入这里, 未出现的字段保持原值
/// @param err 失败时写入具体原因 (含字段名与合法范围), 供 /api/config 直接回给前端
/// @return    true=全部合法; false=有字段非法 (此时 cal 可能已被部分修改, 调用方应丢弃)
static bool calendarFromJson(const json::Value& v, CalendarSpec& cal, std::string& err) {
  const json::Value& wd = v["workdays"];
  if (wd.isArray()) {
    std::vector<int> days;
    for (const auto& x : wd.items()) {
      if (!x.isNumber()) { err = "calendar.workdays 含非数字元素"; return false; }
      int d = static_cast<int>(x.asInt());
      if (d < 0 || d > 6) { err = "calendar.workdays 取值须在 0..6 (0=周日)"; return false; }
      days.push_back(d);
    }
    if (days.empty()) { err = "calendar.workdays 不能为空"; return false; }
    std::set<int> uniq(days.begin(), days.end());  // 去重 + 排序, 输出顺序稳定
    cal.workdays.assign(uniq.begin(), uniq.end());
  }

  const json::Value& sh = v["shifts"];
  if (sh.isArray()) {
    std::vector<Shift> shifts;
    for (const auto& x : sh.items()) {
      int st = static_cast<int>(x["start"].asInt(-1));
      int en = static_cast<int>(x["end"].asInt(-1));
      if (st < 0 || st > 23 || en < 1 || en > 24) {
        err = "calendar.shifts 班次非法: start 须 0..23, end 须 1..24 (end<=start 表跨夜班)";
        return false;
      }
      shifts.push_back(Shift{st, en});
    }
    if (shifts.empty()) { err = "calendar.shifts 不能为空 (整厂无班次等于停产)"; return false; }
    cal.shifts = shifts;
  }

  const json::Value& dt = v["downtime"];
  if (dt.isArray()) {
    std::vector<std::pair<Seconds, Seconds>> win;
    for (const auto& x : dt.items()) {
      long long a = x["start"].asInt(-1);
      long long b = x["end"].asInt(-1);
      if (a < 0 || b <= a) { err = "calendar.downtime 窗口非法 (须 start < end)"; return false; }
      win.emplace_back(static_cast<Seconds>(a), static_cast<Seconds>(b));
    }
    cal.downtime = win;
  }
  return true;
}

/// @brief 把完整配置序列化成三块结构: calendar / demo / solve。
///
/// 这是 GET /api/config 的返回体, 也是落盘文件的唯一来源 —— 两处输出永远一致。
///
/// @param cfg 待序列化的完整配置
/// @return    JSON 对象 {"calendar":{...},"demo":{...},"solve":{...}}
json::Value appConfigToJson(const AppConfig& cfg) {
  json::Value v = json::Value::object();
  v.set("calendar", calendarToJson(cfg.calendar));

  json::Value demo = json::Value::object();
  demo.set("orders", cfg.orderCount);
  demo.set("days", cfg.horizonDays);
  demo.set("seed", cfg.seed);
  v.set("demo", demo);

  json::Value solve = json::Value::object();
  solve.set("algo", cfg.algo);
  solve.set("timeLimitMs", cfg.timeLimitMs);
  v.set("solve", solve);
  return v;
}

/// @brief 从 JSON 解析完整配置, "部分更新"语义 —— 缺失的键沿用 out 的现值。
///
/// 先在副本上改, 全部校验通过才提交到 out; 任何一项非法都返回 false 且 out 保持原样,
/// 所以 POST /api/config 传了坏值不会把服务里的现配置污染掉。
///
/// 校验口径:
///   - demo.orders     1..100000
///   - demo.days       1..366
///   - demo.seed       >= 0
///   - solve.algo      只允许 rule | ga | auto
///   - solve.timeLimitMs 50..600000
///
/// @param v   JSON 对象 (一般来自请求体或配置文件)
/// @param out 输入输出参数: 作为默认值打底, 解析成功后写入最终结果; 失败时不变
/// @param err 失败时写入具体原因 (含字段名与合法范围)
/// @return    true=全部合法并已写入 out; false=校验失败 (out 未被修改)
bool appConfigFromJson(const json::Value& v, AppConfig& out, std::string& err) {
  AppConfig c = out;  // 工作副本: 校验全部通过才提交, 失败不动 out
  if (!calendarFromJson(v["calendar"], c.calendar, err)) return false;

  const json::Value& demo = v["demo"];
  if (demo.isObject()) {
    long long orders = demo["orders"].asInt(c.orderCount);
    long long days = demo["days"].asInt(c.horizonDays);
    long long seed = demo["seed"].asInt(static_cast<long long>(c.seed));
    if (orders < 1 || orders > 100000) { err = "demo.orders 须在 1..100000"; return false; }
    if (days < 1 || days > 366) { err = "demo.days 须在 1..366"; return false; }
    if (seed < 0) { err = "demo.seed 不能为负"; return false; }
    c.orderCount = static_cast<int>(orders);
    c.horizonDays = static_cast<int>(days);
    c.seed = static_cast<std::uint64_t>(seed);
  }

  const json::Value& solve = v["solve"];
  if (solve.isObject()) {
    std::string algo = solve["algo"].asString(c.algo);
    if (algo != "rule" && algo != "ga" && algo != "auto") {
      err = "solve.algo 只支持 rule | ga | auto";
      return false;
    }
    double t = solve["timeLimitMs"].asDouble(c.timeLimitMs);
    if (t < 50 || t > 600000) { err = "solve.timeLimitMs 须在 50..600000"; return false; }
    c.algo = algo;
    c.timeLimitMs = t;
  }

  out = c;
  return true;
}

/// @brief 从本地 JSON 文件读配置。
///
/// 先以 defaultAppConfig() 打底再按文件内容覆盖, 所以文件里只写想改的字段也能工作。
/// 文件不存在时 err 被设为字符串 "missing" —— 调用方靠这个值区分"没有文件"和
/// "文件坏了", 前者会顺手生成一份模板。
///
/// @param path 配置文件路径 (相对路径按进程当前工作目录解析)
/// @param out  输出参数: 成功时是文件内容, 失败时是内置默认值 (调用方可直接用)
/// @param err  失败原因; 文件不存在时固定为 "missing", 解析/校验失败时是具体描述
/// @return     true=读取并校验成功; false=不存在或解析失败 (out 已是默认值)
bool loadAppConfig(const std::string& path, AppConfig& out, std::string& err) {
  out = defaultAppConfig();
  std::ifstream f(path, std::ios::binary);
  if (!f) { err = "missing"; return false; }  // 文件不存在: 用默认值, 让调用方生成模板

  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  try {
    json::Value v = json::parse(text);
    if (!v.isObject()) { err = "配置根节点必须是对象"; return false; }
    return appConfigFromJson(v, out, err);
  } catch (const std::exception& e) {
    err = e.what();
    return false;
  }
}

/// @brief 将 AppConfig 以 2 空格缩进的 JSON 写入文本文件（覆盖式）。
///
/// 以二进制+截断方式打开；写入失败或落盘检查未通过时 err 含路径信息。
///
/// @param path  写入目标路径（已存在则覆盖）
/// @param cfg   要保存的配置
/// @param err   失败原因（输出），如无法打开或写入失败
/// @return     true=写入成功；false=打开或写入失败（err 含原因）
bool saveAppConfig(const std::string& path, const AppConfig& cfg, std::string& err) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) { err = "无法打开文件写入: " + path; return false; }
  f << appConfigToJson(cfg).dump(2) << "\n";
  if (!f.good()) { err = "写入失败: " + path; return false; }
  return true;
}

/// @brief 把通用配置转成演示算例的生成参数 (makeDemoFactory 的入参)。
///
/// 关键点: hasCalendar 必须置 true 并把 cfg.calendar 整体带过去, 否则配置里的
/// 班次/工作日根本到不了算例 —— 页面显示"保存成功"但排产仍用老日历, 属于最难查的
/// 静默失效 (曾被误注释掉一次, 靠单测 T8 抓回来)。
///
/// @param cfg 通用配置
/// @return    演示算例参数; twoShifts 恒为 false (夜班由 calendar.shifts 直接表达)
DemoConfig toDemoConfig(const AppConfig& cfg) {
  DemoConfig d;
  d.orderCount = cfg.orderCount;
  d.horizonDays = cfg.horizonDays;
  d.seed = cfg.seed;
  d.twoShifts = false;       // 日历完全由配置决定, 不再用"加夜班"开关
  d.hasCalendar = true;      // 必须为 true: 否则配置里的班次/工作日到不了算例 (T8 会失败)
  d.calendar = cfg.calendar;
  return d;
}

}  // namespace aps

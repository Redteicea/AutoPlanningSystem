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

// CalendarSpec -> JSON (供 appConfigToJson 复用)。
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

// JSON -> CalendarSpec; exist 控制字段级部分更新 (只覆盖 JSON 里出现的字段)。
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

bool saveAppConfig(const std::string& path, const AppConfig& cfg, std::string& err) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) { err = "无法打开文件写入: " + path; return false; }
  f << appConfigToJson(cfg).dump(2) << "\n";
  if (!f.good()) { err = "写入失败: " + path; return false; }
  return true;
}

DemoConfig toDemoConfig(const AppConfig& cfg) {
  DemoConfig d;
  d.orderCount = cfg.orderCount;
  d.horizonDays = cfg.horizonDays;
  d.seed = cfg.seed;
  d.twoShifts = false;       // 日历完全由配置决定, 不再用"加夜班"开关
  d.hasCalendar = true;
  d.calendar = cfg.calendar;
  return d;
}

}  // namespace aps

// ===========================================================================
// util/time_util.cpp -- 时间/日历工具实现
//
// 内容概要:
//   - 提供 Unix 秒与本地日历 (tm) 的互转、格式化、解析与可读性转换
//   - 跨平台兼容 localtime_s / localtime_r, 时区相关计算按本地时区
// ===========================================================================
#include "aps/util/time_util.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>

namespace aps {
namespace timeutil {

namespace {
  
/// @brief 将 Unix 秒转本地日历分解 (tm); 跨平台兼容 localtime_s / localtime_r。
///
/// 所有日历相关的计算 (取零点、算星期、格式化) 都从这里出发, 统一按本机时区。
///
/// @param t Unix 时间戳 (秒)
/// @return  分解到年月日时分秒的 tm 结构; 失败时返回全零结构 (不抛异常)
std::tm localTm(Seconds t) {
  std::time_t tt = static_cast<std::time_t>(t);
  std::tm out{};
#ifdef _WIN32
  localtime_s(&out, &tt);
#else
  localtime_r(&tt, &out);
#endif
  return out;
}

/// @brief 将本地日历分解反算为 Unix 秒 (mktime 按本地时区, 会自动归一化溢出字段)。
///
/// 因此可以传 "1 月 32 日" 这种越界值, mktime 会顺延成 2 月 1 日。
///
/// @param tmv 本地日历分解 (tm_year 从 1900 起, tm_mon 从 0 起)
/// @return    对应的 Unix 时间戳 (秒)
Seconds fromTm(const std::tm& tmv) {
  std::tm t = tmv;
  return static_cast<Seconds>(std::mktime(&t));
}
}  // namespace

/// @brief 取当前 Unix 时间戳 (秒)。
///
/// @return 服务端当前时刻 (Unix 秒, 按机器本地时区解释)
Seconds now() { return static_cast<Seconds>(std::time(nullptr)); }

/// @brief 取某个时刻所在自然日的 00:00:00 (按本地时区)。
///
/// @param t 任意 Unix 时间戳 (秒)
/// @return  同日零点的时间戳; 夏令时切换日由 mktime 归正
Seconds dayStart(Seconds t) {
  std::tm tmv = localTm(t);
  tmv.tm_hour = 0;
  tmv.tm_min = 0;
  tmv.tm_sec = 0;
  return fromTm(tmv);
}

/// @brief 在时间戳上加减天数 (走本地日历, 不是简单加 86400*days)。
///
/// 先按固定秒数平移, 再让 mktime 处理跨月/月末/夏令时, 所以加 1 天得到的
/// 通常是次日同一时刻。
///
/// @param t    基准 Unix 时间戳 (秒)
/// @param days 天数偏移, 可为负 (负数表示往前推)
/// @return     偏移后的时间戳 (秒)
Seconds addDays(Seconds t, int days) {
  // 先按固定秒数平移再走本地日历, 让 mktime 处理跨月/夏令时归正。
  std::tm tmv = localTm(t + static_cast<Seconds>(days) * 86400);
  return fromTm(tmv);
}

/// @brief 取星期几 (0=周日, 1=周一 … 6=周六), 与 CalendarSpec::workdays 口径一致。
///
/// @param t Unix 时间戳 (秒)
/// @return  0..6, 0 表示周日
int weekday(Seconds t) { return localTm(t).tm_wday; }

/// @brief 取一天中的小时数 (0..23), 用于判断时刻落在哪个班次内。
///
/// @param t Unix 时间戳 (秒)
/// @return  0..23 的整点小时
int hourOfDay(Seconds t) { return localTm(t).tm_hour; }

/// @brief 将时间戳格式化为可读字符串; 兼容带/不带时分两种形式。
///
/// @param t        Unix 时间戳 (秒)
/// @param withTime true=输出 "YYYY-MM-DD HH:MM", false=仅 "YYYY-MM-DD"
/// @return         格式化后的字符串
std::string format(Seconds t, bool withTime) {
  std::tm tmv = localTm(t);
  char buf[32];
  if (withTime) {
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
  } else {
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                  tmv.tm_mday);
  }
  return buf;
}

/// @brief 解析时间字符串: 兼容纯数字时间戳与 "YYYY-MM-DD[ HH:MM]" 两种格式。
///
/// @param s 输入串; 纯数字按 Unix 时间戳, 否则按 "YYYY-MM-DD[ HH:MM]"; 空串失败
/// @return  解析出的 Unix 时间戳 (秒); 解析失败返回 -1
Seconds parse(const std::string& s) {
  if (s.empty()) return -1;
  // 兼容纯数字时间戳
  if (s.find('-') == std::string::npos && s.find(':') == std::string::npos) {
    try {
      return static_cast<Seconds>(std::stoll(s));
    } catch (...) {
      return -1;
    }
  }
  std::tm tmv{};
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  int n = std::sscanf(s.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi);
  if (n < 3) return -1;
  tmv.tm_year = y - 1900;
  tmv.tm_mon = mo - 1;
  tmv.tm_mday = d;
  tmv.tm_hour = (n >= 4) ? h : 0;
  tmv.tm_min = (n >= 5) ? mi : 0;
  tmv.tm_isdst = -1;
  Seconds r = fromTm(tmv);
  return r < 0 ? -1 : r;
}

/// @brief 将时长 (秒) 转换为 "h+分钟"/"分钟"/"秒" 的可读串, 负数递归加前缀。
///
/// 分级规则: >= 1 小时输出 "3h05m"; >= 1 分钟输出 "45m"; 不足 1 分钟输出 "30s"。
///
/// @param dur 时长 (秒), 可为负 —— 负数时先递归取正再补 "-" 前缀
/// @return     可读时长串, 如 "12h30m" / "45m" / "30s" / "-5m"
std::string humanDuration(Seconds dur) {
  if (dur < 0) return "-" + humanDuration(-dur);
  long long h = dur / 3600;
  long long m = (dur % 3600) / 60;
  char buf[64];
  if (h > 0) {
    std::snprintf(buf, sizeof(buf), "%lldh%02lldm", h, m);
  } else if (m > 0) {
    std::snprintf(buf, sizeof(buf), "%lldm", m);
  } else {
    std::snprintf(buf, sizeof(buf), "%llds", static_cast<long long>(dur));
  }
  return buf;
}

}  // namespace timeutil
}  // namespace aps

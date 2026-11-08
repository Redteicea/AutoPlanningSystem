#include "aps/util/time_util.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>

namespace aps {
namespace timeutil {

namespace {
  
/// @brief 将 Unix 秒转本地日历分解 (tm); 跨平台兼容 localtime_s / localtime_r。
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

// 将本地日历分解反算为 Unix 秒 (mktime 按本地时区, 自动归一化溢出字段)。
Seconds fromTm(const std::tm& tmv) {
  std::tm t = tmv;
  return static_cast<Seconds>(std::mktime(&t));
}
}  // namespace

Seconds now() { return static_cast<Seconds>(std::time(nullptr)); }

Seconds dayStart(Seconds t) {
  std::tm tmv = localTm(t);
  tmv.tm_hour = 0;
  tmv.tm_min = 0;
  tmv.tm_sec = 0;
  return fromTm(tmv);
}

Seconds addDays(Seconds t, int days) {
  // 先按固定秒数平移再走本地日历, 让 mktime 处理跨月/夏令时归正。
  std::tm tmv = localTm(t + static_cast<Seconds>(days) * 86400);
  return fromTm(tmv);
}

int weekday(Seconds t) { return localTm(t).tm_wday; }

int hourOfDay(Seconds t) { return localTm(t).tm_hour; }

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

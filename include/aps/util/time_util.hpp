// ---------------------------------------------------------------------------
// util/time_util.hpp -- 时间统一表示
//
// 全系统只有一个时间单位: int64 秒 (Unix epoch)。理由:
//   * 秒级精度对排产足够 (工序时长普遍以分钟计), 避免毫秒/分钟的换算 bug
//   * 整数运算无浮点误差, 时间比较与区间判断可以直接用 < > ==
//   * 序列化到 JSON 就是普通数字, 前端 new Date(x*1000) 直接可用
// ---------------------------------------------------------------------------
//
// 内容概要:
//   - Seconds: 以 int64 秒为唯一时间单位 (Unix epoch)
//   - kInf: 表示"排不下/无穷"的特殊常量 (1<<62)
//   - now/dayStart/addDays: 基准时间换算
//   - weekday/hourOfDay: 日历分量提取
//   - format/parse: 与 "YYYY-MM-DD HH:MM" 字符串互转 (parse 失败返回 -1)
//   - humanDuration: 秒数转人类可读时长
#pragma once

#include <cstdint>
#include <string>

namespace aps {

using Seconds = std::int64_t;

constexpr Seconds kInf = (std::int64_t{1} << 62);

namespace timeutil {

/// 当前 Unix 秒 (本地时区下取整到秒)。
///
/// @return 自 1970-01-01 起的秒数。
Seconds now();

/// 返回 t 所在本地自然日的 00:00:00 (秒)。
///
/// @param t  任意 Unix 秒
/// @return   当天零点对应的 Unix 秒
Seconds dayStart(Seconds t);

/// 在 t 的基础上加 days 个自然日 (按本地日历, 自动处理夏令时/月边界)。
///
/// @param t     基准 Unix 秒
/// @param days  要加的天数 (可为负)
/// @return      加 days 天后的 Unix 秒
Seconds addDays(Seconds t, int days);

/// 星期几 (本地时区)。
///
/// @param t  Unix 秒
/// @return   0=周日 ... 6=周六
int weekday(Seconds t);

/// 当天小时数 (0~23)。
///
/// @param t  Unix 秒
/// @return   本地时区的小时分量
int hourOfDay(Seconds t);

/// 格式化为 "YYYY-MM-DD HH:MM" 或 "YYYY-MM-DD"。
///
/// @param t         Unix 秒
/// @param withTime  为 true 含时分, 为 false 仅日期
/// @return          格式化的字符串
std::string format(Seconds t, bool withTime = true);

/// 解析 "YYYY-MM-DD HH:MM" 或 "YYYY-MM-DD"; 也兼容纯数字时间戳。
///
/// @param s  输入字符串
/// @return   解析得到的 Unix 秒; 失败返回 -1
Seconds parse(const std::string& s);

/// 将秒数转为人类可读时长串, 如 "3h12m" / "45m" / "12s"。
///
/// @param dur  时长 (秒, 可为负, 负数加前缀 "-")
/// @return     形如 "3h12m" 的字符串
std::string humanDuration(Seconds dur);

}  // namespace timeutil
}  // namespace aps

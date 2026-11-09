// ---------------------------------------------------------------------------
// data/app_config.hpp -- 通用配置的本地文件读写
//
// 日历 (工作日/班次)、演示算例参数、求解默认值这类"通用配置"不再散落硬编码,
// 统一放在一个 JSON 文件里 (默认 config.json, 可用 --config 改路径):
//
//   - 启动时读取; 文件不存在则用内置默认值生成一份模板, 用户直接改文件即可
//   - 运行中通过 GET /api/config 读取、POST /api/config 修改 (落盘并重建算例)
//   - 命令行参数 (--orders/--days/--two-shifts) 优先级最高, 但不回写文件
//
// config.json 结构 (全部字段可省略, 省略时沿用内置默认):
// {
//   "calendar": {
//     "workdays": [1,2,3,4,5],                    // 0=周日 ... 6=周六
//     "shifts":   [ {"start":8,"end":12},
//                   {"start":13,"end":17} ],      // 24 小时制, end<=start 表跨夜班
//     "downtime": [ {"start":1700000000,"end":1700086400} ]  // 计划停机 (Unix 秒, 可选)
//   },
//   "demo":  { "orders": 80, "days": 14, "seed": 20261105 },
//   "solve": { "algo": "ga", "timeLimitMs": 2000 }
// }
// ---------------------------------------------------------------------------
//
// 内容概要:
//   - AppConfig: 通用配置载体 (日历 + 演示参数 + 求解默认值)
//   - defaultAppConfig / appConfigToJson / appConfigFromJson: 默认值与 JSON 转换
//   - loadAppConfig / saveAppConfig: 本地文件读写
//   - toDemoConfig: AppConfig -> DemoConfig (喂给 makeDemoFactory)
#pragma once

#include <cstdint>
#include <string>

#include "aps/core/model.hpp"
#include "aps/data/demo_data.hpp"
#include "aps/util/json.hpp"

namespace aps {

/// 通用配置: 厂级日历 + 演示算例参数 + 求解默认值 (见文件头注释的 JSON 结构)。
struct AppConfig {
  CalendarSpec calendar;           // 厂级日历 (工作日 + 班次 [+ 停机窗口])
  int orderCount = 80;             // 演示算例订单数
  int horizonDays = 14;            // 排产窗口天数
  std::uint64_t seed = 20261105;   // 演示算例随机种子 (保证可复现)
  std::string algo = "ga";         // 启动自动排产算法: rule | ga
  double timeLimitMs = 2000;       // GA 时间预算 (毫秒)
};

/// @brief 内置默认配置 (与 v1.0 硬编码行为一致: 周日休, 8-12 + 13-17 两班)。
AppConfig defaultAppConfig();

/// @brief 序列化为 JSON (GET /api/config 的返回体, 也是落盘内容的来源)。
json::Value appConfigToJson(const AppConfig& cfg);

/// @brief 从 JSON 解析配置。
/// @param v   解析好的 JSON 值 (对象); 缺失的字段沿用 out 现值 (部分更新友好)
/// @param out 解析结果写入目标 (调用方一般先填默认值)
/// @param err 非法值 (如空 workdays、越界班次) 时写入原因
/// @return    合法返回 true; 非法返回 false 且 out 保持不变
bool appConfigFromJson(const json::Value& v, AppConfig& out, std::string& err);

/// @brief 读配置文件。
/// @param path 配置文件路径 (JSON 文本)
/// @param out  读取成功时写入配置 (先以默认值打底, 再按文件内容覆盖)
/// @param err  文件不存在时为 "missing"; 解析/校验失败时带具体原因
/// @return     成功返回 true; false 时 out 内容为默认值, 调用方可直接使用
bool loadAppConfig(const std::string& path, AppConfig& out, std::string& err);

/// @brief 写配置文件 (2 空格缩进的可读格式, 方便手工编辑)。
/// @param path 目标文件路径; 父目录需已存在
/// @return     成功返回 true; 失败 err 带原因
bool saveAppConfig(const std::string& path, const AppConfig& cfg, std::string& err);

/// @brief 把通用配置转成演示算例生成参数 (日历总是随配置走)。
DemoConfig toDemoConfig(const AppConfig& cfg);

}  // namespace aps

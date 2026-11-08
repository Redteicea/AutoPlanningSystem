// ---------------------------------------------------------------------------
// data/demo_data.hpp -- 演示算例生成
//
// 场景取自卡片/印刷包装车间 (与本人做过的包装处理平台同行业, 数据构造有真实感):
//   印刷 -> 层压 -> 冲切 -> 写卡 -> 检测 -> 包装
//     ^       ^       ^        ^        ^
//   换版/换墨 换辊   换模    换程序   换规格
//
// 生成的数据满足: 工序顺序固定(偏 flow shop)、部分工序多机可选(柔性)、
// 换型时间依赖前后产品、急单占比约 8%。
// ---------------------------------------------------------------------------
//
// 内容概要:
//   - DemoConfig: 演示算例生成参数
//   - makeDemoFactory: 生成卡片印刷包装车间真实感算例
//   - makeSynthetic: 生成纯规模压测用合成算例 (无业务含义)
#pragma once

#include <cstdint>

#include "aps/core/model.hpp"

namespace aps {

struct DemoConfig {
  int orderCount = 80;       // 订单数量
  int horizonDays = 14;      // 排产窗口天数
  std::uint64_t seed = 20261105; // 随机种子 (决定确定性算例)
  bool twoShifts = false;    // true = 加夜班, 产能翻倍
};

/// 生成演示算例: 卡片印刷包装车间 (印刷→层压→冲切→写卡→检测→包装)。
///
/// @param cfg  订单数/窗口天数/种子/是否夜班
/// @return     构造好的 Problem (含设备/换型矩阵/工艺/订单)
Problem makeDemoFactory(const DemoConfig& cfg);

// 合成算例: 用于规模-性能曲线 (不追求业务含义, 只压规模)
//   orders 个订单 x opsPerOrder 道工序 x machines 台设备
/// 生成纯规模压测算例 (确定性, 单产品多工序, 每序 1~3 台可选设备)。
///
/// @param orders       订单数
/// @param opsPerOrder  每订单工序数
/// @param machines     设备数
/// @param seed         随机种子
/// @return             构造好的 Problem
Problem makeSynthetic(int orders, int opsPerOrder, int machines, std::uint64_t seed);

}  // namespace aps

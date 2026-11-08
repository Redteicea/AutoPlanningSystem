// ---------------------------------------------------------------------------
// util/random.hpp -- 确定性随机数 (splitmix64)
//
// 为什么不用 std::mt19937:
//   mt19937 构造慢、体积大; 排产 GA 需要"同一种子 = 同一结果"以便复现实验,
//   splitmix64 足够快、实现 10 行、跨平台结果一致。
// ---------------------------------------------------------------------------
//
// 内容概要:
//   - Rng: 确定性伪随机数发生器 (splitmix64), 同种子必得同序列
//   - nextU64/nextU32/nextDouble/nextInt: 各种区间采样
//   - chance: 以概率 p 命中
//   - shuffle/pick: 容器级随机洗牌与抽取 (用于 GA 变异/选择)
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace aps {

class Rng {
 public:
  /// 以 seed 构造; seed 为 0 时退化为 1, 避免产生全零状态。
  explicit Rng(std::uint64_t seed = 0x9E3779B97F4A7C15ULL) : s_(seed ? seed : 1) {}

  /// 产生下一个 64 位随机整数 (splitmix64 三步混洗)。
  ///
  /// @return  64 位无符号随机整数
  std::uint64_t nextU64() {
    s_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = s_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  /// 产生下一个 32 位随机整数 (取 nextU64 高 32 位)。
  std::uint32_t nextU32() { return static_cast<std::uint32_t>(nextU64() >> 32); }

  // [0, 1)
  /// 产生 [0,1) 区间双精度浮点 (53 位精度)。
  double nextDouble() { return static_cast<double>(nextU64() >> 11) * (1.0 / 9007199254740992.0); }

  // [0, n)
  /// 产生 [0, n) 区间整数; n<=1 时恒返回 0。
  ///
  /// @param n  上界 (不含)
  int nextInt(int n) {
    if (n <= 1) return 0;
    return static_cast<int>(nextU64() % static_cast<std::uint64_t>(n));
  }

  /// 以概率 p 返回 true (用于变异/接受等伯努利判定)。
  bool chance(double p) { return nextDouble() < p; }

  /// 原地 Fisher–Yates 洗牌, 等概率打乱 v 中元素顺序。
  template <typename T>
  void shuffle(std::vector<T>& v) {
    for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[nextInt(static_cast<int>(i))]);
  }

  /// 等概率随机抽取 v 中的一个元素 (v 非空)。
  template <typename T>
  const T& pick(const std::vector<T>& v) {
    return v[static_cast<size_t>(nextInt(static_cast<int>(v.size())))];
  }

 private:
  std::uint64_t s_;  // 内部状态 (64 位)
};

}  // namespace aps

// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Checked integer arithmetic for externally influenced sizes, capacities, and
// score aggregation. Overflow is never silently wrapped or silently saturated:
// callers receive an explicit signal and decide the outcome.

#ifndef FLOWPLACE_CHECKED_HPP
#define FLOWPLACE_CHECKED_HPP

#include <cstdint>
#include <limits>
#include <optional>

namespace flowplace {

// Returns std::nullopt on overflow.
[[nodiscard]] constexpr std::optional<std::uint64_t> CheckedAdd(std::uint64_t a,
                                                               std::uint64_t b) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) return std::nullopt;
  return a + b;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> CheckedSub(std::uint64_t a,
                                                               std::uint64_t b) noexcept {
  if (b > a) return std::nullopt;
  return a - b;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> CheckedMul(std::uint64_t a,
                                                               std::uint64_t b) noexcept {
  if (a == 0 || b == 0) return std::uint64_t{0};
  if (a > std::numeric_limits<std::uint64_t>::max() / b) return std::nullopt;
  return a * b;
}

// Saturating addition used where a bounded summary (not a decision input) may
// legitimately exceed the counter range.
[[nodiscard]] constexpr std::uint64_t SaturatingAdd(std::uint64_t a, std::uint64_t b) noexcept {
  const auto sum = CheckedAdd(a, b);
  return sum.has_value() ? *sum : std::numeric_limits<std::uint64_t>::max();
}

[[nodiscard]] constexpr std::uint64_t SaturatingMul(std::uint64_t a, std::uint64_t b) noexcept {
  const auto product = CheckedMul(a, b);
  return product.has_value() ? *product : std::numeric_limits<std::uint64_t>::max();
}

// Narrowing helpers that return std::nullopt instead of truncating.
[[nodiscard]] constexpr std::optional<std::uint32_t> NarrowU32(std::uint64_t v) noexcept {
  if (v > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
  return static_cast<std::uint32_t>(v);
}

[[nodiscard]] constexpr std::optional<std::uint16_t> NarrowU16(std::uint64_t v) noexcept {
  if (v > std::numeric_limits<std::uint16_t>::max()) return std::nullopt;
  return static_cast<std::uint16_t>(v);
}

[[nodiscard]] constexpr std::optional<std::uint8_t> NarrowU8(std::uint64_t v) noexcept {
  if (v > std::numeric_limits<std::uint8_t>::max()) return std::nullopt;
  return static_cast<std::uint8_t>(v);
}

}  // namespace flowplace

#endif  // FLOWPLACE_CHECKED_HPP

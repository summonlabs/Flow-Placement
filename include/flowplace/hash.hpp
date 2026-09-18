// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Dependency-free integrity and identity hashing.
//
//  * Crc32C  - table-driven integrity check for durable records and wire frames.
//  * Fnv1a64 - canonical identity digest for decisions and inputs.
//
// CanonicalHasher mixes values with explicit little-endian byte order and
// length-prefixed strings, so a digest computed on one platform equals the
// digest computed on another. Digests are witnesses of equality of inputs, not
// security primitives; they are not used for authentication.

#ifndef FLOWPLACE_HASH_HPP
#define FLOWPLACE_HASH_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace flowplace {

class Crc32C {
 public:
  Crc32C() noexcept = default;

  void Update(const void* data, std::size_t size) noexcept;
  void Update(std::string_view text) noexcept {
    Update(text.data(), text.size());
  }
  [[nodiscard]] std::uint32_t value() const noexcept { return ~state_; }

  [[nodiscard]] static std::uint32_t Compute(const void* data, std::size_t size) noexcept {
    Crc32C crc;
    crc.Update(data, size);
    return crc.value();
  }
  [[nodiscard]] static std::uint32_t Compute(std::string_view text) noexcept {
    return Compute(text.data(), text.size());
  }

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

class Fnv1a64 {
 public:
  Fnv1a64() noexcept = default;

  void Update(const void* data, std::size_t size) noexcept;
  [[nodiscard]] std::uint64_t value() const noexcept { return state_; }

  [[nodiscard]] static std::uint64_t Compute(const void* data, std::size_t size) noexcept {
    Fnv1a64 h;
    h.Update(data, size);
    return h.value();
  }

 private:
  std::uint64_t state_ = 0xCBF29CE484222325ull;
};

// Canonical, platform-stable hasher for domain-separated identity digests.
class CanonicalHasher {
 public:
  CanonicalHasher() noexcept = default;

  void AddTag(std::string_view tag) noexcept;
  void AddU8(std::uint8_t v) noexcept;
  void AddU16(std::uint16_t v) noexcept;
  void AddU32(std::uint32_t v) noexcept;
  void AddU64(std::uint64_t v) noexcept;
  void AddI64(std::int64_t v) noexcept;
  void AddBool(bool v) noexcept { AddU8(v ? 1u : 0u); }
  void AddString(std::string_view v) noexcept;
  void AddBlob(const void* data, std::size_t size) noexcept;

  [[nodiscard]] std::uint64_t value() const noexcept { return hasher_.value(); }

 private:
  Fnv1a64 hasher_;
};

// 128-bit identity digest rendered as 32 lowercase hex characters.
struct Digest {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;

  [[nodiscard]] bool operator==(const Digest& other) const noexcept {
    return hi == other.hi && lo == other.lo;
  }
  [[nodiscard]] bool operator!=(const Digest& other) const noexcept { return !(*this == other); }
  [[nodiscard]] bool operator<(const Digest& other) const noexcept {
    return hi != other.hi ? hi < other.hi : lo < other.lo;
  }
  [[nodiscard]] bool IsZero() const noexcept { return hi == 0 && lo == 0; }
  [[nodiscard]] std::string ToHex() const;

  // Deterministic digest of a canonical byte stream: two independent FNV-1a
  // passes with different offset bases.
  [[nodiscard]] static Digest OfCanonical(const CanonicalHasher& hasher) noexcept;
};

}  // namespace flowplace

#endif  // FLOWPLACE_HASH_HPP

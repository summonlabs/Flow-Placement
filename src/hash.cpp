// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/hash.hpp"

#include <array>

namespace flowplace {
namespace {

// CRC-32C (Castagnoli) reflected polynomial 0x82F63B78.
struct Crc32cTable {
  std::array<std::uint32_t, 256> entries{};
  constexpr Crc32cTable() : entries() {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) ? ((crc >> 1) ^ 0x82F63B78u) : (crc >> 1);
      }
      // The mask makes the index provably inside the table for static analysis
      // as well as for a reader.
      constexpr std::uint32_t kMask = 0xFFu;
      if ((i & kMask) < entries.size()) entries[i & kMask] = crc;
    }
  }
};

constexpr Crc32cTable kCrc32cTable{};
constexpr std::uint64_t kFnvOffsetBasis = 0xCBF29CE484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ull;
constexpr std::uint64_t kSecondOffsetBasis = 0x84222325CBF29CE4ull;

}  // namespace

void Crc32C::Update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t state = state_;
  for (std::size_t i = 0; i < size; ++i) {
    state = kCrc32cTable.entries[(state ^ bytes[i]) & 0xFFu] ^ (state >> 8);
  }
  state_ = state;
}

void Fnv1a64::Update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint64_t state = state_;
  for (std::size_t i = 0; i < size; ++i) {
    state ^= bytes[i];
    state *= kFnvPrime;
  }
  state_ = state;
}

void CanonicalHasher::AddTag(std::string_view tag) noexcept {
  AddU8(0x1F);
  AddString(tag);
}

void CanonicalHasher::AddU8(std::uint8_t v) noexcept {
  hasher_.Update(&v, 1);
}

void CanonicalHasher::AddU16(std::uint16_t v) noexcept {
  const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(v & 0xFFu),
                                 static_cast<std::uint8_t>((v >> 8) & 0xFFu)};
  hasher_.Update(bytes, 2);
}

void CanonicalHasher::AddU32(std::uint32_t v) noexcept {
  const std::uint8_t bytes[4] = {
      static_cast<std::uint8_t>(v & 0xFFu), static_cast<std::uint8_t>((v >> 8) & 0xFFu),
      static_cast<std::uint8_t>((v >> 16) & 0xFFu), static_cast<std::uint8_t>((v >> 24) & 0xFFu)};
  hasher_.Update(bytes, 4);
}

void CanonicalHasher::AddU64(std::uint64_t v) noexcept {
  std::uint8_t bytes[8];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
  }
  hasher_.Update(bytes, 8);
}

void CanonicalHasher::AddI64(std::int64_t v) noexcept {
  AddU64(static_cast<std::uint64_t>(v));
}

void CanonicalHasher::AddString(std::string_view v) noexcept {
  AddU32(static_cast<std::uint32_t>(v.size()));
  if (!v.empty()) hasher_.Update(v.data(), v.size());
}

void CanonicalHasher::AddBlob(const void* data, std::size_t size) noexcept {
  AddU32(static_cast<std::uint32_t>(size));
  if (size != 0) hasher_.Update(data, size);
}

Digest Digest::OfCanonical(const CanonicalHasher& hasher) noexcept {
  // Two independent passes over the same canonical byte stream, distinguished
  // by offset basis, give a 128-bit identity witness without external
  // dependencies. This is an identity digest, not a cryptographic hash.
  const std::uint64_t lo = hasher.value();
  Digest out;
  out.lo = lo;
  std::uint64_t mixed = lo ^ kSecondOffsetBasis;
  mixed *= kFnvPrime;
  mixed ^= mixed >> 29;
  mixed *= 0xBF58476D1CE4E5B9ull;
  mixed ^= mixed >> 32;
  out.hi = mixed ^ kFnvOffsetBasis;
  return out;
}

std::string Digest::ToHex() const {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  const std::uint64_t parts[2] = {hi, lo};
  for (std::uint64_t part : parts) {
    for (int shift = 60; shift >= 0; shift -= 4) {
      out.push_back(kHex[(part >> shift) & 0xFull]);
    }
  }
  return out;
}

}  // namespace flowplace

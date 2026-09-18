// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Explicit little-endian byte encoding. Every integer written by this library
// (durable records, wire frames, canonical payloads) is encoded byte by byte in
// little-endian order so that a store or a frame produced on one platform is
// read identically on another. Readers are bounded: a short read is an error,
// never a zero-filled value.

#ifndef FLOWPLACE_BYTES_HPP
#define FLOWPLACE_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "flowplace/status.hpp"

namespace flowplace {

class ByteWriter {
 public:
  void U8(std::uint8_t v);
  void U16(std::uint16_t v);
  void U32(std::uint32_t v);
  void U64(std::uint64_t v);
  void I64(std::int64_t v);
  void Bytes(const void* data, std::size_t size);
  void String(std::string_view v);  // length-prefixed (u32)

  [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
  [[nodiscard]] std::vector<std::uint8_t>& data() noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  void Clear() noexcept { data_.clear(); }

 private:
  std::vector<std::uint8_t> data_;
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) noexcept
      : data_(data), size_(size) {}
  explicit ByteReader(const std::vector<std::uint8_t>& data) noexcept
      : data_(data.data()), size_(data.size()) {}
  explicit ByteReader(std::string_view text) noexcept
      : data_(reinterpret_cast<const std::uint8_t*>(text.data())), size_(text.size()) {}

  [[nodiscard]] bool U8(std::uint8_t* out);
  [[nodiscard]] bool U16(std::uint16_t* out);
  [[nodiscard]] bool U32(std::uint32_t* out);
  [[nodiscard]] bool U64(std::uint64_t* out);
  [[nodiscard]] bool I64(std::int64_t* out);
  [[nodiscard]] bool Bytes(void* out, std::size_t size);
  // Length-prefixed string; |max_length| bounds the accepted size.
  [[nodiscard]] bool String(std::string* out, std::uint32_t max_length);

  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool AtEnd() const noexcept { return offset_ == size_; }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
};

}  // namespace flowplace

#endif  // FLOWPLACE_BYTES_HPP

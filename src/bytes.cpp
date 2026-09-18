// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/bytes.hpp"

namespace flowplace {

void ByteWriter::U8(std::uint8_t v) { data_.push_back(v); }

void ByteWriter::U16(std::uint16_t v) {
  data_.push_back(static_cast<std::uint8_t>(v & 0xFFu));
  data_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void ByteWriter::U32(std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    data_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }
}

void ByteWriter::U64(std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    data_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }
}

void ByteWriter::I64(std::int64_t v) { U64(static_cast<std::uint64_t>(v)); }

void ByteWriter::Bytes(const void* data, std::size_t size) {
  if (size == 0) return;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  data_.insert(data_.end(), bytes, bytes + size);
}

void ByteWriter::String(std::string_view v) {
  U32(static_cast<std::uint32_t>(v.size()));
  Bytes(v.data(), v.size());
}

bool ByteReader::U8(std::uint8_t* out) {
  if (remaining() < 1) return false;
  *out = data_[offset_++];
  return true;
}

bool ByteReader::U16(std::uint16_t* out) {
  if (remaining() < 2) return false;
  *out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_]) |
                                    (static_cast<std::uint16_t>(data_[offset_ + 1]) << 8));
  offset_ += 2;
  return true;
}

bool ByteReader::U32(std::uint32_t* out) {
  if (remaining() < 4) return false;
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(i)]) << (8 * i);
  }
  offset_ += 4;
  *out = value;
  return true;
}

bool ByteReader::U64(std::uint64_t* out) {
  if (remaining() < 8) return false;
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(i)]) << (8 * i);
  }
  offset_ += 8;
  *out = value;
  return true;
}

bool ByteReader::I64(std::int64_t* out) {
  std::uint64_t raw = 0;
  if (!U64(&raw)) return false;
  *out = static_cast<std::int64_t>(raw);
  return true;
}

bool ByteReader::Bytes(void* out, std::size_t size) {
  if (remaining() < size) return false;
  if (size != 0) std::memcpy(out, data_ + offset_, size);
  offset_ += size;
  return true;
}

bool ByteReader::String(std::string* out, std::uint32_t max_length) {
  std::uint32_t length = 0;
  if (!U32(&length)) return false;
  if (length > max_length) return false;
  if (remaining() < length) return false;
  out->assign(reinterpret_cast<const char*>(data_ + offset_), length);
  offset_ += length;
  return true;
}

}  // namespace flowplace

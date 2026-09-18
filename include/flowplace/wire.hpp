// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Framed wire protocol. Every frame is length-prefixed and integrity checked:
//
//   offset  size  field
//   0       4     frame length in bytes (little-endian, excludes this header)
//   4       4     CRC-32C of the frame body
//   8       2     frame kind
//   10      2     flags
//   12      8     sequence
//   20      L     body
//
// A frame whose declared length exceeds the negotiated maximum is rejected
// before any body byte is buffered. A frame whose CRC does not match is
// rejected. The stream decoder never allocates more than the negotiated bound.

#ifndef FLOWPLACE_WIRE_HPP
#define FLOWPLACE_WIRE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "flowplace/bytes.hpp"
#include "flowplace/model.hpp"
#include "flowplace/runtime.hpp"
#include "flowplace/status.hpp"

namespace flowplace {

inline constexpr std::uint32_t kFrameHeaderBytes = 20;
inline constexpr std::uint32_t kDefaultMaxFrameBytes = 8u << 20;  // 8 MiB
inline constexpr std::uint32_t kMinMaxFrameBytes = 1024;

enum class FrameKind : std::uint16_t {
  kHello = 1,
  kHelloAck = 2,
  kPlaceRequest = 3,
  kPlaceResult = 4,
  kSetEpoch = 5,
  kEpochAck = 6,
  kShutdown = 7,
  kShutdownAck = 8,
  kError = 9,
  kRecoveryQuery = 10,
  kRecoveryReport = 11,
  kStatsQuery = 12,
  kStatsReport = 13,
};

std::string_view FrameKindName(FrameKind v) noexcept;
std::optional<FrameKind> ParseFrameKind(std::uint16_t v) noexcept;

struct Frame {
  FrameKind kind = FrameKind::kHello;
  std::uint16_t flags = 0;
  std::uint64_t sequence = 0;
  std::vector<std::uint8_t> body;
};

// Encode one frame. Returns kFrameTooLarge when the body exceeds |max_frame_bytes|.
Status EncodeFrame(const Frame& frame, std::vector<std::uint8_t>* out,
                   std::uint32_t max_frame_bytes = kDefaultMaxFrameBytes);

// Decode exactly one frame from a complete buffer. Trailing bytes are rejected
// so that a mismatch between declared and actual size cannot pass unnoticed.
Status DecodeFrame(const std::uint8_t* data, std::size_t size, Frame* out,
                   std::uint32_t max_frame_bytes = kDefaultMaxFrameBytes);

// Incremental stream decoder with a bounded buffer.
class FrameStream {
 public:
  explicit FrameStream(std::uint32_t max_frame_bytes = kDefaultMaxFrameBytes)
      : max_frame_bytes_(max_frame_bytes) {}

  Status Feed(const void* data, std::size_t size);
  // Returns kOk with nullopt when more bytes are needed.
  Status Next(std::optional<Frame>* out);
  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - offset_; }
  [[nodiscard]] std::uint32_t max_frame_bytes() const noexcept { return max_frame_bytes_; }
  void SetMaxFrameBytes(std::uint32_t v) noexcept { max_frame_bytes_ = v; }

 private:
  std::vector<std::uint8_t> buffer_;
  std::size_t offset_ = 0;
  std::uint32_t max_frame_bytes_;
};

// ---- payloads ------------------------------------------------------------
inline constexpr std::uint32_t kMaxWireStringBytes = 4096;

struct HelloPayload {
  std::uint32_t protocol_version = 0;
  IncarnationId incarnation;
  FabricEpoch epoch;
  WorkerId worker;
  std::uint32_t max_frame_bytes = kDefaultMaxFrameBytes;
  std::string build;
};

struct HelloAckPayload {
  std::uint32_t protocol_version = 0;
  IncarnationId coordinator;
  FabricEpoch epoch;
  std::uint32_t max_frame_bytes = kDefaultMaxFrameBytes;
  std::uint32_t worker_limit = 0;
  std::string build;
  bool accepted = false;
  StatusCode code = StatusCode::kOk;
  std::string message;
};

struct PlaceRequestPayload {
  AttemptId attempt;
  FabricEpoch epoch;
  PlacementRequest request;
};

struct PlaceResultPayload {
  AttemptId attempt;
  AttemptState state = AttemptState::kRejected;
  StatusCode code = StatusCode::kOk;
  PlacementId placement_id;
  PlacementGeneration placement_generation;
  PlacementDecision decision;
};

struct EpochPayload {
  FabricEpoch epoch;
  FabricEpoch previous_epoch;
  StatusCode code = StatusCode::kOk;
  std::string message;
};

struct ErrorPayload {
  StatusCode code = StatusCode::kOk;
  AttemptId attempt;
  std::string message;
};

void EncodeHello(const HelloPayload& v, ByteWriter& w);
Status DecodeHello(ByteReader& r, HelloPayload* out);
void EncodeHelloAck(const HelloAckPayload& v, ByteWriter& w);
Status DecodeHelloAck(ByteReader& r, HelloAckPayload* out);
void EncodePlaceRequest(const PlaceRequestPayload& v, ByteWriter& w);
Status DecodePlaceRequest(ByteReader& r, const Limits& limits, PlaceRequestPayload* out);
void EncodePlaceResult(const PlaceResultPayload& v, ByteWriter& w);
Status DecodePlaceResult(ByteReader& r, const Limits& limits, PlaceResultPayload* out);
void EncodeEpochPayload(const EpochPayload& v, ByteWriter& w);
Status DecodeEpochPayload(ByteReader& r, EpochPayload* out);
void EncodeErrorPayload(const ErrorPayload& v, ByteWriter& w);
Status DecodeErrorPayload(ByteReader& r, ErrorPayload* out);

}  // namespace flowplace

#endif  // FLOWPLACE_WIRE_HPP

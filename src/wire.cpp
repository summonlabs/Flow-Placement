// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/wire.hpp"

#include <cstring>

#include "flowplace/codec.hpp"
#include "flowplace/hash.hpp"

namespace flowplace {
namespace {

constexpr std::size_t kOffsetLength = 0;
constexpr std::size_t kOffsetCrc = 4;
constexpr std::size_t kOffsetKind = 8;
constexpr std::size_t kOffsetFlags = 10;
constexpr std::size_t kOffsetSequence = 12;

void PutU16(std::uint8_t* out, std::uint16_t v) {
  out[0] = static_cast<std::uint8_t>(v & 0xFFu);
  out[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

void PutU32(std::uint8_t* out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
}

void PutU64(std::uint8_t* out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
}

std::uint16_t GetU16(const std::uint8_t* in) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                    (static_cast<std::uint16_t>(in[1]) << 8));
}

std::uint32_t GetU32(const std::uint8_t* in) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(in[i]) << (8 * i);
  return v;
}

std::uint64_t GetU64(const std::uint8_t* in) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(in[i]) << (8 * i);
  return v;
}

}  // namespace

std::string_view FrameKindName(FrameKind v) noexcept {
  switch (v) {
    case FrameKind::kHello: return "HELLO";
    case FrameKind::kHelloAck: return "HELLO_ACK";
    case FrameKind::kPlaceRequest: return "PLACE_REQUEST";
    case FrameKind::kPlaceResult: return "PLACE_RESULT";
    case FrameKind::kSetEpoch: return "SET_EPOCH";
    case FrameKind::kEpochAck: return "EPOCH_ACK";
    case FrameKind::kShutdown: return "SHUTDOWN";
    case FrameKind::kShutdownAck: return "SHUTDOWN_ACK";
    case FrameKind::kError: return "ERROR";
    case FrameKind::kRecoveryQuery: return "RECOVERY_QUERY";
    case FrameKind::kRecoveryReport: return "RECOVERY_REPORT";
    case FrameKind::kStatsQuery: return "STATS_QUERY";
    case FrameKind::kStatsReport: return "STATS_REPORT";
  }
  return "UNKNOWN_FRAME";
}

std::optional<FrameKind> ParseFrameKind(std::uint16_t v) noexcept {
  switch (v) {
    case 1: return FrameKind::kHello;
    case 2: return FrameKind::kHelloAck;
    case 3: return FrameKind::kPlaceRequest;
    case 4: return FrameKind::kPlaceResult;
    case 5: return FrameKind::kSetEpoch;
    case 6: return FrameKind::kEpochAck;
    case 7: return FrameKind::kShutdown;
    case 8: return FrameKind::kShutdownAck;
    case 9: return FrameKind::kError;
    case 10: return FrameKind::kRecoveryQuery;
    case 11: return FrameKind::kRecoveryReport;
    case 12: return FrameKind::kStatsQuery;
    case 13: return FrameKind::kStatsReport;
    default: return std::nullopt;
  }
}

Status EncodeFrame(const Frame& frame, std::vector<std::uint8_t>* out,
                   std::uint32_t max_frame_bytes) {
  if (out == nullptr) return Status(StatusCode::kInvalidArgument, "output buffer is null");
  if (max_frame_bytes < kMinMaxFrameBytes) {
    return Status(StatusCode::kInvalidArgument, "negotiated frame bound is below the minimum");
  }
  if (frame.body.size() > max_frame_bytes) {
    return Status(StatusCode::kFrameTooLarge, "frame body exceeds the negotiated bound");
  }
  out->assign(kFrameHeaderBytes + frame.body.size(), 0);
  std::uint8_t* data = out->data();
  PutU32(data + kOffsetLength, static_cast<std::uint32_t>(frame.body.size()));
  PutU32(data + kOffsetCrc,
         Crc32C::Compute(frame.body.data(), frame.body.size()));
  PutU16(data + kOffsetKind, static_cast<std::uint16_t>(frame.kind));
  PutU16(data + kOffsetFlags, frame.flags);
  PutU64(data + kOffsetSequence, frame.sequence);
  if (!frame.body.empty()) {
    std::memcpy(data + kFrameHeaderBytes, frame.body.data(), frame.body.size());
  }
  return Status::Ok();
}

Status DecodeFrame(const std::uint8_t* data, std::size_t size, Frame* out,
                   std::uint32_t max_frame_bytes) {
  if (data == nullptr || out == nullptr) {
    return Status(StatusCode::kInvalidArgument, "null frame buffer");
  }
  if (size < kFrameHeaderBytes) {
    return Status(StatusCode::kWireMalformed, "frame is shorter than its header");
  }
  const std::uint32_t declared = GetU32(data + kOffsetLength);
  if (declared > max_frame_bytes) {
    return Status(StatusCode::kFrameTooLarge, "declared frame length exceeds the negotiated bound");
  }
  if (size != kFrameHeaderBytes + static_cast<std::size_t>(declared)) {
    return Status(StatusCode::kWireMalformed, "declared frame length does not match the buffer");
  }
  const std::uint8_t* body = data + kFrameHeaderBytes;
  if (Crc32C::Compute(body, declared) != GetU32(data + kOffsetCrc)) {
    return Status(StatusCode::kFrameChecksumMismatch, "frame checksum mismatch");
  }
  const std::uint16_t raw_kind = GetU16(data + kOffsetKind);
  const std::optional<FrameKind> kind = ParseFrameKind(raw_kind);
  if (!kind.has_value()) {
    return Status(StatusCode::kWireMalformed, "unknown frame kind");
  }
  out->kind = *kind;
  out->flags = GetU16(data + kOffsetFlags);
  out->sequence = GetU64(data + kOffsetSequence);
  out->body.assign(body, body + declared);
  return Status::Ok();
}

Status FrameStream::Feed(const void* data, std::size_t size) {
  if (size == 0) return Status::Ok();
  if (data == nullptr) return Status(StatusCode::kInvalidArgument, "null feed buffer");
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  buffer_.insert(buffer_.end(), bytes, bytes + size);
  return Status::Ok();
}

Status FrameStream::Next(std::optional<Frame>* out) {
  if (out == nullptr) return Status(StatusCode::kInvalidArgument, "output frame is null");
  out->reset();
  if (offset_ != 0) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
    offset_ = 0;
  }
  if (buffer_.size() < kFrameHeaderBytes) return Status::Ok();
  const std::uint32_t declared = GetU32(buffer_.data() + kOffsetLength);
  if (declared > max_frame_bytes_) {
    return Status(StatusCode::kFrameTooLarge, "declared frame length exceeds the negotiated bound");
  }
  const std::size_t total = kFrameHeaderBytes + static_cast<std::size_t>(declared);
  if (buffer_.size() < total) return Status::Ok();
  Frame frame;
  const Status status = DecodeFrame(buffer_.data(), total, &frame, max_frame_bytes_);
  if (!status.ok()) return status;
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total));
  *out = std::move(frame);
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// Payload codecs
// ---------------------------------------------------------------------------

void EncodeHello(const HelloPayload& v, ByteWriter& w) {
  w.U32(v.protocol_version);
  w.U64(v.incarnation.value());
  w.U64(v.epoch.value());
  w.U64(v.worker.value());
  w.U32(v.max_frame_bytes);
  w.String(v.build);
}

Status DecodeHello(ByteReader& r, HelloPayload* out) {
  if (!r.U32(&out->protocol_version)) return Status(StatusCode::kWireMalformed, "truncated hello");
  std::uint64_t raw = 0;
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated hello incarnation");
  out->incarnation = IncarnationId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated hello epoch");
  out->epoch = FabricEpoch{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated hello worker");
  out->worker = WorkerId{raw};
  if (!r.U32(&out->max_frame_bytes)) return Status(StatusCode::kWireMalformed, "truncated hello bound");
  if (out->max_frame_bytes < kMinMaxFrameBytes) {
    return Status(StatusCode::kWireMalformed, "hello frame bound is below the minimum");
  }
  if (!r.String(&out->build, kMaxWireStringBytes)) {
    return Status(StatusCode::kWireMalformed, "truncated hello build string");
  }
  if (!r.AtEnd()) return Status(StatusCode::kWireMalformed, "trailing bytes after hello");
  return Status::Ok();
}

void EncodeHelloAck(const HelloAckPayload& v, ByteWriter& w) {
  w.U32(v.protocol_version);
  w.U64(v.coordinator.value());
  w.U64(v.epoch.value());
  w.U32(v.max_frame_bytes);
  w.U32(v.worker_limit);
  w.String(v.build);
  w.U8(v.accepted ? 1u : 0u);
  w.U16(static_cast<std::uint16_t>(v.code));
  w.String(v.message);
}

Status DecodeHelloAck(ByteReader& r, HelloAckPayload* out) {
  if (!r.U32(&out->protocol_version)) return Status(StatusCode::kWireMalformed, "truncated hello ack");
  std::uint64_t raw = 0;
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated hello ack coordinator");
  out->coordinator = IncarnationId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated hello ack epoch");
  out->epoch = FabricEpoch{raw};
  if (!r.U32(&out->max_frame_bytes) || !r.U32(&out->worker_limit)) {
    return Status(StatusCode::kWireMalformed, "truncated hello ack bounds");
  }
  if (out->max_frame_bytes < kMinMaxFrameBytes) {
    return Status(StatusCode::kWireMalformed, "hello ack frame bound is below the minimum");
  }
  if (!r.String(&out->build, kMaxWireStringBytes)) {
    return Status(StatusCode::kWireMalformed, "truncated hello ack build string");
  }
  std::uint8_t accepted = 0;
  if (!r.U8(&accepted)) return Status(StatusCode::kWireMalformed, "truncated hello ack flag");
  if (accepted > 1) return Status(StatusCode::kWireMalformed, "malformed hello ack flag");
  out->accepted = accepted != 0;
  std::uint16_t code = 0;
  if (!r.U16(&code)) return Status(StatusCode::kWireMalformed, "truncated hello ack code");
  if (!IsKnownStatusCode(code)) {
    return Status(StatusCode::kWireMalformed, "unknown hello ack status code");
  }
  out->code = static_cast<StatusCode>(code);
  if (!r.String(&out->message, kMaxWireStringBytes)) {
    return Status(StatusCode::kWireMalformed, "truncated hello ack message");
  }
  if (!r.AtEnd()) return Status(StatusCode::kWireMalformed, "trailing bytes after hello ack");
  return Status::Ok();
}

void EncodePlaceRequest(const PlaceRequestPayload& v, ByteWriter& w) {
  w.U64(v.attempt.value());
  w.U64(v.epoch.value());
  EncodeRequest(v.request, w);
}

Status DecodePlaceRequest(ByteReader& r, const Limits& limits, PlaceRequestPayload* out) {
  std::uint64_t raw = 0;
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated place request attempt");
  out->attempt = AttemptId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated place request epoch");
  out->epoch = FabricEpoch{raw};
  return DecodeRequest(r, limits, &out->request);
}

void EncodePlaceResult(const PlaceResultPayload& v, ByteWriter& w) {
  w.U64(v.attempt.value());
  w.U8(static_cast<std::uint8_t>(v.state));
  w.U16(static_cast<std::uint16_t>(v.code));
  w.U64(v.placement_id.value());
  w.U64(v.placement_generation.value());
  EncodeDecision(v.decision, w);
}

Status DecodePlaceResult(ByteReader& r, const Limits& limits, PlaceResultPayload* out) {
  std::uint64_t raw = 0;
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated place result attempt");
  out->attempt = AttemptId{raw};
  std::uint8_t state = 0;
  if (!r.U8(&state) || state > 6) return Status(StatusCode::kWireMalformed, "malformed attempt state");
  out->state = static_cast<AttemptState>(state);
  std::uint16_t code = 0;
  if (!r.U16(&code)) return Status(StatusCode::kWireMalformed, "truncated place result code");
  if (!IsKnownStatusCode(code)) {
    return Status(StatusCode::kWireMalformed, "unknown place result status code");
  }
  out->code = static_cast<StatusCode>(code);
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated place result id");
  out->placement_id = PlacementId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated place result generation");
  out->placement_generation = PlacementGeneration{raw};
  return DecodeDecision(r, limits, &out->decision);
}

void EncodeEpochPayload(const EpochPayload& v, ByteWriter& w) {
  w.U64(v.epoch.value());
  w.U64(v.previous_epoch.value());
  w.U16(static_cast<std::uint16_t>(v.code));
  w.String(v.message);
}

Status DecodeEpochPayload(ByteReader& r, EpochPayload* out) {
  std::uint64_t raw = 0;
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated epoch");
  out->epoch = FabricEpoch{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated previous epoch");
  out->previous_epoch = FabricEpoch{raw};
  std::uint16_t code = 0;
  if (!r.U16(&code)) return Status(StatusCode::kWireMalformed, "truncated epoch code");
  if (!IsKnownStatusCode(code)) {
    return Status(StatusCode::kWireMalformed, "unknown epoch status code");
  }
  out->code = static_cast<StatusCode>(code);
  if (!r.String(&out->message, kMaxWireStringBytes)) {
    return Status(StatusCode::kWireMalformed, "truncated epoch message");
  }
  if (!r.AtEnd()) return Status(StatusCode::kWireMalformed, "trailing bytes after epoch payload");
  return Status::Ok();
}

void EncodeErrorPayload(const ErrorPayload& v, ByteWriter& w) {
  w.U16(static_cast<std::uint16_t>(v.code));
  w.U64(v.attempt.value());
  w.String(v.message);
}

Status DecodeErrorPayload(ByteReader& r, ErrorPayload* out) {
  std::uint16_t code = 0;
  if (!r.U16(&code)) return Status(StatusCode::kWireMalformed, "truncated error code");
  if (!IsKnownStatusCode(code)) {
    return Status(StatusCode::kWireMalformed, "unknown error status code");
  }
  out->code = static_cast<StatusCode>(code);
  std::uint64_t raw = 0;
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated error attempt");
  out->attempt = AttemptId{raw};
  if (!r.String(&out->message, kMaxWireStringBytes)) {
    return Status(StatusCode::kWireMalformed, "truncated error message");
  }
  if (!r.AtEnd()) return Status(StatusCode::kWireMalformed, "trailing bytes after error payload");
  return Status::Ok();
}

}  // namespace flowplace

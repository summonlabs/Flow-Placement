// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fixtures.hpp"
#include "harness.hpp"

#include <string>
#include <vector>

#include "flowplace/codec.hpp"
#include "flowplace/version.hpp"
#include "flowplace/wire.hpp"

using namespace flowplace;
using namespace fptest;

namespace {

bool SameIntent(const PlacementIntent& a, const PlacementIntent& b) { return a == b; }

}  // namespace

FP_TEST(codec_wire, request_round_trip_preserves_every_field) {
  PlacementRequest request = Baseline(5);
  SetIncumbentOn(request, PathId{3});
  request.policy.locality.required_locality = LocalityId{10};
  request.policy.failure_domains.avoid_occupied_domains = true;
  request.occupied_failure_domains = {FailureDomainId{99}};
  request.attempt = AttemptId{1234};
  ReservationRef ref;
  ref.id = ReservationId{8};
  ref.generation = ReservationGeneration{3};
  request.candidates.paths[1].attributes.reservations = {ref};
  request.policy.reservation_affinity.refs = {ref};
  request.policy.evidence.on_missing = EvidencePolicyMode::kRankWorst;
  request.policy.service.allow_degraded = true;
  request.policy.service.max_relaxation_steps = 2;
  request.policy.churn.prefer_incumbent = true;
  request.policy.churn.move_improvement_threshold = 11;
  request.provenance.observer_unix_nanos = -424242;

  ByteWriter writer;
  EncodeRequest(request, writer);
  ByteReader reader(writer.data());
  PlacementRequest decoded;
  FP_REQUIRE(DecodeRequest(reader, Limits{}, &decoded).ok());
  FP_CHECK_EQ(RequestDigest(request, Limits{}), RequestDigest(decoded, Limits{}));
  FP_CHECK(decoded.incumbent.has_value());
  FP_CHECK_EQ(decoded.incumbent->path, PathId{3});
  FP_CHECK_EQ(decoded.provenance.observer_unix_nanos, std::int64_t{-424242});
  FP_CHECK_EQ(decoded.policy.churn.move_improvement_threshold, std::uint64_t{11});
  FP_CHECK(decoded.policy.service.allow_degraded);
  FP_CHECK_EQ(decoded.candidates.paths[1].attributes.reservations.size(), std::size_t{1});
  const PlacementEngine engine;
  FP_CHECK_EQ(engine.Place(request).digest, engine.Place(decoded).digest);
}

FP_TEST(codec_wire, decision_and_intent_round_trip) {
  const PlacementRequest request = Baseline(3);
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_REQUIRE(decision.intent.has_value());

  ByteWriter writer;
  EncodeDecision(decision, writer);
  ByteReader reader(writer.data());
  PlacementDecision decoded;
  FP_REQUIRE(DecodeDecision(reader, Limits{}, &decoded).ok());
  FP_CHECK_EQ(decoded.digest, decision.digest);
  FP_CHECK_EQ(decoded.outcome, decision.outcome);
  FP_CHECK_EQ(decoded.code, decision.code);
  FP_CHECK_EQ(decoded.delta, decision.delta);
  FP_REQUIRE(decoded.intent.has_value());
  FP_CHECK(SameIntent(*decoded.intent, *decision.intent));
  FP_CHECK_EQ(decoded.explanation.ranked.size(), decision.explanation.ranked.size());
  FP_CHECK_EQ(decoded.authority.ToString(), decision.authority.ToString());
  FP_CHECK_EQ(DecisionDigest(decoded), DecisionDigest(decision));

  ByteWriter intent_writer;
  EncodeIntent(*decision.intent, intent_writer);
  ByteReader intent_reader(intent_writer.data());
  PlacementIntent intent;
  FP_REQUIRE(DecodeIntent(intent_reader, &intent).ok());
  FP_CHECK_EQ(IntentDigest(intent), decision.intent->digest);
}

FP_TEST(codec_wire, attempt_and_placement_records_round_trip) {
  AttemptRecord attempt;
  attempt.attempt = AttemptId{42};
  attempt.incarnation = IncarnationId{7};
  attempt.epoch = FabricEpoch{9};
  attempt.flow = FlowId{11};
  attempt.phase = AttemptPhase::kFenced;
  attempt.code = StatusCode::kFencedStaleEpoch;
  attempt.request_digest = Digest{1, 2};
  attempt.sequence = 5;
  attempt.observed_unix_nanos = -5;
  ByteWriter attempt_writer;
  EncodeAttemptRecord(attempt, attempt_writer);
  ByteReader attempt_reader(attempt_writer.data());
  AttemptRecord decoded_attempt;
  FP_REQUIRE(DecodeAttemptRecord(attempt_reader, &decoded_attempt).ok());
  FP_CHECK_EQ(decoded_attempt.attempt, attempt.attempt);
  FP_CHECK_EQ(decoded_attempt.phase, AttemptPhase::kFenced);
  FP_CHECK_EQ(decoded_attempt.code, StatusCode::kFencedStaleEpoch);
  FP_CHECK(decoded_attempt.request_digest == attempt.request_digest);

  const PlacementRequest request = Baseline(2);
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_REQUIRE(decision.intent.has_value());
  PlacementRecord record;
  record.id = PlacementId{3};
  record.generation = PlacementGeneration{2};
  record.intent = *decision.intent;
  record.delta = IncumbentDelta::kKept;
  record.decision_digest = decision.digest;
  record.committed_by = IncarnationId{8};
  record.commit_epoch = FabricEpoch{4};
  record.sequence = 6;
  record.supersedes_generation = 1;
  record.commit_unix_nanos = 99;
  ByteWriter record_writer;
  EncodePlacementRecord(record, record_writer);
  ByteReader record_reader(record_writer.data());
  PlacementRecord decoded_record;
  FP_REQUIRE(DecodePlacementRecord(record_reader, &decoded_record).ok());
  FP_CHECK_EQ(decoded_record.id, record.id);
  FP_CHECK_EQ(decoded_record.generation, record.generation);
  FP_CHECK_EQ(decoded_record.supersedes_generation, std::uint64_t{1});
  FP_CHECK_EQ(decoded_record.commit_epoch, record.commit_epoch);
  FP_CHECK(decoded_record.intent == record.intent);
  FP_CHECK(decoded_record.requires_revalidation);
}

FP_TEST(codec_wire, recovery_report_round_trips) {
  RecoveryReport report;
  report.truncated_tail = true;
  report.bytes_discarded = 12;
  report.records_valid = 3;
  report.placements_total = 2;
  report.attempts_total = 4;
  report.unfinished_attempts = 1;
  report.max_sequence = 9;
  report.last_epoch = FabricEpoch{5};
  report.last_incarnation = IncarnationId{6};
  report.detail = "unfinished attempts require re-driving";
  AttemptRecord attempt;
  attempt.attempt = AttemptId{1};
  attempt.epoch = FabricEpoch{5};
  attempt.phase = AttemptPhase::kStarted;
  report.unfinished.push_back(attempt);
  report.liveness_restored = false;

  ByteWriter writer;
  EncodeRecoveryReport(report, writer);
  ByteReader reader(writer.data());
  RecoveryReport decoded;
  FP_REQUIRE(DecodeRecoveryReport(reader, &decoded).ok());
  FP_CHECK_EQ(decoded.records_valid, report.records_valid);
  FP_CHECK_EQ(decoded.unfinished.size(), std::size_t{1});
  FP_CHECK_EQ(decoded.last_epoch, report.last_epoch);
  FP_CHECK(!decoded.liveness_restored);
  FP_CHECK_EQ(decoded.detail, report.detail);
}

FP_TEST(codec_wire, hello_payloads_round_trip_and_reject_bad_bounds) {
  HelloPayload hello;
  hello.protocol_version = kWireProtocolVersion;
  hello.incarnation = IncarnationId{77};
  hello.epoch = FabricEpoch{3};
  hello.worker = WorkerId{9};
  hello.max_frame_bytes = 65536;
  hello.build = "flowplace 1.0.0";
  ByteWriter writer;
  EncodeHello(hello, writer);
  ByteReader reader(writer.data());
  HelloPayload decoded;
  FP_REQUIRE(DecodeHello(reader, &decoded).ok());
  FP_CHECK_EQ(decoded.worker, hello.worker);
  FP_CHECK_EQ(decoded.build, hello.build);

  HelloPayload too_small = hello;
  too_small.max_frame_bytes = 1;
  ByteWriter small_writer;
  EncodeHello(too_small, small_writer);
  ByteReader small_reader(small_writer.data());
  HelloPayload small_decoded;
  FP_CHECK(!DecodeHello(small_reader, &small_decoded).ok());

  HelloAckPayload ack;
  ack.protocol_version = kWireProtocolVersion;
  ack.coordinator = IncarnationId{5};
  ack.epoch = FabricEpoch{2};
  ack.max_frame_bytes = 4096;
  ack.worker_limit = 16;
  ack.build = "flowplace 1.0.0";
  ack.accepted = false;
  ack.code = StatusCode::kProtocolMismatch;
  ack.message = "wire protocol version mismatch";
  ByteWriter ack_writer;
  EncodeHelloAck(ack, ack_writer);
  ByteReader ack_reader(ack_writer.data());
  HelloAckPayload ack_decoded;
  FP_REQUIRE(DecodeHelloAck(ack_reader, &ack_decoded).ok());
  FP_CHECK(!ack_decoded.accepted);
  FP_CHECK_EQ(ack_decoded.code, StatusCode::kProtocolMismatch);
}

FP_TEST(codec_wire, place_request_and_result_round_trip) {
  const PlacementRequest request = Baseline(2);
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_REQUIRE(decision.intent.has_value());

  PlaceRequestPayload request_payload;
  request_payload.attempt = AttemptId{555};
  request_payload.epoch = FabricEpoch{kEpoch};
  request_payload.request = request;
  ByteWriter request_writer;
  EncodePlaceRequest(request_payload, request_writer);
  ByteReader request_reader(request_writer.data());
  PlaceRequestPayload decoded_request;
  FP_REQUIRE(DecodePlaceRequest(request_reader, Limits{}, &decoded_request).ok());
  FP_CHECK_EQ(decoded_request.attempt, request_payload.attempt);
  FP_CHECK_EQ(decoded_request.epoch, request_payload.epoch);
  FP_CHECK_EQ(RequestDigest(decoded_request.request, Limits{}), RequestDigest(request, Limits{}));

  PlaceResultPayload result_payload;
  result_payload.attempt = AttemptId{555};
  result_payload.state = AttemptState::kCommitted;
  result_payload.code = StatusCode::kOk;
  result_payload.placement_id = PlacementId{12};
  result_payload.placement_generation = PlacementGeneration{2};
  result_payload.decision = decision;
  ByteWriter result_writer;
  EncodePlaceResult(result_payload, result_writer);
  ByteReader result_reader(result_writer.data());
  PlaceResultPayload decoded_result;
  FP_REQUIRE(DecodePlaceResult(result_reader, Limits{}, &decoded_result).ok());
  FP_CHECK_EQ(decoded_result.placement_id, result_payload.placement_id);
  FP_CHECK_EQ(decoded_result.state, AttemptState::kCommitted);
  FP_CHECK_EQ(DecisionDigest(decoded_result.decision), decision.digest);
}

FP_TEST(codec_wire, epoch_and_error_payloads_round_trip) {
  EpochPayload epoch;
  epoch.epoch = FabricEpoch{4};
  epoch.previous_epoch = FabricEpoch{3};
  epoch.code = StatusCode::kOk;
  epoch.message = "epoch updated";
  ByteWriter epoch_writer;
  EncodeEpochPayload(epoch, epoch_writer);
  ByteReader epoch_reader(epoch_writer.data());
  EpochPayload decoded_epoch;
  FP_REQUIRE(DecodeEpochPayload(epoch_reader, &decoded_epoch).ok());
  FP_CHECK_EQ(decoded_epoch.epoch, epoch.epoch);
  FP_CHECK_EQ(decoded_epoch.previous_epoch, epoch.previous_epoch);

  ErrorPayload error;
  error.code = StatusCode::kFencedStaleEpoch;
  error.attempt = AttemptId{88};
  error.message = "request is bound to a superseded fabric epoch";
  ByteWriter error_writer;
  EncodeErrorPayload(error, error_writer);
  ByteReader error_reader(error_writer.data());
  ErrorPayload decoded_error;
  FP_REQUIRE(DecodeErrorPayload(error_reader, &decoded_error).ok());
  FP_CHECK_EQ(decoded_error.code, error.code);
  FP_CHECK_EQ(decoded_error.attempt, error.attempt);
  FP_CHECK_EQ(decoded_error.message, error.message);
}

FP_TEST(codec_wire, frame_encoding_is_stable) {
  Frame frame;
  frame.kind = FrameKind::kPlaceResult;
  frame.flags = 1;
  frame.sequence = 42;
  frame.body = {1, 2, 3, 4, 5, 6, 7};
  std::vector<std::uint8_t> first;
  std::vector<std::uint8_t> second;
  FP_REQUIRE(EncodeFrame(frame, &first).ok());
  FP_REQUIRE(EncodeFrame(frame, &second).ok());
  FP_CHECK(first == second);
  FP_CHECK_EQ(first.size(), kFrameHeaderBytes + frame.body.size());
  Frame decoded;
  FP_REQUIRE(DecodeFrame(first.data(), first.size(), &decoded).ok());
  FP_CHECK_EQ(decoded.flags, std::uint16_t{1});
  FP_CHECK_EQ(decoded.kind, FrameKind::kPlaceResult);

  Frame oversized;
  oversized.kind = FrameKind::kHello;
  oversized.body.assign(2048, 0);
  std::vector<std::uint8_t> out;
  FP_CHECK_EQ(EncodeFrame(oversized, &out, 1024).code(), StatusCode::kFrameTooLarge);
  FP_CHECK_EQ(EncodeFrame(frame, &out, 64).code(), StatusCode::kInvalidArgument);
}

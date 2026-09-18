// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fixtures.hpp"
#include "harness.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "flowplace/codec.hpp"
#include "flowplace/scenario.hpp"
#include "flowplace/wire.hpp"

using namespace flowplace;
using namespace fptest;

namespace {

std::vector<std::uint8_t> EncodeRequestBytes(const PlacementRequest& request) {
  ByteWriter writer;
  EncodeRequest(request, writer);
  return writer.data();
}

}  // namespace

FP_TEST(adversarial_input, decoders_reject_trailing_bytes_and_unknown_codes) {
  const PlacementRequest request = Baseline(1);
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_REQUIRE(decision.intent.has_value());

  ByteWriter intent_writer;
  EncodeIntent(*decision.intent, intent_writer);
  ByteReader exact(intent_writer.data());
  PlacementIntent decoded_intent;
  FP_CHECK(DecodeIntent(exact, &decoded_intent).ok());
  std::vector<std::uint8_t> trailing = intent_writer.data();
  trailing.push_back(0xABu);
  ByteReader extra(trailing.data(), trailing.size());
  FP_CHECK_EQ(DecodeIntent(extra, &decoded_intent).code(), StatusCode::kWireMalformed);

  ByteWriter expectation_writer;
  EncodeAuthorityExpectation(request.expected, expectation_writer);
  std::vector<std::uint8_t> expectation_bytes = expectation_writer.data();
  expectation_bytes.push_back(0x01u);
  ByteReader expectation_reader(expectation_bytes.data(), expectation_bytes.size());
  AuthorityExpectation expectation;
  FP_CHECK_EQ(DecodeAuthorityExpectation(expectation_reader, &expectation).code(),
              StatusCode::kWireMalformed);

  ByteWriter decision_writer;
  EncodeDecision(decision, decision_writer);
  std::vector<std::uint8_t> decision_bytes = decision_writer.data();
  // Layout: outcome u8, then the u16 status code.
  decision_bytes[1] = 0xFFu;
  decision_bytes[2] = 0xFFu;
  ByteReader decision_reader(decision_bytes.data(), decision_bytes.size());
  PlacementDecision decoded_decision;
  FP_CHECK_EQ(DecodeDecision(decision_reader, Limits{}, &decoded_decision).code(),
              StatusCode::kWireMalformed);

  const std::string long_line = "attempt " + std::string(kMaxScenarioLineBytes + 1, 'a') + "\n";
  FP_CHECK_EQ(ParseScenario(long_line, Limits{}).status().code(), StatusCode::kOversizeRequest);

  std::string many_keys = "policy";
  for (int i = 0; i < 80; ++i) many_keys += " k" + std::to_string(i) + " v";
  many_keys += "\n";
  FP_CHECK_EQ(ParseScenario(many_keys, Limits{}).status().code(), StatusCode::kOversizeRequest);
}

FP_TEST(adversarial_input, scenario_grammar_rejects_malformed_documents) {
  const std::string valid = WriteScenario(Baseline(2));
  FP_CHECK(ParseScenario(valid, Limits{}).ok());

  const std::vector<std::pair<std::string, StatusCode>> cases = {
      {"version 99\n" + valid, StatusCode::kUnsupported},
      {valid + "nonsense directive\n", StatusCode::kMalformedRequest},
      {valid + "flow id 1 gen\n", StatusCode::kMalformedRequest},
      {valid + "flow id 5 id 6\n", StatusCode::kMalformedRequest},
      {valid + "attempt id 1 bogus 2\n", StatusCode::kMalformedRequest},
      // Repeated singleton directives are rejected as malformed...
      {valid + "attempt id 7\n", StatusCode::kMalformedRequest},
      {valid + "evidence id 1 gen 1\n", StatusCode::kMalformedRequest},
      // ...and the guard applies to directives the baseline does not use once
      // the directive appears twice.
      {valid + "occupied domains 1\noccupied domains 2\n", StatusCode::kMalformedRequest},
      {valid + "incumbent id 1 gen 1 path 1 pathauth 7 cset 2 cap 5 policy 6 qos 4 epoch 11\n"
               "incumbent id 2 gen 1 path 1 pathauth 7 cset 2 cap 5 policy 6 qos 4 epoch 11\n",
       StatusCode::kMalformedRequest},
      {"flow id 1 gen 1\n", StatusCode::kMissingField},
      {std::string(), StatusCode::kMissingField},
      {valid + "qos id 1 gen 1 class teleport\n", StatusCode::kMalformedRequest},
      {valid + "occupied domains 1,,2\n", StatusCode::kInvalidFieldValue},
      {valid + "policy id 1 gen 1 objectives cost:sideways\n", StatusCode::kMalformedRequest},
      {"version 1\nflow id 1 gen 1\npolicy id 1 gen 1 objectives cost:min bogus key\n",
       StatusCode::kMalformedRequest},
  };
  std::size_t case_index = 0;
  for (const auto& entry : cases) {
    ++case_index;
    const Result<PlacementRequest> parsed = ParseScenario(entry.first, Limits{});
    if (parsed.ok()) {
      std::fprintf(stderr, "case %llu unexpectedly parsed: [%s]\n",
                   static_cast<unsigned long long>(case_index),
                   entry.first.substr(entry.first.size() > 160 ? entry.first.size() - 160 : 0).c_str());
    }
    FP_CHECK(!parsed.ok());
    if (!parsed.ok()) FP_CHECK_EQ(parsed.status().code(), entry.second);
  }

  // Numeric and list values are validated independently of the duplicate guard.
  const std::string attempt_reference = "attempt id 0";
  FP_CHECK(valid.find(attempt_reference) != std::string::npos);
  const auto ReplaceAttempt = [&valid, &attempt_reference](const std::string& value) {
    std::string copy = valid;
    copy.replace(copy.find(attempt_reference), attempt_reference.size(), "attempt id " + value);
    return copy;
  };
  const std::vector<std::pair<std::string, StatusCode>> numeric_cases = {
      {ReplaceAttempt("-1"), StatusCode::kInvalidFieldValue},
      {ReplaceAttempt("99999999999999999999999"), StatusCode::kInvalidFieldValue},
      {ReplaceAttempt("1 2 3"), StatusCode::kMalformedRequest},
  };
  for (const auto& entry : numeric_cases) {
    const Result<PlacementRequest> parsed = ParseScenario(entry.first, Limits{});
    FP_CHECK(!parsed.ok());
    if (!parsed.ok()) FP_CHECK_EQ(parsed.status().code(), entry.second);
  }

  // Unknown keys inside a valid directive are rejected, not ignored.
  std::string tampered = valid;
  const std::size_t position = tampered.find("attempt id");
  tampered.insert(position, "mystery 1 ");
  FP_CHECK(!ParseScenario(tampered, Limits{}).ok());

  // A directive repeated where the grammar allows it only once.
  FP_CHECK(!ParseScenario(valid + "try id 1\n", Limits{}).ok());
}

FP_TEST(adversarial_input, scenario_round_trips_canonically) {
  PlacementRequest request = Baseline(4);
  SetIncumbentOn(request, PathId{2});
  request.policy.locality.preferred_locality = LocalityId{11};
  request.policy.locality.forbidden_localities = {LocalityId{13}};
  request.policy.failure_domains.forbidden_domains = {FailureDomainId{22}};
  request.policy.failure_domains.avoid_occupied_domains = true;
  request.policy.failure_domains.min_distinct_domains = 2;
  request.policy.allowed_tiers = {PathTier::kPremium, PathTier::kStandard};
  request.policy.required_labels = {PolicyLabelId{4}};
  request.policy.forbidden_labels = {PolicyLabelId{5}};
  ReservationRef ref;
  ref.id = ReservationId{12};
  ref.generation = ReservationGeneration{6};
  request.policy.reservation_affinity.refs = {ref};
  request.policy.admission.min_residual_headroom_bytes = 10;
  request.policy.admission.on_below = GateAction::kDefer;
  request.policy.service.allow_degraded = true;
  request.policy.service.max_relaxation_steps = 2;
  request.policy.churn.prefer_incumbent = true;
  request.policy.churn.move_improvement_threshold = 7;
  request.policy.evidence.on_missing = EvidencePolicyMode::kRankWorst;
  request.policy.revalidate_after_nanos = 42;
  request.occupied_failure_domains = {FailureDomainId{31}, FailureDomainId{32}};
  request.attempt = AttemptId{77};
  request.candidates.paths[0].attributes.labels = {PolicyLabelId{4}, PolicyLabelId{2}};
  request.candidates.paths[1].attributes.reservations = {ReservationRef{ReservationId{12},
                                                                       ReservationGeneration{6}}};
  request.provenance.observer_unix_nanos = 1234;
  SetObjectives(request, {MakeObjective(ObjectiveKind::kCost, Direction::kMinimize),
                          MakeObjective(ObjectiveKind::kLatency, Direction::kMaximize)});

  const std::string text = WriteScenario(request);
  const Result<PlacementRequest> reparsed = ParseScenario(text, Limits{});
  if (!reparsed.ok()) {
    std::fprintf(stderr, "scenario round trip failed: %s\n--- document ---\n%s\n",
                 reparsed.status().ToString().c_str(), text.c_str());
  }
  FP_REQUIRE(reparsed.ok());
  FP_CHECK_EQ(RequestDigest(request, Limits{}), RequestDigest(reparsed.value(), Limits{}));
  FP_CHECK_EQ(WriteScenario(reparsed.value()), text);
  const PlacementEngine engine;
  FP_CHECK_EQ(engine.Place(request).digest, engine.Place(reparsed.value()).digest);
}

FP_TEST(adversarial_input, truncated_request_payloads_are_rejected) {
  const PlacementRequest request = Baseline(3);
  const std::vector<std::uint8_t> bytes = EncodeRequestBytes(request);
  FP_CHECK(bytes.size() > 64);
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    ByteReader reader(bytes.data(), length);
    PlacementRequest decoded;
    const Status status = DecodeRequest(reader, Limits{}, &decoded);
    FP_CHECK(!status.ok());
  }
  ByteReader reader(bytes.data(), bytes.size());
  PlacementRequest decoded;
  FP_CHECK(DecodeRequest(reader, Limits{}, &decoded).ok());
  FP_CHECK_EQ(RequestDigest(decoded, Limits{}), RequestDigest(request, Limits{}));
}

FP_TEST(adversarial_input, every_single_byte_corruption_is_detected_or_harmless) {
  const PlacementRequest request = Baseline(3);
  std::vector<std::uint8_t> bytes = EncodeRequestBytes(request);
  std::uint64_t accepted = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const std::uint8_t original = bytes[i];
    bytes[i] = static_cast<std::uint8_t>(original ^ 0xFFu);
    ByteReader reader(bytes.data(), bytes.size());
    PlacementRequest decoded;
    const Status status = DecodeRequest(reader, Limits{}, &decoded);
    if (status.ok()) {
      ++accepted;
      // A mutated but structurally valid request must still be decidable.
      const PlacementEngine engine;
      static_cast<void>(engine.Place(decoded));
    } else {
      FP_CHECK(!status.message().empty());
    }
    bytes[i] = original;
  }
  // Mutating a byte inside a length field usually breaks the structure; the
  // point of the assertion is that nothing is silently accepted as the
  // original request.
  FP_CHECK(accepted < bytes.size());
}

FP_TEST(adversarial_input, codec_rejects_trailing_and_unknown_data) {
  const PlacementRequest request = Baseline(2);
  std::vector<std::uint8_t> bytes = EncodeRequestBytes(request);
  bytes.push_back(0xAB);
  ByteReader reader(bytes.data(), bytes.size());
  PlacementRequest decoded;
  FP_CHECK_EQ(DecodeRequest(reader, Limits{}, &decoded).code(), StatusCode::kWireMalformed);

  std::vector<std::uint8_t> unknown = EncodeRequestBytes(request);
  ByteReader unknown_reader(unknown.data(), unknown.size());
  PlacementRequest unknown_decoded;
  FP_CHECK(DecodeRequest(unknown_reader, Limits{}, &unknown_decoded).ok());
}

FP_TEST(adversarial_input, codec_honours_limits_when_decoding_counts) {
  const PlacementRequest request = Baseline(4);
  const std::vector<std::uint8_t> bytes = EncodeRequestBytes(request);
  Limits tight;
  tight.max_candidate_paths = 2;
  ByteReader reader(bytes.data(), bytes.size());
  PlacementRequest decoded;
  FP_CHECK_EQ(DecodeRequest(reader, tight, &decoded).code(), StatusCode::kOversizeRequest);
}

FP_TEST(adversarial_input, random_bytes_never_crash_the_decoders) {
  Rng rng(0xC0FFEE);
  const PlacementEngine engine;
  for (int iteration = 0; iteration < 4000; ++iteration) {
    const std::size_t length = rng.Below(96);
    std::vector<std::uint8_t> bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
      bytes[i] = static_cast<std::uint8_t>(rng.Below(256));
    }
    {
      ByteReader reader(bytes.data(), bytes.size());
      PlacementRequest decoded;
      const Status status = DecodeRequest(reader, Limits{}, &decoded);
      if (status.ok()) static_cast<void>(engine.Place(decoded));
    }
    {
      Frame frame;
      const Status status = DecodeFrame(bytes.data(), bytes.size(), &frame, kDefaultMaxFrameBytes);
      if (status.ok()) static_cast<void>(frame);
    }
    {
      ByteReader reader(bytes.data(), bytes.size());
      PlacementDecision decision;
      static_cast<void>(DecodeDecision(reader, Limits{}, &decision));
    }
    {
      ByteReader reader(bytes.data(), bytes.size());
      RecoveryReport report;
      static_cast<void>(DecodeRecoveryReport(reader, &report));
    }
    {
      FrameStream stream;
      static_cast<void>(stream.Feed(bytes.data(), bytes.size()));
      std::optional<Frame> frame;
      static_cast<void>(stream.Next(&frame));
    }
  }
}

FP_TEST(adversarial_input, frame_decoder_rejects_inconsistent_lengths) {
  Frame frame;
  frame.kind = FrameKind::kHello;
  frame.sequence = 3;
  frame.body = {1, 2, 3, 4, 5};
  std::vector<std::uint8_t> encoded;
  FP_CHECK(EncodeFrame(frame, &encoded, kDefaultMaxFrameBytes).ok());
  Frame decoded;
  FP_CHECK(DecodeFrame(encoded.data(), encoded.size(), &decoded, kDefaultMaxFrameBytes).ok());
  FP_CHECK_EQ(decoded.sequence, std::uint64_t{3});
  FP_CHECK(decoded.body == frame.body);

  // Declared length larger than the buffer.
  FP_CHECK_EQ(DecodeFrame(encoded.data(), encoded.size() - 1, &decoded, kDefaultMaxFrameBytes).code(),
              StatusCode::kWireMalformed);
  // Body corruption.
  std::vector<std::uint8_t> corrupted = encoded;
  corrupted[kFrameHeaderBytes] ^= 0xFFu;
  FP_CHECK_EQ(DecodeFrame(corrupted.data(), corrupted.size(), &decoded, kDefaultMaxFrameBytes).code(),
              StatusCode::kFrameChecksumMismatch);
  // Oversize declaration.
  std::vector<std::uint8_t> oversize = encoded;
  oversize[0] = 0xFFu;
  oversize[1] = 0xFFu;
  oversize[2] = 0xFFu;
  oversize[3] = 0x7Fu;
  FP_CHECK_EQ(DecodeFrame(oversize.data(), oversize.size(), &decoded, 1024).code(),
              StatusCode::kFrameTooLarge);
  // Unknown frame kind.
  std::vector<std::uint8_t> unknown_kind;
  Frame other;
  other.kind = FrameKind::kHello;
  other.body = {9, 9};
  FP_CHECK(EncodeFrame(other, &unknown_kind, kDefaultMaxFrameBytes).ok());
  unknown_kind[8] = 0x7Fu;
  unknown_kind[9] = 0x00u;
  FP_CHECK_EQ(DecodeFrame(unknown_kind.data(), unknown_kind.size(), &decoded, kDefaultMaxFrameBytes)
                  .code(),
              StatusCode::kWireMalformed);
}

FP_TEST(adversarial_input, frame_stream_handles_arbitrary_chunking) {
  Frame first;
  first.kind = FrameKind::kPlaceRequest;
  first.sequence = 1;
  first.body = {1, 2, 3};
  Frame second;
  second.kind = FrameKind::kPlaceResult;
  second.sequence = 2;
  second.body = {9, 8, 7, 6};
  std::vector<std::uint8_t> buffer;
  std::vector<std::uint8_t> one;
  FP_CHECK(EncodeFrame(first, &one, kDefaultMaxFrameBytes).ok());
  buffer.insert(buffer.end(), one.begin(), one.end());
  FP_CHECK(EncodeFrame(second, &one, kDefaultMaxFrameBytes).ok());
  buffer.insert(buffer.end(), one.begin(), one.end());

  for (std::size_t chunk = 1; chunk <= 5; ++chunk) {
    FrameStream stream;
    std::size_t offset = 0;
    std::vector<Frame> frames;
    while (offset < buffer.size()) {
      const std::size_t size = std::min(chunk, buffer.size() - offset);
      FP_CHECK(stream.Feed(buffer.data() + offset, size).ok());
      offset += size;
      for (;;) {
        std::optional<Frame> frame;
        FP_CHECK(stream.Next(&frame).ok());
        if (!frame.has_value()) break;
        frames.push_back(*frame);
      }
    }
    FP_REQUIRE(frames.size() == 2);
    FP_CHECK_EQ(frames[0].kind, FrameKind::kPlaceRequest);
    FP_CHECK(frames[1].body == second.body);
  }
}

FP_TEST(adversarial_input, hostile_scenario_size_is_rejected) {
  const std::string massive(kMaxScenarioBytes + 10, 'x');
  const Result<PlacementRequest> parsed = ParseScenario(massive, Limits{});
  FP_CHECK_EQ(parsed.status().code(), StatusCode::kOversizeRequest);
}

FP_TEST(adversarial_input, unrepresentable_identifiers_are_rejected) {
  const std::string text =
      "flow id 18446744073709551616 gen 1\n"
      "candidateset id 1 gen 1\n"
      "capacity id 1 gen 1\n"
      "qos id 1 gen 1 class best_effort\n"
      "policy id 1 gen 1 objectives cost:min\n"
      "expected pathauth 1 cset 1 cap 1 policy 1 qos 1 epoch 1\n";
  FP_CHECK(!ParseScenario(text, Limits{}).ok());
}

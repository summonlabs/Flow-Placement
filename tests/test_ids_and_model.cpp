// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fixtures.hpp"
#include "harness.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "flowplace/checked.hpp"
#include "flowplace/hash.hpp"

using namespace flowplace;
using namespace fptest;

FP_TEST(ids_and_model, typed_ids_are_distinct_types) {
  const FlowId flow{7};
  const PathId path{7};
  FP_CHECK_EQ(flow.value(), path.value());
  FP_CHECK(flow.valid());
  FP_CHECK(!FlowId{}.valid());
  FP_CHECK_EQ(flow.ToString(), std::string("7"));
}

FP_TEST(ids_and_model, generation_advance_and_overflow) {
  FlowGeneration generation{1};
  FP_CHECK(generation.Advance());
  FP_CHECK_EQ(generation.value(), std::uint64_t{2});
  FP_CHECK(generation.IsNewerThan(FlowGeneration{1}));
  FlowGeneration maximum{0xFFFFFFFFFFFFFFFFull};
  FP_CHECK(!maximum.Advance());
  FP_CHECK_EQ(maximum.value(), 0xFFFFFFFFFFFFFFFFull);
  FP_CHECK_EQ(maximum.Next().value(), 0xFFFFFFFFFFFFFFFFull);
}

FP_TEST(ids_and_model, crc32c_known_vector) {
  // CRC-32C of "123456789" is 0xE3069283.
  FP_CHECK_EQ(Crc32C::Compute("123456789"), 0xE3069283u);
  FP_CHECK_EQ(Crc32C::Compute(""), 0u);
}

FP_TEST(ids_and_model, digest_is_order_insensitive_for_unordered_collections) {
  PlacementRequest first = Baseline(3);
  PlacementRequest second = Baseline(3);
  std::reverse(second.candidates.paths.begin(), second.candidates.paths.end());
  std::reverse(second.capacity.entries.begin(), second.capacity.entries.end());
  std::reverse(second.evidence.entries.begin(), second.evidence.entries.end());
  first.candidates.paths[0].attributes.labels = {PolicyLabelId{3}, PolicyLabelId{1}};
  for (CandidatePath& path : second.candidates.paths) {
    if (path.id == PathId{1}) path.attributes.labels = {PolicyLabelId{1}, PolicyLabelId{3}};
  }
  FP_CHECK(CandidateSetDigest(first.candidates) == CandidateSetDigest(second.candidates));
  FP_CHECK(CapacitySnapshotDigest(first.capacity) == CapacitySnapshotDigest(second.capacity));
  FP_CHECK(EvidenceDigest(first.evidence) == EvidenceDigest(second.evidence));
  FP_CHECK(RequestDigest(first, Limits{}) == RequestDigest(second, Limits{}));
  second.candidates.paths[0].attributes.labels.push_back(PolicyLabelId{9});
  FP_CHECK(CandidateSetDigest(first.candidates) != CandidateSetDigest(second.candidates));
}

FP_TEST(ids_and_model, digests_are_stable_and_distinguishing) {
  const PlacementRequest request = Baseline(3);
  FP_CHECK(RequestDigest(request, Limits{}) == RequestDigest(request, Limits{}));
  PlacementRequest changed = request;
  changed.candidates.paths[0].attributes.cost_micro += 1;
  FP_CHECK(RequestDigest(request, Limits{}) != RequestDigest(changed, Limits{}));
  // The informational observation timestamp does not participate.
  PlacementRequest observed = request;
  observed.provenance.observer_unix_nanos = 123456789;
  FP_CHECK(RequestDigest(request, Limits{}) == RequestDigest(observed, Limits{}));
}

FP_TEST(ids_and_model, enum_names_and_parsers_round_trip) {
  for (int i = 0; i <= 3; ++i) {
    const ServiceClass value = static_cast<ServiceClass>(i);
    FP_CHECK(ParseServiceClass(ServiceClassName(value)).has_value());
    FP_CHECK_EQ(*ParseServiceClass(ServiceClassName(value)), value);
  }
  for (int i = 0; i <= 8; ++i) {
    const ObjectiveKind value = static_cast<ObjectiveKind>(i);
    FP_CHECK(ParseObjectiveKind(ObjectiveKindName(value)).has_value());
    FP_CHECK_EQ(*ParseObjectiveKind(ObjectiveKindName(value)), value);
  }
  for (int i = 0; i <= 2; ++i) {
    const PathTier value = static_cast<PathTier>(i);
    FP_CHECK_EQ(*ParsePathTier(PathTierName(value)), value);
  }
  for (int i = 0; i <= 4; ++i) {
    const LocalityScope value = static_cast<LocalityScope>(i);
    FP_CHECK_EQ(*ParseLocalityScope(LocalityScopeName(value)), value);
  }
  FP_CHECK(!ParseObjectiveKind("nonsense").has_value());
  FP_CHECK_EQ(std::string(StatusCodeName(StatusCode::kStaleFabricEpoch)),
              std::string("STALE_FABRIC_EPOCH"));
  FP_CHECK_EQ(std::string(StatusCodeCategory(StatusCode::kStaleFabricEpoch)),
              std::string("staleness"));
}

FP_TEST(ids_and_model, authority_vector_rendering_is_complete) {
  const PlacementRequest request = Baseline(1);
  const AuthorityVector authority = [&request]() {
    AuthorityVector value;
    value.path_authority = request.expected.path_authority;
    value.candidate_set = request.candidates.id;
    value.candidate_set_generation = request.candidates.generation;
    value.capacity = request.capacity.id;
    value.capacity_generation = request.capacity.generation;
    value.policy = request.policy.id;
    value.policy_generation = request.policy.generation;
    value.qos = request.qos.id;
    value.qos_generation = request.qos.generation;
    value.evidence = request.evidence.id;
    value.evidence_generation = request.evidence.generation;
    value.fabric_epoch = request.expected.fabric_epoch;
    return value;
  }();
  const std::string text = authority.ToString();
  FP_CHECK(text.find("pathauth=7") != std::string::npos);
  FP_CHECK(text.find("cset=200/2") != std::string::npos);
  FP_CHECK(text.find("epoch=11") != std::string::npos);
  FP_CHECK(!AuthorityVectorDigest(authority).IsZero());
}

FP_TEST(ids_and_model, outcome_and_exclusion_names_are_total) {
  for (int i = 0; i <= 7; ++i) {
    const std::string name(OutcomeName(static_cast<Outcome>(i)));
    FP_CHECK(name != "UNKNOWN_OUTCOME");
  }
  for (int i = 0; i <= 17; ++i) {
    const std::string name(ExclusionReasonName(static_cast<ExclusionReason>(i)));
    FP_CHECK(name != "unknown_exclusion");
  }
  FP_CHECK(IsCapacityReason(ExclusionReason::kCapacityInsufficient));
  FP_CHECK(IsServiceReason(ExclusionReason::kServiceReservationUnmet));
  FP_CHECK(!IsPolicyReason(ExclusionReason::kServiceReservationUnmet));
  FP_CHECK(IsStalenessReason(ExclusionReason::kPathAuthorityStale));
  FP_CHECK(IsPolicyReason(ExclusionReason::kTierNotAllowed));
  FP_CHECK(IsQosReason(ExclusionReason::kLatencyBudgetExceeded));
  FP_CHECK(IsDegradingFlag(DegradationFlag::kServiceRelaxed));
  FP_CHECK(!IsDegradingFlag(DegradationFlag::kChurnSuppressed));
}

namespace {

// Runtime (non-constant-folded) extremes so that the checks below exercise the
// arithmetic instead of the compiler's constant folder.
std::uint64_t MaxU64() { return 0xFFFFFFFFFFFFFFFFull; }
std::uint64_t OverU32() { return 0x1FFFFFFFFull; }

}  // namespace

FP_TEST(ids_and_model, checked_arithmetic_reports_overflow) {
  const std::uint64_t maximum = MaxU64();
  FP_CHECK(CheckedAdd(1, 2).has_value());
  FP_CHECK(!CheckedAdd(maximum, 1).has_value());
  FP_CHECK(!CheckedMul(maximum, 2).has_value());
  FP_CHECK_EQ(*CheckedMul(0, maximum), std::uint64_t{0});
  FP_CHECK_EQ(SaturatingAdd(maximum, 5), maximum);
  FP_CHECK(!NarrowU32(OverU32()).has_value());
  FP_CHECK_EQ(*NarrowU32(7), 7u);
}

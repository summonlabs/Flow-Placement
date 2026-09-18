// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fixtures.hpp"
#include "harness.hpp"

#include <string>

using namespace flowplace;
using namespace fptest;

namespace {

bool HasDegradation(const PlacementDecision& decision, DegradationFlag flag) {
  for (const DegradationFlag value : decision.explanation.degradations) {
    if (value == flag) return true;
  }
  return false;
}

}  // namespace

namespace {

std::uint64_t CountExclusions(const PlacementDecision& decision, ExclusionReason reason) {
  for (const ExclusionSummary& summary : decision.explanation.exclusions) {
    if (summary.reason == reason) return summary.count;
  }
  return 0;
}

bool HasExclusionReason(const PlacementDecision& decision, ExclusionReason reason) {
  for (const ExclusionSummary& summary : decision.explanation.exclusions) {
    if (summary.reason == reason) return true;
  }
  return false;
}

}  // namespace

FP_TEST(engine_staleness, evidence_entries_without_a_generation_are_not_authority) {
  // Entries that arrive without a bundle identity are not a bundle: they must
  // never be ranked as a known utilization, even though the vector is populated.
  PlacementRequest request = Baseline(2);
  SetObjectives(request,
                {MakeObjective(ObjectiveKind::kCongestionUtilization, Direction::kMinimize)});
  request.evidence.id = EvidenceId{};
  request.evidence.generation = EvidenceGeneration{};
  FP_CHECK_EQ(request.evidence.entries.size(), std::size_t{2});
  request.policy.evidence.on_missing = EvidencePolicyMode::kHardExclude;
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPolicyRejected);
  FP_CHECK_EQ(decision.code, StatusCode::kPolicyRejectedAllPaths);
  FP_CHECK_EQ(CountExclusions(decision, ExclusionReason::kEvidenceMissing), std::uint64_t{2});

  // With rank-worst the candidates stay legal but every key is UNKNOWN.
  request.policy.evidence.on_missing = EvidencePolicyMode::kRankWorst;
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlacedDegraded);
  FP_CHECK_EQ(decision.explanation.unknown_evidence_count, std::uint64_t{2});
  for (const RankedCandidate& ranked : decision.explanation.ranked) {
    FP_CHECK(!ranked.keys[0].known);
  }
}

FP_TEST(engine_staleness, ineligible_incumbent_on_the_same_path_is_not_reaffirmed) {
  // A stale incumbent is never reported as kept, even when its path happens to
  // be the best legal candidate.
  PlacementRequest request = Baseline(2);
  SetIncumbentOn(request, PathId{1});
  request.incumbent->capacity_generation = CapacitySnapshotGeneration{kCapacityGeneration - 1};
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{1});
  FP_CHECK_EQ(decision.delta, IncumbentDelta::kReplacedStale);
}

FP_TEST(engine_staleness, request_level_generations_must_match_expectations) {
  const PlacementEngine engine;
  {
    PlacementRequest request = Baseline(2);
    request.candidates.generation = CandidateSetGeneration{kCandidateSetGeneration + 1};
    const PlacementDecision decision = engine.Place(request);
    FP_CHECK_EQ(decision.outcome, Outcome::kStaleInput);
    FP_CHECK_EQ(decision.code, StatusCode::kStaleCandidateSet);
  }
  {
    PlacementRequest request = Baseline(2);
    request.capacity.generation = CapacitySnapshotGeneration{kCapacityGeneration + 1};
    FP_CHECK_EQ(engine.Place(request).code, StatusCode::kStaleCapacitySnapshot);
  }
  {
    PlacementRequest request = Baseline(2);
    request.policy.generation = PolicyGeneration{kPolicyGeneration + 1};
    FP_CHECK_EQ(engine.Place(request).code, StatusCode::kStalePolicy);
  }
  {
    PlacementRequest request = Baseline(2);
    request.qos.generation = QosGeneration{kQosGeneration + 1};
    FP_CHECK_EQ(engine.Place(request).code, StatusCode::kStaleQos);
  }
}

FP_TEST(engine_staleness, stale_path_authority_excludes_the_path) {
  PlacementRequest request = Baseline(3);
  request.candidates.paths[0].authority = PathAuthorityGeneration{kPathAuthority + 1};
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{2});
  FP_CHECK_EQ(decision.intent->path_authority, PathAuthorityGeneration{kPathAuthority});
  FP_REQUIRE(decision.explanation.exclusions.size() == 1);
  FP_CHECK_EQ(decision.explanation.exclusions[0].reason, ExclusionReason::kPathAuthorityStale);
}

FP_TEST(engine_staleness, every_path_stale_is_stale_input) {
  PlacementRequest request = Baseline(3);
  for (CandidatePath& path : request.candidates.paths) {
    path.authority = PathAuthorityGeneration{kPathAuthority + 1};
  }
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kStaleInput);
  FP_CHECK_EQ(decision.code, StatusCode::kStaleAuthorityGeneration);
  FP_CHECK(!decision.intent.has_value());
}

FP_TEST(engine_staleness, stale_incumbent_cannot_survive_a_generation_change) {
  PlacementRequest request = Baseline(3);
  SetIncumbentOn(request, PathId{3});
  request.incumbent->path_authority = PathAuthorityGeneration{kPathAuthority + 1};
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{1});
  FP_CHECK_EQ(decision.intent->path_authority, PathAuthorityGeneration{kPathAuthority});
  FP_CHECK_EQ(decision.delta, IncumbentDelta::kReplacedStale);
  FP_CHECK(HasDegradation(decision, DegradationFlag::kIncumbentStale));
  FP_CHECK(!HasDegradation(decision, DegradationFlag::kChurnSuppressed));
}

FP_TEST(engine_staleness, stale_incumbent_is_not_reaffirmed_when_nothing_is_legal) {
  PlacementRequest request = Baseline(2);
  SetIncumbentOn(request, PathId{1});
  request.incumbent->path_authority = PathAuthorityGeneration{kPathAuthority + 1};
  for (PathCapacity& capacity : request.capacity.entries) capacity.residual_bytes = 1;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kInsufficientCapacity);
  FP_CHECK(!decision.intent.has_value());
}

FP_TEST(engine_staleness, incumbent_from_an_older_capacity_or_policy_generation_is_stale) {
  const PlacementEngine engine;
  {
    PlacementRequest request = Baseline(3);
    SetIncumbentOn(request, PathId{3});
    request.incumbent->capacity_generation = CapacitySnapshotGeneration{kCapacityGeneration - 1};
    const PlacementDecision decision = engine.Place(request);
    FP_CHECK_EQ(decision.delta, IncumbentDelta::kReplacedStale);
  }
  {
    PlacementRequest request = Baseline(3);
    SetIncumbentOn(request, PathId{3});
    request.incumbent->fabric_epoch = FabricEpoch{kEpoch - 1};
    const PlacementDecision decision = engine.Place(request);
    FP_CHECK_EQ(decision.delta, IncumbentDelta::kReplacedStale);
  }
  {
    PlacementRequest request = Baseline(3);
    SetIncumbentOn(request, PathId{3});
    request.incumbent->policy_generation = PolicyGeneration{kPolicyGeneration - 1};
    FP_CHECK_EQ(engine.Place(request).delta, IncumbentDelta::kReplacedStale);
  }
}

FP_TEST(engine_staleness, incumbent_that_left_the_candidate_set_is_not_eligible) {
  PlacementRequest request = Baseline(3);
  SetIncumbentOn(request, PathId{99});
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_CHECK_EQ(decision.delta, IncumbentDelta::kReplacedStale);
}

FP_TEST(engine_staleness, stale_evidence_with_hard_exclusion_is_stale_input) {
  PlacementRequest request = Baseline(2);
  SetObjectives(request,
                {MakeObjective(ObjectiveKind::kCongestionUtilization, Direction::kMinimize)});
  request.evidence.generation = EvidenceGeneration{kEvidenceGeneration - 1};
  request.policy.evidence.on_stale = EvidencePolicyMode::kHardExclude;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kStaleInput);
  FP_CHECK_EQ(decision.code, StatusCode::kStaleEvidence);
  FP_CHECK(!decision.intent.has_value());
}

FP_TEST(engine_staleness, untrusted_evidence_is_never_used_as_authority) {
  PlacementRequest request = Baseline(2);
  SetObjectives(request,
                {MakeObjective(ObjectiveKind::kCongestionUtilization, Direction::kMinimize)});
  request.expected.evidence = EvidenceGeneration{0};  // no evidence generation is trusted
  request.policy.evidence.on_stale = EvidencePolicyMode::kRankWorst;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlacedDegraded);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK(!decision.intent->evidence_generation.valid());
  FP_CHECK(HasDegradation(decision, DegradationFlag::kEvidenceUnknown));
  FP_CHECK_EQ(decision.explanation.unknown_evidence_count, std::uint64_t{2});
  bool untrusted_note = false;
  for (const std::string& note : decision.explanation.notes) {
    if (note.find("not bound to the expected evidence generation") != std::string::npos) {
      untrusted_note = true;
    }
  }
  FP_CHECK(untrusted_note);
}

FP_TEST(engine_staleness, missing_evidence_ranks_worst_instead_of_assuming_headroom) {
  PlacementRequest request = Baseline(3);
  SetObjectives(request, {MakeObjective(ObjectiveKind::kCongestionUtilization, Direction::kMinimize),
                          MakeObjective(ObjectiveKind::kCost, Direction::kMinimize)});
  request.evidence.entries.erase(request.evidence.entries.begin());
  request.policy.evidence.on_missing = EvidencePolicyMode::kRankWorst;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{2});
  FP_CHECK_EQ(decision.explanation.unknown_evidence_count, std::uint64_t{1});
  FP_REQUIRE(decision.explanation.ranked.size() == 3);
  FP_CHECK(!decision.explanation.ranked.back().keys[0].known);
  FP_CHECK(decision.explanation.ranked.back().path == PathId{1});
  FP_CHECK(!HasDegradation(decision, DegradationFlag::kEvidenceUnknown));
}

FP_TEST(engine_staleness, missing_evidence_with_hard_exclusion_removes_the_candidate) {
  PlacementRequest request = Baseline(2);
  SetObjectives(request,
                {MakeObjective(ObjectiveKind::kCongestionUtilization, Direction::kMinimize)});
  request.evidence.entries.erase(request.evidence.entries.begin());
  request.policy.evidence.on_missing = EvidencePolicyMode::kHardExclude;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{2});
  FP_REQUIRE(decision.explanation.exclusions.size() == 1);
  FP_CHECK_EQ(decision.explanation.exclusions[0].reason, ExclusionReason::kEvidenceMissing);
}

FP_TEST(engine_staleness, evidence_mismatch_is_reported_even_when_unused) {
  PlacementRequest request = Baseline(2);
  request.evidence.generation = EvidenceGeneration{kEvidenceGeneration + 3};
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK(!decision.intent->evidence_generation.valid());
}

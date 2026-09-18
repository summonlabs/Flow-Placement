// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fixtures.hpp"
#include "harness.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace flowplace;
using namespace fptest;

namespace {

bool ChosenIsInCandidateSet(const PlacementRequest& request, const PlacementDecision& decision) {
  if (!decision.intent) return true;
  for (const CandidatePath& path : request.candidates.paths) {
    if (path.id == decision.intent->path) {
      return path.authority == decision.intent->path_authority;
    }
  }
  return false;
}

}  // namespace

FP_TEST(engine_basic, places_the_lowest_cost_path) {
  const PlacementRequest request = Baseline(3);
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_CHECK_EQ(decision.code, StatusCode::kOk);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{1});
  FP_CHECK_EQ(decision.intent->path_authority, PathAuthorityGeneration{kPathAuthority});
  FP_CHECK_EQ(decision.intent->flow, FlowId{kFlow});
  FP_CHECK_EQ(decision.intent->candidate_set_generation,
              CandidateSetGeneration{kCandidateSetGeneration});
  FP_CHECK_EQ(decision.intent->capacity_generation, CapacitySnapshotGeneration{kCapacityGeneration});
  FP_CHECK_EQ(decision.intent->policy_generation, PolicyGeneration{kPolicyGeneration});
  FP_CHECK_EQ(decision.intent->qos_generation, QosGeneration{kQosGeneration});
  FP_CHECK_EQ(decision.intent->fabric_epoch, FabricEpoch{kEpoch});
  FP_CHECK_EQ(decision.delta, IncumbentDelta::kNoIncumbent);
  FP_CHECK(decision.validation.ok());
  FP_REQUIRE(decision.explanation.ranked.size() == 3);
  FP_CHECK_EQ(decision.explanation.ranked[0].path, PathId{1});
  FP_CHECK_EQ(decision.explanation.ranked[1].path, PathId{2});
  FP_CHECK_EQ(decision.explanation.ranked[2].path, PathId{3});
  FP_CHECK_EQ(decision.explanation.ranked[0].rank, 0u);
  FP_CHECK_EQ(decision.explanation.legal_candidate_count, std::uint64_t{3});
  FP_CHECK_EQ(decision.explanation.excluded_candidate_count, std::uint64_t{0});
  FP_CHECK_EQ(decision.intent->digest, IntentDigest(*decision.intent));
  FP_CHECK_EQ(decision.digest, DecisionDigest(decision));
  std::string reason;
  FP_CHECK(engine.VerifyIntent(request, *decision.intent, &reason));
}

FP_TEST(engine_basic, decision_is_reproducible) {
  const PlacementRequest request = Baseline(4);
  const PlacementEngine engine;
  const PlacementDecision first = engine.Place(request);
  const PlacementDecision second = engine.Place(request);
  FP_CHECK_EQ(first.digest, second.digest);
  FP_CHECK_EQ(first.outcome, second.outcome);
  FP_CHECK(first.intent.has_value() && second.intent.has_value());
  FP_CHECK_EQ(first.intent->digest, second.intent->digest);
}

FP_TEST(engine_basic, chosen_path_is_always_a_member_of_the_authorized_set) {
  const PlacementEngine engine;
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    Rng rng(seed);
    PlacementRequest request = Baseline(1 + rng.Below(8));
    for (std::size_t i = 0; i < request.candidates.paths.size(); ++i) {
      request.candidates.paths[i].attributes.cost_micro = rng.Below(1000);
      request.capacity.entries[i].residual_bytes = 100 + rng.Below(10000);
      if (rng.Chance(30)) request.candidates.paths[i].attributes.tier = PathTier::kEconomy;
    }
    const PlacementDecision decision = engine.Place(request);
    FP_CHECK(ChosenIsInCandidateSet(request, decision));
    FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  }
}

FP_TEST(engine_basic, placement_does_not_depend_on_input_order) {
  const PlacementEngine engine;
  PlacementRequest request = Baseline(6);
  const PlacementDecision baseline = engine.Place(request);
  std::reverse(request.candidates.paths.begin(), request.candidates.paths.end());
  std::reverse(request.capacity.entries.begin(), request.capacity.entries.end());
  std::reverse(request.evidence.entries.begin(), request.evidence.entries.end());
  const PlacementDecision reordered = engine.Place(request);
  FP_CHECK_EQ(baseline.digest, reordered.digest);
  FP_REQUIRE(baseline.intent.has_value() && reordered.intent.has_value());
  FP_CHECK_EQ(baseline.intent->path, reordered.intent->path);
}

FP_TEST(engine_basic, ties_are_broken_by_path_id) {
  PlacementRequest request = Baseline(3);
  for (PathCapacity& capacity : request.capacity.entries) capacity.residual_bytes = 5000;
  for (CandidatePath& path : request.candidates.paths) path.attributes.cost_micro = 42;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{1});
}

FP_TEST(engine_basic, empty_candidate_set_is_no_legal_path) {
  PlacementRequest request = Baseline(0);
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kNoLegalPath);
  FP_CHECK_EQ(decision.code, StatusCode::kNotFound);
  FP_CHECK(!decision.intent.has_value());
  FP_CHECK_EQ(decision.explanation.candidate_count, std::uint64_t{0});
  FP_CHECK(!decision.explanation.notes.empty());
}

FP_TEST(engine_basic, unknown_capacity_is_not_treated_as_capacity) {
  PlacementRequest request = Baseline(3);
  request.capacity.entries.erase(request.capacity.entries.begin());
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{2});
  FP_REQUIRE(decision.explanation.exclusions.size() == 1);
  FP_CHECK_EQ(decision.explanation.exclusions[0].reason, ExclusionReason::kCapacityUnknown);
  FP_CHECK_EQ(decision.explanation.exclusions[0].count, std::uint64_t{1});
}

FP_TEST(engine_basic, absent_capacity_evidence_everywhere_is_not_placement) {
  PlacementRequest request = Baseline(2);
  request.capacity.entries.clear();
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kInsufficientCapacity);
  FP_CHECK(!decision.intent.has_value());
  FP_CHECK(decision.code == StatusCode::kNotFound);
}

FP_TEST(engine_basic, no_hidden_fallback_when_nothing_is_legal) {
  PlacementRequest request = Baseline(3);
  for (PathCapacity& capacity : request.capacity.entries) capacity.residual_bytes = 1;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kInsufficientCapacity);
  FP_CHECK_EQ(decision.code, StatusCode::kCapacityInsufficient);
  FP_CHECK(!decision.intent.has_value());
  FP_REQUIRE(decision.explanation.exclusions.size() == 1);
  FP_CHECK_EQ(decision.explanation.exclusions[0].reason, ExclusionReason::kCapacityInsufficient);
  FP_CHECK_EQ(decision.explanation.exclusions[0].count, std::uint64_t{3});
  FP_REQUIRE(!decision.explanation.binding.empty());
  FP_CHECK_EQ(decision.explanation.binding[0].required, std::uint64_t{100});
  FP_CHECK_EQ(decision.explanation.binding[0].available, std::uint64_t{1});
}

FP_TEST(engine_basic, explanation_is_bounded_for_large_fan_out) {
  PlacementRequest request = Baseline(20000);
  for (PathCapacity& capacity : request.capacity.entries) capacity.residual_bytes = 1;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kInsufficientCapacity);
  FP_CHECK(decision.explanation.ranked.size() <= 32);
  FP_CHECK(decision.explanation.exclusions.size() <= 64);
  for (const ExclusionSummary& summary : decision.explanation.exclusions) {
    FP_CHECK(summary.sample.size() <= 4);
    FP_CHECK_EQ(summary.count, std::uint64_t{20000});
  }
  FP_CHECK(decision.explanation.truncated);
}

FP_TEST(engine_basic, explanation_is_bounded_when_many_are_legal) {
  PlacementRequest request = Baseline(5000);
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_CHECK_EQ(decision.explanation.legal_candidate_count, std::uint64_t{5000});
  FP_CHECK(decision.explanation.ranked.size() <= 32);
  FP_CHECK(decision.explanation.truncated);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{1});
}

FP_TEST(engine_basic, decision_rendering_exposes_the_required_sections) {
  const PlacementRequest request = Baseline(2);
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  const std::string text = RenderDecision(decision);
  FP_CHECK(text.find("outcome=PLACED") != std::string::npos);
  FP_CHECK(text.find("authority:") != std::string::npos);
  FP_CHECK(text.find("ranking:") != std::string::npos);
  FP_CHECK(text.find("intent:") != std::string::npos);
  FP_CHECK(text.find("candidates: total=2 legal=2") != std::string::npos);
}

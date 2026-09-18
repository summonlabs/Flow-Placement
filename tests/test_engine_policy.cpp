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

FP_TEST(engine_policy, deferred_decision_still_reports_the_incumbent_relation) {
  // An outcome that produces no placement still states the incumbent relation.
  PlacementRequest request = Baseline(1, 1500);
  SetIncumbentOn(request, PathId{1});
  request.policy.admission.min_residual_headroom_bytes = 100000;
  request.policy.admission.on_below = GateAction::kDefer;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kDeferred);
  FP_CHECK_EQ(decision.delta, IncumbentDelta::kKept);

  PlacementRequest stale = Baseline(1, 1500);
  SetIncumbentOn(stale, PathId{1});
  stale.incumbent->capacity_generation = CapacitySnapshotGeneration{kCapacityGeneration - 1};
  stale.policy.admission.min_residual_headroom_bytes = 100000;
  stale.policy.admission.on_below = GateAction::kDefer;
  const PlacementDecision stale_decision = engine.Place(stale);
  FP_CHECK_EQ(stale_decision.outcome, Outcome::kDeferred);
  FP_CHECK_EQ(stale_decision.delta, IncumbentDelta::kReplacedStale);
}

FP_TEST(engine_policy, objective_order_decides_the_ranking) {
  PlacementRequest request = Baseline(3);
  request.candidates.paths[0].attributes.cost_micro = 10;
  request.candidates.paths[0].attributes.latency_nanos = 9000;
  request.candidates.paths[1].attributes.cost_micro = 20;
  request.candidates.paths[1].attributes.latency_nanos = 100;
  request.candidates.paths[2].attributes.cost_micro = 30;
  request.candidates.paths[2].attributes.latency_nanos = 5000;
  const PlacementEngine engine;
  SetObjectives(request, {MakeObjective(ObjectiveKind::kCost, Direction::kMinimize)});
  FP_CHECK_EQ(engine.Place(request).intent->path, PathId{1});
  SetObjectives(request, {MakeObjective(ObjectiveKind::kLatency, Direction::kMinimize)});
  FP_CHECK_EQ(engine.Place(request).intent->path, PathId{2});
  SetObjectives(request, {MakeObjective(ObjectiveKind::kCost, Direction::kMaximize)});
  FP_CHECK_EQ(engine.Place(request).intent->path, PathId{3});
  SetObjectives(request, {MakeObjective(ObjectiveKind::kLatency, Direction::kMinimize),
                          MakeObjective(ObjectiveKind::kCost, Direction::kMinimize)});
  FP_CHECK_EQ(engine.Place(request).intent->path, PathId{2});
}

FP_TEST(engine_policy, residual_capacity_objective_uses_capacity_evidence) {
  PlacementRequest request = Baseline(3);
  request.capacity.entries[0].residual_bytes = 900;
  request.capacity.entries[0].capacity_bytes = 1000;
  request.capacity.entries[1].residual_bytes = 5000;
  request.capacity.entries[2].residual_bytes = 700;
  request.capacity.entries[2].capacity_bytes = 1000;
  SetObjectives(request, {MakeObjective(ObjectiveKind::kResidualCapacity, Direction::kMaximize)});
  const PlacementEngine engine;
  FP_CHECK_EQ(engine.Place(request).intent->path, PathId{2});
}

FP_TEST(engine_policy, incumbent_stability_objective_prefers_the_incumbent) {
  PlacementRequest request = Baseline(3);
  SetIncumbentOn(request, PathId{3});
  SetObjectives(request, {MakeObjective(ObjectiveKind::kIncumbentStability, Direction::kMinimize),
                          MakeObjective(ObjectiveKind::kCost, Direction::kMinimize)});
  request.policy.churn.threshold_objective_index = 0;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{3});
  FP_CHECK_EQ(decision.delta, IncumbentDelta::kKept);
  FP_REQUIRE(!decision.explanation.ranked.empty());
  FP_CHECK(decision.explanation.ranked[0].is_incumbent);
}

FP_TEST(engine_policy, churn_threshold_keeps_the_incumbent_within_tolerance) {
  PlacementRequest request = Baseline(3);
  request.candidates.paths[0].attributes.cost_micro = 100;
  request.candidates.paths[2].attributes.cost_micro = 120;
  SetIncumbentOn(request, PathId{3});
  request.policy.churn.prefer_incumbent = true;
  request.policy.churn.move_improvement_threshold = 50;
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{3});
  FP_CHECK_EQ(decision.delta, IncumbentDelta::kKept);
  FP_CHECK(HasDegradation(decision, DegradationFlag::kChurnSuppressed));

  request.policy.churn.move_improvement_threshold = 5;
  decision = engine.Place(request);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{1});
  FP_CHECK_EQ(decision.delta, IncumbentDelta::kMoved);
  FP_CHECK(HasDegradation(decision, DegradationFlag::kIncumbentSuperseded));
}

FP_TEST(engine_policy, churn_policy_can_defer_instead_of_moving) {
  PlacementRequest request = Baseline(3);
  SetIncumbentOn(request, PathId{3});
  request.policy.churn.prefer_incumbent = true;
  request.policy.churn.on_move = ChurnAction::kDefer;
  request.policy.churn.move_improvement_threshold = 0;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kDeferred);
  FP_CHECK_EQ(decision.code, StatusCode::kPolicyMoveSuppressed);
  FP_CHECK(!decision.intent.has_value());
}

FP_TEST(engine_policy, churn_deferral_also_applies_when_the_incumbent_is_ineligible) {
  PlacementRequest request = Baseline(3);
  SetIncumbentOn(request, PathId{3});
  request.incumbent->path_authority = PathAuthorityGeneration{kPathAuthority + 1};
  request.policy.churn.prefer_incumbent = true;
  request.policy.churn.on_move = ChurnAction::kDefer;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kDeferred);
  FP_CHECK_EQ(decision.delta, IncumbentDelta::kReplacedStale);
  FP_CHECK(!decision.intent.has_value());
}

FP_TEST(engine_policy, admission_gate_defers_or_degrades) {
  PlacementRequest request = Baseline(2, 1500);
  request.policy.admission.min_residual_headroom_bytes = 100000;
  request.policy.admission.on_below = GateAction::kDefer;
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kDeferred);
  FP_CHECK_EQ(decision.code, StatusCode::kPolicyHeadroomGate);
  FP_REQUIRE(!decision.explanation.binding.empty());
  FP_CHECK_EQ(decision.explanation.binding[0].required, std::uint64_t{100000});

  request.policy.admission.on_below = GateAction::kPlace;
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlacedDegraded);
  FP_CHECK(HasDegradation(decision, DegradationFlag::kHeadroomBelowComfort));
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{1});
}

FP_TEST(engine_policy, locality_preference_degrades_but_does_not_reject) {
  PlacementRequest request = Baseline(3);
  request.policy.locality.preferred_locality = LocalityId{12};
  SetObjectives(request, {MakeObjective(ObjectiveKind::kLocalityAffinity, Direction::kMinimize),
                          MakeObjective(ObjectiveKind::kCost, Direction::kMinimize)});
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{3});

  SetObjectives(request, {MakeObjective(ObjectiveKind::kCost, Direction::kMinimize)});
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlacedDegraded);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{1});
  FP_CHECK(HasDegradation(decision, DegradationFlag::kLocalityPreferenceUnmet));
}

FP_TEST(engine_policy, reservation_preference_degrades_but_does_not_reject) {
  PlacementRequest request = Baseline(2);
  ReservationRef ref;
  ref.id = ReservationId{31};
  ref.generation = ReservationGeneration{1};
  request.policy.reservation_affinity.refs = {ref};
  SetObjectives(request, {MakeObjective(ObjectiveKind::kReservationAffinity, Direction::kMinimize),
                          MakeObjective(ObjectiveKind::kCost, Direction::kMinimize)});
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlacedDegraded);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK(HasDegradation(decision, DegradationFlag::kReservationPreferenceUnmet));

  request.candidates.paths[1].attributes.reservations = {ref};
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_CHECK_EQ(decision.intent->path, PathId{2});
  FP_CHECK(decision.intent->reservation == ref);
}

FP_TEST(engine_policy, tier_and_tier_objective_are_independent) {
  PlacementRequest request = Baseline(3);
  request.candidates.paths[0].attributes.tier = PathTier::kEconomy;
  request.candidates.paths[1].attributes.tier = PathTier::kPremium;
  request.candidates.paths[2].attributes.tier = PathTier::kStandard;
  SetObjectives(request, {MakeObjective(ObjectiveKind::kTier, Direction::kMinimize)});
  const PlacementEngine engine;
  FP_CHECK_EQ(engine.Place(request).intent->path, PathId{2});

  request.policy.allowed_tiers = {PathTier::kEconomy, PathTier::kStandard};
  FP_CHECK_EQ(engine.Place(request).intent->path, PathId{3});
}

FP_TEST(engine_policy, hop_count_objective_is_used) {
  PlacementRequest request = Baseline(3);
  request.candidates.paths[0].attributes.hop_count = 9;
  request.candidates.paths[1].attributes.hop_count = 3;
  request.candidates.paths[2].attributes.hop_count = 7;
  SetObjectives(request, {MakeObjective(ObjectiveKind::kHopCount, Direction::kMinimize)});
  const PlacementEngine engine;
  FP_CHECK_EQ(engine.Place(request).intent->path, PathId{2});
}

FP_TEST(engine_policy, revalidate_after_is_carried_into_the_intent) {
  PlacementRequest request = Baseline(2);
  request.policy.revalidate_after_nanos = 123456789;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->revalidate_after_nanos, std::uint64_t{123456789});
}

FP_TEST(engine_policy, unknown_threshold_objective_is_rejected) {
  PlacementRequest request = Baseline(1);
  request.policy.churn.threshold_objective_index = 4;
  FP_CHECK_EQ(ValidateRequest(request, Limits{}).code(), StatusCode::kInvalidFieldValue);
}

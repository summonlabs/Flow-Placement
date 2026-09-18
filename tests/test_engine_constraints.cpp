// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fixtures.hpp"
#include "harness.hpp"

#include <string>
#include <vector>

using namespace flowplace;
using namespace fptest;

namespace {

Status Validate(const PlacementRequest& request) { return ValidateRequest(request, Limits{}); }

bool HasExclusion(const PlacementDecision& decision, ExclusionReason reason) {
  for (const ExclusionSummary& summary : decision.explanation.exclusions) {
    if (summary.reason == reason) return true;
  }
  return false;
}

std::uint64_t ExclusionCount(const PlacementDecision& decision, ExclusionReason reason) {
  for (const ExclusionSummary& summary : decision.explanation.exclusions) {
    if (summary.reason == reason) return summary.count;
  }
  return 0;
}

}  // namespace

FP_TEST(engine_constraints, exclusion_reason_follows_the_documented_order) {
  // Capacity evidence is evaluated before affinity and latency, so the recorded
  // reason is the first one in the documented order.
  PlacementRequest request = Baseline(1);
  request.qos.max_latency_nanos = 1;
  request.capacity.entries.clear();
  ReservationRef ref;
  ref.id = ReservationId{5};
  ref.generation = ReservationGeneration{1};
  request.policy.reservation_affinity.required = true;
  request.policy.reservation_affinity.refs = {ref};
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kInsufficientCapacity);
  FP_CHECK_EQ(decision.code, StatusCode::kNotFound);
  FP_REQUIRE(decision.explanation.exclusions.size() == 1);
  FP_CHECK_EQ(decision.explanation.exclusions[0].reason, ExclusionReason::kCapacityUnknown);
}

FP_TEST(engine_constraints, absent_failure_domain_is_not_occupied) {
  // Identity 0 means absent, so an absent domain can never match the
  // occupied-domain set.
  PlacementRequest request = Baseline(1);
  request.candidates.paths[0].attributes.failure_domain = FailureDomainId{};
  request.policy.failure_domains.avoid_occupied_domains = true;
  request.occupied_failure_domains = {FailureDomainId{}};
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_CHECK(!HasExclusion(decision, ExclusionReason::kFailureDomainOccupied));
}

FP_TEST(engine_constraints, tier_policy_excludes_and_rejects) {
  PlacementRequest request = Baseline(3);
  request.policy.allowed_tiers = {PathTier::kPremium};
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPolicyRejected);
  FP_CHECK_EQ(decision.code, StatusCode::kPolicyRejectedAllPaths);
  FP_CHECK_EQ(ExclusionCount(decision, ExclusionReason::kTierNotAllowed), std::uint64_t{3});
  request.candidates.paths[1].attributes.tier = PathTier::kPremium;
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{2});
}

FP_TEST(engine_constraints, labels_are_hard_constraints) {
  PlacementRequest request = Baseline(2);
  request.candidates.paths[0].attributes.labels = {PolicyLabelId{5}};
  request.candidates.paths[1].attributes.labels = {PolicyLabelId{6}};
  request.policy.required_labels = {PolicyLabelId{6}};
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{2});
  FP_CHECK_EQ(ExclusionCount(decision, ExclusionReason::kMissingRequiredLabel), std::uint64_t{1});

  request.policy.required_labels.clear();
  request.policy.forbidden_labels = {PolicyLabelId{5}, PolicyLabelId{6}};
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPolicyRejected);
  FP_CHECK_EQ(ExclusionCount(decision, ExclusionReason::kForbiddenLabel), std::uint64_t{2});
}

FP_TEST(engine_constraints, forbidden_and_required_locality_are_contradictory) {
  PlacementRequest request = Baseline(2);
  request.policy.locality.required_locality = LocalityId{10};
  request.policy.locality.forbidden_localities = {LocalityId{10}};
  const Status status = Validate(request);
  FP_CHECK_EQ(status.code(), StatusCode::kContradictoryConstraints);
}

FP_TEST(engine_constraints, forbidden_and_required_failure_domain_are_contradictory) {
  PlacementRequest request = Baseline(2);
  request.policy.failure_domains.required_domain = FailureDomainId{20};
  request.policy.failure_domains.forbidden_domains = {FailureDomainId{20}};
  const Status status = Validate(request);
  FP_CHECK_EQ(status.code(), StatusCode::kContradictoryConstraints);
}

FP_TEST(engine_constraints, failure_domain_requirements_are_hard) {
  PlacementRequest request = Baseline(3);
  request.policy.failure_domains.required_domain = FailureDomainId{21};
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{2});
  FP_CHECK_EQ(ExclusionCount(decision, ExclusionReason::kRequiredFailureDomainMismatch),
              std::uint64_t{2});

  request.policy.failure_domains.required_domain.reset();
  request.policy.failure_domains.forbidden_domains = {FailureDomainId{20}, FailureDomainId{21}};
  request.policy.failure_domains.avoid_occupied_domains = true;
  request.occupied_failure_domains = {FailureDomainId{22}};
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPolicyRejected);
  FP_CHECK_EQ(ExclusionCount(decision, ExclusionReason::kForbiddenFailureDomain), std::uint64_t{2});
  FP_CHECK_EQ(ExclusionCount(decision, ExclusionReason::kFailureDomainOccupied), std::uint64_t{1});
}

FP_TEST(engine_constraints, failure_domain_diversity_is_enforced_over_legal_candidates) {
  PlacementRequest request = Baseline(3);
  request.policy.failure_domains.min_distinct_domains = 3;
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_CHECK_EQ(decision.explanation.distinct_failure_domains, std::uint64_t{3});

  // Collapsing two paths onto one domain would make the requirement
  // unsatisfiable by construction, which is a structural rejection.
  request.candidates.paths[2].attributes.failure_domain = FailureDomainId{20};
  FP_CHECK_EQ(Validate(request).code(), StatusCode::kContradictoryConstraints);

  // Losing a domain through a hard exclusion is a feasibility failure.
  request.candidates.paths[2].attributes.failure_domain = FailureDomainId{22};
  request.policy.failure_domains.forbidden_domains = {FailureDomainId{22}};
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kNoLegalPath);
  FP_CHECK_EQ(decision.code, StatusCode::kFailureDomainDiversityUnmet);
  FP_CHECK(!decision.intent.has_value());
  FP_CHECK_EQ(decision.explanation.distinct_failure_domains, std::uint64_t{2});
}

FP_TEST(engine_constraints, unsatisfiable_diversity_requirement_is_structural) {
  PlacementRequest request = Baseline(2);
  request.policy.failure_domains.min_distinct_domains = 5;
  const Status status = Validate(request);
  FP_CHECK_EQ(status.code(), StatusCode::kContradictoryConstraints);
}

FP_TEST(engine_constraints, latency_budget_is_hard) {
  PlacementRequest request = Baseline(3);
  request.qos.max_latency_nanos = 1050;
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{1});
  FP_CHECK_EQ(ExclusionCount(decision, ExclusionReason::kLatencyBudgetExceeded), std::uint64_t{2});
  FP_REQUIRE(!decision.explanation.binding.empty());
  FP_CHECK_EQ(decision.explanation.binding[0].path, PathId{1});
  FP_CHECK_EQ(decision.explanation.binding[0].label, std::string("chosen path headroom"));

  request.qos.max_latency_nanos = 10;
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kNoLegalPath);
  FP_CHECK_EQ(decision.code, StatusCode::kLatencyBudgetUnmet);
}

FP_TEST(engine_constraints, reservation_affinity_required_is_hard) {
  PlacementRequest request = Baseline(2);
  ReservationRef ref;
  ref.id = ReservationId{77};
  ref.generation = ReservationGeneration{2};
  request.policy.reservation_affinity.required = true;
  request.policy.reservation_affinity.refs = {ref};
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPolicyRejected);
  FP_CHECK_EQ(ExclusionCount(decision, ExclusionReason::kReservationAffinityUnsatisfied),
              std::uint64_t{2});

  request.candidates.paths[1].attributes.reservations = {ref};
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{2});
  FP_CHECK(decision.intent->reservation == ref);
}

FP_TEST(engine_constraints, reservation_affinity_must_match_the_exact_generation) {
  PlacementRequest request = Baseline(1);
  ReservationRef ref;
  ref.id = ReservationId{77};
  ref.generation = ReservationGeneration{2};
  ReservationRef stale = ref;
  stale.generation = ReservationGeneration{1};
  request.policy.reservation_affinity.required = true;
  request.policy.reservation_affinity.refs = {ref};
  request.candidates.paths[0].attributes.reservations = {stale};
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPolicyRejected);
}

FP_TEST(engine_constraints, service_class_headroom_is_enforced_and_relaxable) {
  PlacementRequest request = Baseline(2, 250);
  request.qos.service_class = ServiceClass::kAssured;
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kNoLegalPath);
  FP_CHECK_EQ(decision.code, StatusCode::kServiceClassUnsatisfied);
  FP_CHECK_EQ(ExclusionCount(decision, ExclusionReason::kServiceHeadroomUnmet), std::uint64_t{2});
  FP_REQUIRE(!decision.explanation.binding.empty());
  FP_CHECK_EQ(decision.explanation.binding[0].required, std::uint64_t{300});

  request.policy.service.allow_degraded = true;
  request.policy.service.max_relaxation_steps = 1;
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlacedDegraded);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{1});
  bool service_relaxed = false;
  for (const DegradationFlag flag : decision.explanation.degradations) {
    if (flag == DegradationFlag::kServiceRelaxed) service_relaxed = true;
  }
  FP_CHECK(service_relaxed);
}

FP_TEST(engine_constraints, service_relaxation_steps_are_bounded) {
  PlacementRequest request = Baseline(1);
  request.policy.service.allow_degraded = true;
  request.policy.service.max_relaxation_steps = 3;
  const Status status = Validate(request);
  FP_CHECK_EQ(status.code(), StatusCode::kInvalidFieldValue);

  PlacementRequest inconsistent = Baseline(1);
  inconsistent.policy.service.max_relaxation_steps = 1;
  FP_CHECK_EQ(Validate(inconsistent).code(), StatusCode::kInvalidFieldValue);
}

FP_TEST(engine_constraints, reserved_service_class_requires_a_reservation) {
  PlacementRequest request = Baseline(2);
  request.qos.service_class = ServiceClass::kReserved;
  const PlacementEngine engine;
  PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kNoLegalPath);
  FP_CHECK_EQ(decision.code, StatusCode::kServiceClassUnsatisfied);
  FP_CHECK_EQ(ExclusionCount(decision, ExclusionReason::kServiceReservationUnmet), std::uint64_t{2});

  ReservationRef ref;
  ref.id = ReservationId{5};
  ref.generation = ReservationGeneration{1};
  request.candidates.paths[0].attributes.reservations = {ref};
  decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kPlaced);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK_EQ(decision.intent->path, PathId{1});
  FP_CHECK(decision.intent->reservation == ref);
}

FP_TEST(engine_constraints, base_capacity_requirement_is_never_relaxed) {
  PlacementRequest request = Baseline(2, 50);
  request.qos.service_class = ServiceClass::kAssured;
  request.qos.required_residual_bytes = 100;
  request.policy.service.allow_degraded = true;
  request.policy.service.max_relaxation_steps = 2;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kInsufficientCapacity);
  FP_CHECK_EQ(ExclusionCount(decision, ExclusionReason::kCapacityInsufficient), std::uint64_t{2});
}

FP_TEST(engine_constraints, duplicate_path_ids_are_rejected) {
  PlacementRequest request = Baseline(3);
  request.candidates.paths[2].id = request.candidates.paths[1].id;
  FP_CHECK_EQ(Validate(request).code(), StatusCode::kDuplicatePathId);
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_CHECK_EQ(decision.outcome, Outcome::kConflictingInput);
  FP_CHECK_EQ(decision.code, StatusCode::kDuplicatePathId);
  FP_CHECK(!decision.intent.has_value());
}

FP_TEST(engine_constraints, one_path_with_two_authority_generations_is_inconsistent) {
  PlacementRequest request = Baseline(3);
  request.candidates.paths[2].id = request.candidates.paths[1].id;
  request.candidates.paths[2].authority = PathAuthorityGeneration{kPathAuthority + 1};
  FP_CHECK_EQ(Validate(request).code(), StatusCode::kInconsistentAuthorityGeneration);
}

FP_TEST(engine_constraints, unbounded_identity_and_generation_fields_are_rejected) {
  PlacementRequest request = Baseline(1);
  request.flow = FlowId{0};
  FP_CHECK_EQ(Validate(request).code(), StatusCode::kInvalidId);

  PlacementRequest missing_expected = Baseline(1);
  missing_expected.expected.path_authority = PathAuthorityGeneration{0};
  FP_CHECK_EQ(Validate(missing_expected).code(), StatusCode::kMissingField);

  PlacementRequest no_objectives = Baseline(1);
  no_objectives.policy.objectives.clear();
  FP_CHECK_EQ(Validate(no_objectives).code(), StatusCode::kMissingField);

  PlacementRequest duplicate_objective = Baseline(1);
  duplicate_objective.policy.objectives.push_back(
      MakeObjective(ObjectiveKind::kCost, Direction::kMinimize));
  FP_CHECK_EQ(Validate(duplicate_objective).code(), StatusCode::kInvalidFieldValue);
}

FP_TEST(engine_constraints, duplicate_capacity_and_evidence_entries_are_rejected) {
  PlacementRequest request = Baseline(2);
  request.capacity.entries.push_back(request.capacity.entries[0]);
  FP_CHECK_EQ(Validate(request).code(), StatusCode::kDuplicatePathId);

  PlacementRequest evidence = Baseline(2);
  evidence.evidence.entries.push_back(evidence.evidence.entries[1]);
  FP_CHECK_EQ(Validate(evidence).code(), StatusCode::kDuplicatePathId);

  PlacementRequest residual = Baseline(1);
  residual.capacity.entries[0].residual_bytes = residual.capacity.entries[0].capacity_bytes + 1;
  FP_CHECK_EQ(Validate(residual).code(), StatusCode::kInvalidFieldValue);
}

FP_TEST(engine_constraints, oversize_inputs_are_rejected_not_truncated) {
  Limits limits;
  limits.max_candidate_paths = 4;
  PlacementRequest request = Baseline(5);
  FP_CHECK_EQ(ValidateRequest(request, limits).code(), StatusCode::kOversizeRequest);

  Limits tight_objectives;
  tight_objectives.max_objectives = 0;
  PlacementRequest objective = Baseline(1);
  FP_CHECK_EQ(ValidateRequest(objective, tight_objectives).code(), StatusCode::kOversizeRequest);

  Limits tiny_labels;
  tiny_labels.max_path_labels = 1;
  PlacementRequest labels = Baseline(1);
  labels.candidates.paths[0].attributes.labels = {PolicyLabelId{1}, PolicyLabelId{2}};
  FP_CHECK_EQ(ValidateRequest(labels, tiny_labels).code(), StatusCode::kOversizeRequest);
}

FP_TEST(engine_constraints, overflow_in_service_requirement_is_rejected) {
  PlacementRequest request = Baseline(1);
  request.qos.service_class = ServiceClass::kAssured;
  request.qos.required_residual_bytes = 0xFFFFFFFFFFFFFFFFull;
  FP_CHECK_EQ(Validate(request).code(), StatusCode::kArithmeticOverflow);
}

FP_TEST(engine_constraints, unset_path_authority_is_structurally_invalid) {
  PlacementRequest request = Baseline(1);
  request.candidates.paths[0].authority = PathAuthorityGeneration{0};
  FP_CHECK_EQ(Validate(request).code(), StatusCode::kInconsistentAuthorityGeneration);
}

FP_TEST(engine_constraints, policy_deferring_moves_must_prefer_the_incumbent) {
  PlacementRequest request = Baseline(2);
  request.policy.churn.on_move = ChurnAction::kDefer;
  request.policy.churn.prefer_incumbent = false;
  FP_CHECK_EQ(Validate(request).code(), StatusCode::kInvalidFieldValue);
}

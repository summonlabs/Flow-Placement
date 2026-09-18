// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fixtures.hpp"
#include "harness.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace flowplace;
using namespace fptest;

namespace {

PlacementRequest RandomRequest(Rng* rng) {
  PlacementRequest request = Baseline(1 + rng->Below(12));
  const std::uint64_t objectives = 1 + rng->Below(4);
  std::vector<Objective> list;
  const ObjectiveKind kinds[] = {ObjectiveKind::kCost, ObjectiveKind::kLatency,
                                 ObjectiveKind::kResidualCapacity, ObjectiveKind::kHopCount,
                                 ObjectiveKind::kCongestionUtilization};
  for (std::uint64_t i = 0; i < objectives; ++i) {
    list.push_back(MakeObjective(kinds[rng->Below(5)],
                                 rng->Chance(50) ? Direction::kMinimize : Direction::kMaximize));
  }
  // Objective kinds must be unique for a valid policy.
  std::sort(list.begin(), list.end(), [](const Objective& a, const Objective& b) {
    return static_cast<int>(a.kind) < static_cast<int>(b.kind);
  });
  list.erase(std::unique(list.begin(), list.end(),
                         [](const Objective& a, const Objective& b) { return a.kind == b.kind; }),
             list.end());
  SetObjectives(request, list);
  for (std::size_t i = 0; i < request.candidates.paths.size(); ++i) {
    CandidatePath& path = request.candidates.paths[i];
    path.attributes.cost_micro = rng->Below(100000);
    path.attributes.latency_nanos = rng->Below(1000000);
    path.attributes.hop_count = static_cast<std::uint16_t>(rng->Below(16));
    path.attributes.tier = static_cast<PathTier>(rng->Below(3));
    path.attributes.failure_domain = FailureDomainId{20 + rng->Below(4)};
    if (rng->Chance(20)) path.attributes.labels = {PolicyLabelId{1 + rng->Below(4)}};
    request.capacity.entries[i].residual_bytes = rng->Below(20000);
    request.capacity.entries[i].capacity_bytes = 20000;
    request.evidence.entries[i].utilization_ppb = static_cast<std::uint32_t>(rng->Below(1000000000u));
  }
  if (rng->Chance(25)) {
    SetIncumbentOn(request, request.candidates.paths[rng->Below(request.candidates.paths.size())].id);
    request.policy.churn.prefer_incumbent = rng->Chance(70);
    request.policy.churn.move_improvement_threshold = rng->Below(5000);
  }
  if (rng->Chance(20)) request.qos.required_residual_bytes = rng->Below(5000);
  if (rng->Chance(15)) request.qos.max_latency_nanos = rng->Below(500000);
  if (rng->Chance(20)) {
    request.policy.allowed_tiers = {static_cast<PathTier>(rng->Below(3))};
  }
  if (rng->Chance(20)) {
    request.policy.admission.min_residual_headroom_bytes = rng->Below(20000);
    request.policy.admission.on_below = rng->Chance(50) ? GateAction::kDefer : GateAction::kPlace;
  }
  if (rng->Chance(15)) request.policy.evidence.on_missing = EvidencePolicyMode::kRankWorst;
  return request;
}

}  // namespace

FP_TEST(property_determinism, reservation_selection_is_order_independent) {
  // The chosen reservation is the canonical element of the path's set.
  PlacementRequest request = Baseline(1);
  ReservationRef low;
  low.id = ReservationId{5};
  low.generation = ReservationGeneration{1};
  ReservationRef high;
  high.id = ReservationId{9};
  high.generation = ReservationGeneration{1};
  request.qos.service_class = ServiceClass::kReserved;
  request.qos.required_residual_bytes = 100;
  request.candidates.paths[0].attributes.reservations = {high, low};
  const PlacementEngine engine;
  const PlacementDecision first = engine.Place(request);
  FP_REQUIRE(first.intent.has_value());
  FP_CHECK(first.intent->reservation == low);

  request.candidates.paths[0].attributes.reservations = {low, high};
  const PlacementDecision second = engine.Place(request);
  FP_REQUIRE(second.intent.has_value());
  FP_CHECK(second.intent->reservation == low);
  FP_CHECK_EQ(first.intent->digest, second.intent->digest);
  FP_CHECK_EQ(first.digest, second.digest);
}

FP_TEST(property_determinism, verify_intent_rejects_an_untrusted_evidence_binding) {
  // A placement may only be bound to evidence the request declared trusted.
  PlacementRequest request = Baseline(2);
  SetObjectives(request,
                {MakeObjective(ObjectiveKind::kCongestionUtilization, Direction::kMinimize)});
  request.expected.evidence = EvidenceGeneration{};
  request.policy.evidence.on_stale = EvidencePolicyMode::kRankWorst;
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_REQUIRE(decision.intent.has_value());
  FP_CHECK(!decision.intent->evidence_generation.valid());

  PlacementIntent untrusted = *decision.intent;
  untrusted.evidence = request.evidence.id;
  untrusted.evidence_generation = request.evidence.generation;
  untrusted.digest = IntentDigest(untrusted);
  std::string reason;
  FP_CHECK(!engine.VerifyIntent(request, untrusted, &reason));
  FP_CHECK(!reason.empty());
}

FP_TEST(property_determinism, seeded_populations_place_deterministically) {
  const PlacementEngine engine;
  for (std::uint64_t seed = 1; seed <= 150; ++seed) {
    Rng rng(seed);
    const PlacementRequest request = RandomRequest(&rng);
    const PlacementDecision first = engine.Place(request);
    const PlacementDecision second = engine.Place(request);
    FP_CHECK_EQ(first.digest, second.digest);
    FP_CHECK_EQ(first.outcome, second.outcome);
    FP_CHECK_EQ(first.code, second.code);
    FP_CHECK_EQ(first.delta, second.delta);
    if (first.placed()) {
      FP_REQUIRE(first.intent.has_value());
      std::string reason;
      FP_CHECK(engine.VerifyIntent(request, *first.intent, &reason));
      bool found = false;
      for (const CandidatePath& path : request.candidates.paths) {
        if (path.id == first.intent->path) {
          found = true;
          FP_CHECK_EQ(path.authority, first.intent->path_authority);
        }
      }
      FP_CHECK(found);
      bool degrading = false;
      for (const DegradationFlag flag : first.explanation.degradations) {
        if (IsDegradingFlag(flag)) degrading = true;
      }
      FP_CHECK_EQ(first.outcome, degrading ? Outcome::kPlacedDegraded : Outcome::kPlaced);
    } else {
      FP_CHECK(!first.intent.has_value());
    }
  }
}

FP_TEST(property_determinism, reordering_inputs_never_changes_the_decision) {
  const PlacementEngine engine;
  for (std::uint64_t seed = 1; seed <= 120; ++seed) {
    Rng rng(seed * 7919);
    PlacementRequest request = RandomRequest(&rng);
    const PlacementDecision baseline = engine.Place(request);
    // Deterministic shuffle of every unordered collection.
    for (std::size_t i = request.candidates.paths.size(); i > 1; --i) {
      std::swap(request.candidates.paths[i - 1],
                request.candidates.paths[rng.Below(i)]);
    }
    for (std::size_t i = request.capacity.entries.size(); i > 1; --i) {
      std::swap(request.capacity.entries[i - 1], request.capacity.entries[rng.Below(i)]);
    }
    for (std::size_t i = request.evidence.entries.size(); i > 1; --i) {
      std::swap(request.evidence.entries[i - 1], request.evidence.entries[rng.Below(i)]);
    }
    const PlacementDecision shuffled = engine.Place(request);
    if (!(baseline.digest == shuffled.digest)) {
      std::fprintf(stderr, "=== seed %llu order-dependent ===\n--- baseline ---\n%s--- shuffled ---\n%s",
                   static_cast<unsigned long long>(seed * 7919),
                   RenderDecision(baseline).c_str(), RenderDecision(shuffled).c_str());
    }
    FP_CHECK_EQ(baseline.digest, shuffled.digest);
    FP_CHECK_EQ(baseline.outcome, shuffled.outcome);
    if (baseline.placed()) {
      FP_REQUIRE(baseline.intent.has_value() && shuffled.intent.has_value());
      FP_CHECK_EQ(baseline.intent->digest, shuffled.intent->digest);
    }
  }
}

FP_TEST(property_determinism, engine_is_reentrant_across_threads) {
  const PlacementEngine engine;
  std::vector<PlacementRequest> requests;
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    Rng rng(seed);
    requests.push_back(RandomRequest(&rng));
  }
  std::vector<Digest> expected;
  for (const PlacementRequest& request : requests) expected.push_back(engine.Place(request).digest);

  std::vector<std::vector<Digest>> observed(8);
  std::vector<std::thread> threads;
  threads.reserve(observed.size());
  for (std::size_t t = 0; t < observed.size(); ++t) {
    threads.emplace_back([&engine, &requests, &observed, t]() {
      for (int round = 0; round < 20; ++round) {
        for (const PlacementRequest& request : requests) {
          observed[t].push_back(engine.Place(request).digest);
        }
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  for (const std::vector<Digest>& list : observed) {
    FP_CHECK_EQ(list.size(), expected.size() * 20);
    for (std::size_t i = 0; i < list.size(); ++i) {
      FP_CHECK(list[i] == expected[i % expected.size()]);
    }
  }
}

FP_TEST(property_determinism, verify_intent_rejects_every_tampering) {
  PlacementRequest request = Baseline(3);
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  FP_REQUIRE(decision.intent.has_value());
  const PlacementIntent original = *decision.intent;
  std::string reason;

  PlacementIntent tampered = original;
  tampered.path = PathId{999};
  FP_CHECK(!engine.VerifyIntent(request, tampered, &reason));
  tampered = original;
  tampered.path_authority = PathAuthorityGeneration{kPathAuthority + 1};
  FP_CHECK(!engine.VerifyIntent(request, tampered, &reason));
  tampered = original;
  tampered.candidate_set_generation = CandidateSetGeneration{kCandidateSetGeneration + 1};
  FP_CHECK(!engine.VerifyIntent(request, tampered, &reason));
  tampered = original;
  tampered.capacity_generation = CapacitySnapshotGeneration{kCapacityGeneration + 1};
  FP_CHECK(!engine.VerifyIntent(request, tampered, &reason));
  tampered = original;
  tampered.policy_generation = PolicyGeneration{kPolicyGeneration + 1};
  FP_CHECK(!engine.VerifyIntent(request, tampered, &reason));
  tampered = original;
  tampered.qos_generation = QosGeneration{kQosGeneration + 1};
  FP_CHECK(!engine.VerifyIntent(request, tampered, &reason));
  tampered = original;
  tampered.fabric_epoch = FabricEpoch{kEpoch + 1};
  FP_CHECK(!engine.VerifyIntent(request, tampered, &reason));
  tampered = original;
  tampered.flow = FlowId{kFlow + 1};
  FP_CHECK(!engine.VerifyIntent(request, tampered, &reason));
  tampered = original;
  tampered.reservation = ReservationRef{ReservationId{5}, ReservationGeneration{5}};
  FP_CHECK(!engine.VerifyIntent(request, tampered, &reason));
  FP_CHECK(engine.VerifyIntent(request, original, &reason));

  // A placement on a path whose evidence is now insufficient must fail
  // verification against the updated request.
  PlacementRequest tightened = request;
  tightened.capacity.entries[0].residual_bytes = 1;
  FP_CHECK(!engine.VerifyIntent(tightened, original, &reason));
}

FP_TEST(property_determinism, explanations_stay_bounded_on_large_populations) {
  const PlacementEngine engine;
  for (std::uint64_t fanout : {1000u, 40000u}) {
    Rng rng(fanout);
    PlacementRequest request = Baseline(fanout);
    for (std::size_t i = 0; i < request.candidates.paths.size(); ++i) {
      request.capacity.entries[i].residual_bytes = rng.Below(2) == 0 ? 1 : 100000;
      request.capacity.entries[i].capacity_bytes = 200000;
      request.candidates.paths[i].attributes.cost_micro = rng.Below(1000000);
    }
    const PlacementDecision decision = engine.Place(request);
    FP_CHECK(decision.explanation.ranked.size() <= 32);
    FP_CHECK(decision.explanation.exclusions.size() <= 64);
    FP_CHECK(decision.explanation.binding.size() <= 8);
    FP_CHECK(decision.explanation.notes.size() <= 16);
    for (const ExclusionSummary& summary : decision.explanation.exclusions) {
      FP_CHECK(summary.sample.size() <= 4);
    }
    FP_CHECK_EQ(decision.explanation.candidate_count, fanout);
  }
}

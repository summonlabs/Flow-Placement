// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Shared fixtures for the test suite.

#ifndef FLOWPLACE_TESTS_FIXTURES_HPP
#define FLOWPLACE_TESTS_FIXTURES_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "flowplace/engine.hpp"
#include "flowplace/model.hpp"
#include "flowplace/scenario.hpp"

namespace fptest {

using namespace flowplace;

inline constexpr std::uint64_t kFlow = 100;
inline constexpr std::uint64_t kFlowGeneration = 3;
inline constexpr std::uint64_t kCandidateSet = 200;
inline constexpr std::uint64_t kCapacitySnapshot = 300;
inline constexpr std::uint64_t kQos = 400;
inline constexpr std::uint64_t kPolicy = 500;
inline constexpr std::uint64_t kEvidence = 600;
inline constexpr std::uint64_t kPathAuthority = 7;
inline constexpr std::uint64_t kCandidateSetGeneration = 2;
inline constexpr std::uint64_t kCapacityGeneration = 5;
inline constexpr std::uint64_t kQosGeneration = 4;
inline constexpr std::uint64_t kPolicyGeneration = 6;
inline constexpr std::uint64_t kEvidenceGeneration = 9;
inline constexpr std::uint64_t kEpoch = 11;

// A minimal, valid request: |path_count| candidate paths, cost-ordered
// placement, sufficient capacity evidence for every path, no incumbent.
inline PlacementRequest Baseline(std::uint64_t path_count = 3,
                                std::uint64_t residual = 5000) {
  PlacementRequest request;
  request.flow = FlowId{kFlow};
  request.flow_generation = FlowGeneration{kFlowGeneration};
  request.candidates.id = CandidateSetId{kCandidateSet};
  request.candidates.generation = CandidateSetGeneration{kCandidateSetGeneration};
  request.capacity.id = CapacitySnapshotId{kCapacitySnapshot};
  request.capacity.generation = CapacitySnapshotGeneration{kCapacityGeneration};
  request.qos.id = QosProfileId{kQos};
  request.qos.generation = QosGeneration{kQosGeneration};
  request.qos.service_class = ServiceClass::kBestEffort;
  request.qos.required_residual_bytes = 100;
  request.policy.id = PolicyId{kPolicy};
  request.policy.generation = PolicyGeneration{kPolicyGeneration};
  Objective objective;
  objective.kind = ObjectiveKind::kCost;
  objective.direction = Direction::kMinimize;
  request.policy.objectives.push_back(objective);
  request.policy.churn.threshold_objective_index = 0;
  request.evidence.id = EvidenceId{kEvidence};
  request.evidence.generation = EvidenceGeneration{kEvidenceGeneration};
  request.expected.path_authority = PathAuthorityGeneration{kPathAuthority};
  request.expected.candidate_set = CandidateSetGeneration{kCandidateSetGeneration};
  request.expected.capacity = CapacitySnapshotGeneration{kCapacityGeneration};
  request.expected.policy = PolicyGeneration{kPolicyGeneration};
  request.expected.qos = QosGeneration{kQosGeneration};
  request.expected.fabric_epoch = FabricEpoch{kEpoch};
  request.expected.evidence = EvidenceGeneration{kEvidenceGeneration};
  request.provenance.producer = "flowplace-tests";
  request.provenance.producer_version = "1.0.0";

  for (std::uint64_t i = 0; i < path_count; ++i) {
    CandidatePath path;
    path.id = PathId{1 + i};
    path.authority = PathAuthorityGeneration{kPathAuthority};
    path.attributes.tier = PathTier::kStandard;
    path.attributes.locality = LocalityId{10 + i};
    path.attributes.locality_scope = LocalityScope::kRack;
    path.attributes.failure_domain = FailureDomainId{20 + i};
    path.attributes.cost_micro = 100 + i * 10;
    path.attributes.latency_nanos = 1000 + i * 100;
    path.attributes.hop_count = static_cast<std::uint16_t>(1 + i);
    request.candidates.paths.push_back(path);

    PathCapacity capacity;
    capacity.path = path.id;
    capacity.capacity_bytes = residual * 2;
    capacity.residual_bytes = residual;
    request.capacity.entries.push_back(capacity);

    CongestionEvidenceEntry entry;
    entry.path = path.id;
    // Utilization evidence is a bounded 0..1e9 ppb quantity; keep the synthetic
    // population inside that range even for very large fan-outs.
    entry.utilization_ppb =
        100000000u + static_cast<std::uint32_t>((i * 1000000u) % 800000000u);
    request.evidence.entries.push_back(entry);
  }
  return request;
}

inline CandidatePath& PathAt(PlacementRequest& request, std::size_t index) {
  return request.candidates.paths[index];
}

inline PathCapacity& CapacityAt(PlacementRequest& request, std::size_t index) {
  return request.capacity.entries[index];
}

inline CongestionEvidenceEntry& EvidenceAt(PlacementRequest& request, std::size_t index) {
  return request.evidence.entries[index];
}

inline void SetObjectives(PlacementRequest& request, std::vector<Objective> objectives) {
  request.policy.objectives = std::move(objectives);
  request.policy.churn.threshold_objective_index = 0;
}

inline Objective MakeObjective(ObjectiveKind kind, Direction direction) {
  Objective objective;
  objective.kind = kind;
  objective.direction = direction;
  return objective;
}

inline void SetIncumbentOn(PlacementRequest& request, PathId path) {
  IncumbentPlacement incumbent;
  incumbent.id = PlacementId{9001};
  incumbent.generation = PlacementGeneration{4};
  incumbent.path = path;
  incumbent.path_authority = request.expected.path_authority;
  incumbent.candidate_set_generation = request.expected.candidate_set;
  incumbent.capacity_generation = request.expected.capacity;
  incumbent.policy_generation = request.expected.policy;
  incumbent.qos_generation = request.expected.qos;
  incumbent.fabric_epoch = request.expected.fabric_epoch;
  request.incumbent = incumbent;
}

// Deterministic pseudo-random generator for seeded property tests.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed + 0x9E3779B97F4A7C15ull) {}

  std::uint64_t Next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  std::uint64_t Below(std::uint64_t bound) { return bound == 0 ? 0 : Next() % bound; }

  bool Chance(std::uint64_t percent) { return Below(100) < percent; }

 private:
  std::uint64_t state_;
};

}  // namespace fptest

#endif  // FLOWPLACE_TESTS_FIXTURES_HPP

// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/bench.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "flowplace/hash.hpp"
#include "flowplace/version.hpp"

namespace flowplace {
namespace {

// Deterministic synthetic population generator (splitmix64).
class SyntheticRng {
 public:
  explicit SyntheticRng(std::uint64_t seed) : state_(seed + 0x9E3779B97F4A7C15ull) {}

  std::uint64_t Next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  std::uint64_t Below(std::uint64_t bound) { return bound == 0 ? 0 : Next() % bound; }

 private:
  std::uint64_t state_;
};

PlacementRequest MakeRequest(const BenchConfig& config, std::uint64_t index, SyntheticRng* rng) {
  const std::uint64_t fanout = std::max<std::uint64_t>(1, config.fanout);
  const std::uint64_t domains = std::max<std::uint64_t>(1, config.domains);
  PlacementRequest request;
  request.flow = FlowId{1000 + index};
  request.flow_generation = FlowGeneration{1 + index};
  request.candidates.id = CandidateSetId{7};
  request.candidates.generation = CandidateSetGeneration{3};
  request.candidates.paths.reserve(static_cast<std::size_t>(fanout));
  for (std::uint64_t i = 0; i < fanout; ++i) {
    CandidatePath path;
    path.id = PathId{1 + i};
    path.authority = PathAuthorityGeneration{5};
    path.attributes.tier = static_cast<PathTier>(rng->Below(3));
    path.attributes.locality = LocalityId{1 + rng->Below(std::max<std::uint64_t>(1, config.resources))};
    path.attributes.locality_scope = static_cast<LocalityScope>(rng->Below(5));
    path.attributes.failure_domain = FailureDomainId{1 + rng->Below(domains)};
    path.attributes.cost_micro = 100 + rng->Below(10000);
    path.attributes.latency_nanos = 1000 + rng->Below(1000000);
    path.attributes.hop_count = static_cast<std::uint16_t>(1 + rng->Below(9));
    if (rng->Below(4) == 0) path.attributes.labels.push_back(PolicyLabelId{1 + rng->Below(8)});
    if (rng->Below(3) == 0) {
      ReservationRef ref;
      ref.id = ReservationId{1 + rng->Below(64)};
      ref.generation = ReservationGeneration{1 + rng->Below(4)};
      path.attributes.reservations.push_back(ref);
    }
    request.candidates.paths.push_back(std::move(path));
  }
  request.capacity.id = CapacitySnapshotId{9};
  request.capacity.generation = CapacitySnapshotGeneration{4};
  request.capacity.entries.reserve(static_cast<std::size_t>(fanout));
  for (const CandidatePath& path : request.candidates.paths) {
    PathCapacity entry;
    entry.path = path.id;
    entry.capacity_bytes = 1000000;
    entry.residual_bytes = 50000 + rng->Below(950000);
    request.capacity.entries.push_back(entry);
  }
  request.qos.id = QosProfileId{11};
  request.qos.generation = QosGeneration{2};
  request.qos.service_class = ServiceClass::kControlled;
  request.qos.priority = PriorityClass::kNormal;
  request.qos.required_residual_bytes = 10000;
  request.evidence.id = EvidenceId{13};
  request.evidence.generation = EvidenceGeneration{6};
  for (const CandidatePath& path : request.candidates.paths) {
    CongestionEvidenceEntry entry;
    entry.path = path.id;
    entry.utilization_ppb = static_cast<std::uint32_t>(rng->Below(1000000000u));
    request.evidence.entries.push_back(entry);
  }
  request.policy.id = PolicyId{17};
  request.policy.generation = PolicyGeneration{8};
  const ObjectiveKind kinds[] = {ObjectiveKind::kCost,      ObjectiveKind::kLatency,
                                 ObjectiveKind::kResidualCapacity, ObjectiveKind::kCongestionUtilization,
                                 ObjectiveKind::kHopCount,  ObjectiveKind::kTier,
                                 ObjectiveKind::kLocalityAffinity, ObjectiveKind::kIncumbentStability};
  const Direction directions[] = {Direction::kMinimize, Direction::kMinimize, Direction::kMaximize,
                                  Direction::kMinimize, Direction::kMinimize, Direction::kMinimize,
                                  Direction::kMinimize, Direction::kMinimize};
  const std::uint64_t objectives = std::max<std::uint64_t>(1, std::min<std::uint64_t>(8, config.objectives));
  for (std::uint64_t i = 0; i < objectives; ++i) {
    Objective objective;
    objective.kind = kinds[i];
    objective.direction = directions[i];
    request.policy.objectives.push_back(objective);
  }
  request.policy.churn.threshold_objective_index = 0;
  request.policy.revalidate_after_nanos = 1000000000ull;
  request.expected.path_authority = PathAuthorityGeneration{5};
  request.expected.candidate_set = CandidateSetGeneration{3};
  request.expected.capacity = CapacitySnapshotGeneration{4};
  request.expected.policy = PolicyGeneration{8};
  request.expected.qos = QosGeneration{2};
  request.expected.fabric_epoch = FabricEpoch{1};
  request.expected.evidence = EvidenceGeneration{6};
  request.provenance.producer = "flowplace-bench";
  request.provenance.producer_version = std::string(kVersionString);
  request.provenance.source_sequence = index;
  request.attempt = AttemptId{100000 + index};
  if (config.churn_percent != 0 && rng->Below(100) < config.churn_percent) {
    IncumbentPlacement incumbent;
    incumbent.id = PlacementId{500 + index};
    incumbent.generation = PlacementGeneration{1};
    incumbent.path = request.candidates.paths[rng->Below(fanout)].id;
    incumbent.path_authority = PathAuthorityGeneration{5};
    incumbent.candidate_set_generation = CandidateSetGeneration{3};
    incumbent.capacity_generation = CapacitySnapshotGeneration{4};
    incumbent.policy_generation = PolicyGeneration{8};
    incumbent.qos_generation = QosGeneration{2};
    incumbent.fabric_epoch = FabricEpoch{1};
    request.incumbent = incumbent;
    request.policy.churn.prefer_incumbent = true;
    request.policy.churn.move_improvement_threshold = 50;
  }
  return request;
}

std::uint64_t Percentile(std::vector<std::uint64_t>* samples, double fraction) {
  if (samples->empty()) return 0;
  std::sort(samples->begin(), samples->end());
  const double position = fraction * static_cast<double>(samples->size() - 1);
  const std::size_t index = static_cast<std::size_t>(position + 0.5);
  return (*samples)[std::min(index, samples->size() - 1)];
}

}  // namespace

BenchResult RunSyntheticPlacementBenchmark(const BenchConfig& config) {
  BenchResult result;
  result.config = config;
  const PlacementEngine engine(config.limits);

  // Geometric sweep over fan-out, ending at the configured value.
  std::vector<std::uint64_t> fanouts;
  const std::uint64_t target = std::max<std::uint64_t>(1, config.fanout);
  for (std::uint64_t value = 1; value < target; value *= 2) fanouts.push_back(value);
  fanouts.push_back(target);
  if (fanouts.size() > 6) {
    fanouts.erase(fanouts.begin(), fanouts.end() - 6);
    fanouts.front() = target / 32 == 0 ? 1 : target / 32;
  }

  for (const std::uint64_t fanout : fanouts) {
    BenchConfig point = config;
    point.fanout = fanout;
    SyntheticRng rng(config.seed ^ (fanout * 0x9E3779B97F4A7C15ull));
    BenchSample sample;
    sample.fanout = fanout;
    std::vector<std::uint64_t> durations;
    std::uint64_t digest_accumulator = 0;
    const std::uint64_t warmup = std::min(config.warmup, config.iterations);
    for (std::uint64_t i = 0; i < config.iterations + warmup; ++i) {
      PlacementRequest request = MakeRequest(point, i, &rng);
      const auto start = std::chrono::steady_clock::now();
      const PlacementDecision decision = engine.Place(request);
      const auto finish = std::chrono::steady_clock::now();
      if (i < warmup) continue;
      const auto nanos =
          std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
      durations.push_back(static_cast<std::uint64_t>(nanos));
      sample.total_nanos += static_cast<std::uint64_t>(nanos);
      sample.candidates_examined += decision.explanation.candidate_count;
      switch (decision.outcome) {
        case Outcome::kPlaced: ++sample.placed; break;
        case Outcome::kPlacedDegraded: ++sample.placed_degraded; break;
        case Outcome::kDeferred: ++sample.deferred; break;
        case Outcome::kNoLegalPath: ++sample.no_legal_path; break;
        case Outcome::kInsufficientCapacity: ++sample.insufficient_capacity; break;
        case Outcome::kPolicyRejected: ++sample.policy_rejected; break;
        case Outcome::kStaleInput: ++sample.stale_input; break;
        case Outcome::kConflictingInput: ++sample.conflicting_input; break;
      }
      digest_accumulator ^= decision.digest.lo;
    }
    sample.iterations = config.iterations;
    sample.p50_nanos = Percentile(&durations, 0.50);
    sample.p99_nanos = Percentile(&durations, 0.99);
    sample.max_nanos = durations.empty() ? 0 : durations.back();
    sample.decision_digest_accumulator = digest_accumulator;
    sample.placements_per_second =
        sample.total_nanos == 0
            ? 0.0
            : static_cast<double>(sample.iterations) * 1e9 / static_cast<double>(sample.total_nanos);
    result.samples.push_back(sample);
  }
  return result;
}

std::string RenderBenchResult(const BenchResult& result) {
  std::string out;
  out += "label: " + result.label + " (in-process generated populations; not a network measurement)\n";
  out += "config: fanout=" + std::to_string(result.config.fanout) +
         " resources=" + std::to_string(result.config.resources) +
         " domains=" + std::to_string(result.config.domains) +
         " churn_percent=" + std::to_string(result.config.churn_percent) +
         " objectives=" + std::to_string(result.config.objectives) +
         " iterations=" + std::to_string(result.config.iterations) +
         " seed=" + std::to_string(result.config.seed) + "\n";
  char buffer[512];
  for (const BenchSample& sample : result.samples) {
    std::snprintf(buffer, sizeof(buffer),
                  "fanout=%-7llu placed=%-6llu degraded=%-6llu no_legal=%-6llu insufficient=%-6llu "
                  "policy=%-6llu deferred=%-6llu stale=%-6llu conflicting=%-6llu "
                  "p50_ns=%-9llu p99_ns=%-9llu max_ns=%-9llu placements_per_second=%.1f\n",
                  static_cast<unsigned long long>(sample.fanout),
                  static_cast<unsigned long long>(sample.placed),
                  static_cast<unsigned long long>(sample.placed_degraded),
                  static_cast<unsigned long long>(sample.no_legal_path),
                  static_cast<unsigned long long>(sample.insufficient_capacity),
                  static_cast<unsigned long long>(sample.policy_rejected),
                  static_cast<unsigned long long>(sample.deferred),
                  static_cast<unsigned long long>(sample.stale_input),
                  static_cast<unsigned long long>(sample.conflicting_input),
                  static_cast<unsigned long long>(sample.p50_nanos),
                  static_cast<unsigned long long>(sample.p99_nanos),
                  static_cast<unsigned long long>(sample.max_nanos), sample.placements_per_second);
    out += buffer;
  }
  return out;
}

}  // namespace flowplace

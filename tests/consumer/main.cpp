// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Downstream consumer check. It exercises the installed public API end to end:
// a deterministic placement, a durable commit through the coordinator, a
// restart that requires revalidation, and a bounded explanation.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "flowplace/engine.hpp"
#include "flowplace/runtime.hpp"
#include "flowplace/store.hpp"
#include "flowplace/version.hpp"

using namespace flowplace;

namespace {

bool Check(bool condition, const char* what) {
  if (!condition) std::fprintf(stderr, "consumer check failed: %s\n", what);
  return condition;
}

PlacementRequest MakeRequest() {
  PlacementRequest request;
  request.flow = FlowId{77};
  request.flow_generation = FlowGeneration{4};
  request.candidates.id = CandidateSetId{11};
  request.candidates.generation = CandidateSetGeneration{2};
  request.capacity.id = CapacitySnapshotId{13};
  request.capacity.generation = CapacitySnapshotGeneration{3};
  request.qos.id = QosProfileId{17};
  request.qos.generation = QosGeneration{1};
  request.qos.service_class = ServiceClass::kControlled;
  request.qos.required_residual_bytes = 1000;
  request.policy.id = PolicyId{19};
  request.policy.generation = PolicyGeneration{5};
  Objective objective;
  objective.kind = ObjectiveKind::kCost;
  objective.direction = Direction::kMinimize;
  request.policy.objectives.push_back(objective);
  request.policy.churn.threshold_objective_index = 0;
  request.evidence.id = EvidenceId{23};
  request.evidence.generation = EvidenceGeneration{7};
  request.expected.path_authority = PathAuthorityGeneration{9};
  request.expected.candidate_set = CandidateSetGeneration{2};
  request.expected.capacity = CapacitySnapshotGeneration{3};
  request.expected.policy = PolicyGeneration{5};
  request.expected.qos = QosGeneration{1};
  request.expected.fabric_epoch = FabricEpoch{6};
  request.expected.evidence = EvidenceGeneration{7};
  request.provenance.producer = "flowplace_consumer";
  request.provenance.producer_version = "1.0.0";
  for (std::uint64_t i = 0; i < 4; ++i) {
    CandidatePath path;
    path.id = PathId{100 + i};
    path.authority = PathAuthorityGeneration{9};
    path.attributes.tier = PathTier::kStandard;
    path.attributes.locality = LocalityId{30 + i};
    path.attributes.failure_domain = FailureDomainId{40 + i};
    path.attributes.cost_micro = 500 - i * 10;
    path.attributes.latency_nanos = 2000 + i;
    path.attributes.hop_count = static_cast<std::uint16_t>(2 + i);
    request.candidates.paths.push_back(path);

    PathCapacity capacity;
    capacity.path = path.id;
    capacity.capacity_bytes = 100000;
    capacity.residual_bytes = 50000;
    request.capacity.entries.push_back(capacity);

    CongestionEvidenceEntry evidence;
    evidence.path = path.id;
    evidence.utilization_ppb = 100000000u * static_cast<std::uint32_t>(i + 1);
    request.evidence.entries.push_back(evidence);
  }
  return request;
}

}  // namespace

int main() {
  bool ok = true;
  ok = Check(std::string(kVersionString) == "1.0.0", "version string") && ok;

  const PlacementRequest request = MakeRequest();
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(request);
  ok = Check(decision.outcome == Outcome::kPlaced, "outcome is PLACED") && ok;
  ok = Check(decision.intent.has_value(), "intent is present") && ok;
  if (decision.intent.has_value()) {
    // The cheapest path is the highest index in this fixture.
    ok = Check(decision.intent->path == PathId{103}, "cheapest path chosen") && ok;
    ok = Check(decision.intent->path_authority == PathAuthorityGeneration{9},
               "chosen path carries the expected authority generation") && ok;
    std::string reason;
    ok = Check(engine.VerifyIntent(request, *decision.intent, &reason), "intent verifies") && ok;
  }
  ok = Check(engine.Place(request).digest == decision.digest, "placement is reproducible") && ok;
  const std::string rendered = RenderDecision(decision);
  ok = Check(rendered.find("outcome=PLACED") != std::string::npos, "rendered decision") && ok;

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "flowplace-consumer-store";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  const std::string store_path = (directory / "placements.log").string();
  {
    CoordinatorOptions options;
    options.store_path = store_path;
    options.initial_epoch = FabricEpoch{6};
    options.worker_threads = 2;
    Result<std::unique_ptr<PlacementCoordinator>> coordinator =
        PlacementCoordinator::Start(options);
    ok = Check(coordinator.ok(), "coordinator starts") && ok;
    if (coordinator.ok()) {
      Result<AttemptOutcome> outcome = coordinator.value()->PlaceNow(request, AttemptId{1});
      ok = Check(outcome.ok(), "synchronous placement") && ok;
      if (outcome.ok()) {
        ok = Check(outcome.value().state == AttemptState::kCommitted, "attempt committed") && ok;
        ok = Check(outcome.value().placement_generation == PlacementGeneration{1},
                   "first generation") &&
             ok;
      }
      ok = Check(coordinator.value()->Shutdown().ok(), "coordinator shuts down") && ok;
    }
  }
  {
    Result<PlacementStore> store = PlacementStore::Open(store_path, StoreOptions{});
    ok = Check(store.ok(), "store reopens") && ok;
    if (store.ok()) {
      const RecoveryReport& report = store.value().recovery();
      ok = Check(report.placements_total == 1, "one durable placement") && ok;
      ok = Check(!report.liveness_restored, "durable state restores no liveness") && ok;
      ok = Check(report.placements_requiring_revalidation == 1, "revalidation required") && ok;
      ok = Check(store.value().Close().ok(), "store closes") && ok;
    }
  }
  std::filesystem::remove_all(directory, error);
  std::printf("flowplace_consumer: %s\n", ok ? "OK" : "FAILED");
  return ok ? 0 : 1;
}

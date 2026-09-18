// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fixtures.hpp"
#include "harness.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "flowplace/process.hpp"
#include "flowplace/runtime.hpp"
#include "flowplace/store.hpp"

using namespace flowplace;
using namespace fptest;

namespace {

std::filesystem::path MakeTempDirectory(const std::string& name) {
  static int counter = 0;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("flowplace-runtime-" + name + "-" + std::to_string(CurrentProcessId()) + "-" +
       std::to_string(++counter));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  return directory;
}

struct TempDir {
  std::filesystem::path directory;
  explicit TempDir(const std::string& name) : directory(MakeTempDirectory(name)) {}
  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  [[nodiscard]] std::string File(const std::string& name) const {
    return (directory / name).string();
  }
};

CoordinatorOptions OptionsFor(const std::string& store_path, bool durable = true) {
  CoordinatorOptions options;
  options.store_path = durable ? store_path : std::string();
  options.worker_threads = 2;
  options.queue_capacity = 64;
  options.initial_epoch = FabricEpoch{kEpoch};
  options.incarnation = IncarnationId{4242};
  return options;
}

}  // namespace

FP_TEST(runtime, synchronous_placement_commits_durably) {
  TempDir dir("sync");
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(dir.File("placements.log")));
  FP_REQUIRE(coordinator.ok());
  const AttemptId attempt{1};
  Result<AttemptOutcome> outcome =
      coordinator.value()->PlaceNow(Baseline(3), attempt);
  FP_REQUIRE(outcome.ok());
  FP_CHECK_EQ(outcome.value().state, AttemptState::kCommitted);
  FP_CHECK_EQ(outcome.value().code, StatusCode::kOk);
  FP_CHECK_EQ(outcome.value().placement_id, PlacementId{1});
  FP_CHECK_EQ(outcome.value().placement_generation, PlacementGeneration{1});
  FP_CHECK_EQ(outcome.value().attempt, attempt);
  FP_CHECK(outcome.value().decision.placed());
  FP_CHECK_EQ(coordinator.value()->stats().placed, std::uint64_t{1});
  FP_CHECK(coordinator.value()->Shutdown().ok());
  FP_CHECK_EQ(coordinator.value()->stats().queued, std::uint64_t{0});
  FP_CHECK_EQ(coordinator.value()->stats().active, std::uint64_t{0});

  Result<PlacementStore> store = PlacementStore::Open(dir.File("placements.log"), StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().placements_total, std::uint64_t{1});
  FP_CHECK_EQ(store.value().recovery().committed_attempts, std::uint64_t{1});
  FP_CHECK_EQ(store.value().recovery().unfinished_attempts, std::uint64_t{0});
}

FP_TEST(runtime, epoch_advance_between_prepare_and_commit_fences_the_attempt) {
  TempDir dir("epoch");
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(dir.File("placements.log")));
  FP_REQUIRE(coordinator.ok());
  const AttemptId attempt{2};
  Result<PreparedAttempt> prepared = coordinator.value()->Prepare(Baseline(3), attempt);
  FP_REQUIRE(prepared.ok());
  FP_CHECK(prepared.value().decision.placed());
  FP_CHECK(coordinator.value()->AdvanceEpoch().ok());
  Result<AttemptOutcome> outcome = coordinator.value()->Commit(std::move(prepared.value()));
  FP_REQUIRE(outcome.ok());
  FP_CHECK_EQ(outcome.value().state, AttemptState::kFenced);
  FP_CHECK_EQ(outcome.value().code, StatusCode::kFencedStaleEpoch);
  FP_CHECK_EQ(outcome.value().placement_id, PlacementId{});
  FP_CHECK(coordinator.value()->Shutdown().ok());

  Result<PlacementStore> store = PlacementStore::Open(dir.File("placements.log"), StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().placements_total, std::uint64_t{0});
  FP_CHECK_EQ(store.value().recovery().fenced_attempts, std::uint64_t{1});
  FP_CHECK_EQ(store.value().recovery().unfinished_attempts, std::uint64_t{0});
}

FP_TEST(runtime, authority_change_between_prepare_and_commit_fences_the_attempt) {
  TempDir dir("authority");
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(dir.File("placements.log")));
  FP_REQUIRE(coordinator.ok());
  Result<PreparedAttempt> prepared = coordinator.value()->Prepare(Baseline(3), AttemptId{3});
  FP_REQUIRE(prepared.ok());
  AuthorityExpectation authority = coordinator.value()->authority();
  authority.policy = PolicyGeneration{kPolicyGeneration + 1};
  authority.capacity = CapacitySnapshotGeneration{kCapacityGeneration + 1};
  FP_CHECK(coordinator.value()->SetAuthority(authority).ok());
  Result<AttemptOutcome> outcome = coordinator.value()->Commit(std::move(prepared.value()));
  FP_REQUIRE(outcome.ok());
  FP_CHECK_EQ(outcome.value().state, AttemptState::kFenced);
  FP_CHECK_EQ(outcome.value().code, StatusCode::kStaleAuthorityGeneration);
  FP_CHECK(coordinator.value()->Shutdown().ok());

  Result<PlacementStore> store = PlacementStore::Open(dir.File("placements.log"), StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().placements_total, std::uint64_t{0});
}

FP_TEST(runtime, superseded_epoch_is_refused_at_submission) {
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor("", false));
  FP_REQUIRE(coordinator.ok());
  FP_CHECK(coordinator.value()->AdvanceEpoch().ok());
  const Status status = coordinator.value()->Submit(Baseline(2), AttemptId{4});
  FP_CHECK(!status.ok());
  FP_CHECK_EQ(status.code(), StatusCode::kStaleFabricEpoch);
  FP_CHECK_EQ(coordinator.value()->stats().accepted, std::uint64_t{0});
  FP_CHECK(coordinator.value()->Shutdown().ok());
}

FP_TEST(runtime, cancellation_before_the_commit_boundary_prevents_mutation) {
  TempDir dir("cancel");
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(dir.File("placements.log")));
  FP_REQUIRE(coordinator.ok());
  const AttemptId attempt{5};
  Result<PreparedAttempt> prepared = coordinator.value()->Prepare(Baseline(3), attempt);
  FP_REQUIRE(prepared.ok());
  FP_CHECK(coordinator.value()->Cancel(attempt).ok());
  Result<AttemptOutcome> outcome = coordinator.value()->Commit(std::move(prepared.value()));
  FP_REQUIRE(outcome.ok());
  FP_CHECK_EQ(outcome.value().state, AttemptState::kCancelled);
  FP_CHECK_EQ(outcome.value().code, StatusCode::kCancelled);
  FP_CHECK(coordinator.value()->Shutdown().ok());

  Result<PlacementStore> store = PlacementStore::Open(dir.File("placements.log"), StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().placements_total, std::uint64_t{0});
  FP_CHECK_EQ(store.value().recovery().cancelled_attempts, std::uint64_t{1});
}

FP_TEST(runtime, cancellation_after_the_commit_boundary_does_not_undo_the_placement) {
  TempDir dir("late-cancel");
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(dir.File("placements.log")));
  FP_REQUIRE(coordinator.ok());
  const AttemptId attempt{6};
  Result<AttemptOutcome> outcome = coordinator.value()->PlaceNow(Baseline(3), attempt);
  FP_REQUIRE(outcome.ok());
  FP_CHECK_EQ(outcome.value().state, AttemptState::kCommitted);
  const Status late = coordinator.value()->Cancel(attempt);
  FP_CHECK(!late.ok());
  FP_CHECK_EQ(late.code(), StatusCode::kAlreadyCommitted);
  Result<AttemptOutcome> awaited = coordinator.value()->Await(attempt);
  FP_REQUIRE(awaited.ok());
  FP_CHECK_EQ(awaited.value().state, AttemptState::kCommitted);
  FP_CHECK(coordinator.value()->Shutdown().ok());

  Result<PlacementStore> store = PlacementStore::Open(dir.File("placements.log"), StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().placements_total, std::uint64_t{1});
}

FP_TEST(runtime, duplicate_attempts_are_idempotent_or_conflicting) {
  TempDir dir("duplicate");
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(dir.File("placements.log")));
  FP_REQUIRE(coordinator.ok());
  const AttemptId attempt{7};
  FP_CHECK(coordinator.value()->Submit(Baseline(3), attempt).ok());
  const Status replay = coordinator.value()->Submit(Baseline(3), attempt);
  FP_CHECK(!replay.ok());
  FP_CHECK_EQ(replay.code(), StatusCode::kAlreadyExists);
  PlacementRequest different = Baseline(3);
  different.qos.required_residual_bytes = 400;
  const Status conflict = coordinator.value()->Submit(different, attempt);
  FP_CHECK(!conflict.ok());
  FP_CHECK_EQ(conflict.code(), StatusCode::kDuplicateAttemptConflict);
  Result<AttemptOutcome> outcome = coordinator.value()->Await(attempt);
  FP_REQUIRE(outcome.ok());
  FP_CHECK_EQ(outcome.value().state, AttemptState::kCommitted);
  const CoordinatorStats stats = coordinator.value()->stats();
  FP_CHECK_EQ(stats.duplicate_replays, std::uint64_t{1});
  FP_CHECK_EQ(stats.duplicate_conflicts, std::uint64_t{1});
  FP_CHECK(coordinator.value()->Shutdown().ok());
}

FP_TEST(runtime, malformed_requests_are_refused_before_any_durable_work) {
  TempDir dir("malformed");
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(dir.File("placements.log")));
  FP_REQUIRE(coordinator.ok());
  PlacementRequest broken = Baseline(2);
  broken.candidates.paths[1].id = broken.candidates.paths[0].id;
  const Status status = coordinator.value()->Submit(broken, AttemptId{8});
  FP_CHECK(!status.ok());
  FP_CHECK_EQ(status.code(), StatusCode::kDuplicatePathId);
  FP_CHECK_EQ(coordinator.value()->stats().accepted, std::uint64_t{0});
  FP_CHECK(coordinator.value()->Shutdown().ok());
  Result<PlacementStore> store = PlacementStore::Open(dir.File("placements.log"), StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().attempts_total, std::uint64_t{0});
}

FP_TEST(runtime, asynchronous_work_reaches_terminal_states_and_drains) {
  TempDir dir("async");
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(dir.File("placements.log")));
  FP_REQUIRE(coordinator.ok());
  constexpr std::uint64_t kAttempts = 24;
  for (std::uint64_t i = 0; i < kAttempts; ++i) {
    PlacementRequest request = Baseline(4);
    request.flow = FlowId{1000 + i};
    request.flow_generation = FlowGeneration{i + 1};
    FP_REQUIRE(coordinator.value()->Submit(request, AttemptId{100 + i}).ok());
  }
  FP_CHECK(coordinator.value()->Drain().ok());
  std::uint64_t committed = 0;
  for (std::uint64_t i = 0; i < kAttempts; ++i) {
    Result<AttemptOutcome> outcome = coordinator.value()->Await(AttemptId{100 + i});
    FP_REQUIRE(outcome.ok());
    FP_CHECK(outcome.value().state == AttemptState::kCommitted);
    if (outcome.value().state == AttemptState::kCommitted) ++committed;
  }
  FP_CHECK_EQ(committed, kAttempts);
  const CoordinatorStats stats = coordinator.value()->stats();
  FP_CHECK_EQ(stats.queued, std::uint64_t{0});
  FP_CHECK_EQ(stats.active, std::uint64_t{0});
  FP_CHECK_EQ(stats.completed, kAttempts);
  FP_CHECK(coordinator.value()->Shutdown().ok());
  Result<PlacementStore> store = PlacementStore::Open(dir.File("placements.log"), StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().placements_total, kAttempts);
  FP_CHECK_EQ(store.value().recovery().committed_attempts, kAttempts);
  FP_CHECK_EQ(store.value().recovery().unfinished_attempts, std::uint64_t{0});
}

FP_TEST(runtime, await_reports_unknown_attempts) {
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor("", false));
  FP_REQUIRE(coordinator.ok());
  Result<AttemptOutcome> outcome = coordinator.value()->Await(AttemptId{999});
  FP_CHECK(!outcome.ok());
  FP_CHECK_EQ(outcome.status().code(), StatusCode::kUnknownAttempt);
  FP_CHECK_EQ(coordinator.value()->Cancel(AttemptId{999}).code(), StatusCode::kUnknownAttempt);
  FP_CHECK(coordinator.value()->Shutdown().ok());
}

FP_TEST(runtime, shutdown_rejects_new_work_and_returns_accounting_to_baseline) {
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor("", false));
  FP_REQUIRE(coordinator.ok());
  FP_CHECK(coordinator.value()->Shutdown().ok());
  const Status status = coordinator.value()->Submit(Baseline(2), AttemptId{9});
  FP_CHECK(!status.ok());
  FP_CHECK_EQ(status.code(), StatusCode::kShuttingDown);
  const CoordinatorStats stats = coordinator.value()->stats();
  FP_CHECK_EQ(stats.queued, std::uint64_t{0});
  FP_CHECK_EQ(stats.active, std::uint64_t{0});
  FP_CHECK(!coordinator.value()->running());
  FP_CHECK(coordinator.value()->Shutdown().ok());  // idempotent
}

FP_TEST(runtime, shutdown_cancelling_never_leaves_a_false_success) {
  TempDir dir("shutdown");
  CoordinatorOptions options = OptionsFor(dir.File("placements.log"));
  options.worker_threads = 1;
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(options);
  FP_REQUIRE(coordinator.ok());
  constexpr std::uint64_t kAttempts = 16;
  std::uint64_t accepted = 0;
  for (std::uint64_t i = 0; i < kAttempts; ++i) {
    PlacementRequest request = Baseline(i % 4 == 0 ? 4000 : 4);
    request.flow = FlowId{2000 + i};
    request.flow_generation = FlowGeneration{i + 1};
    const Status status = coordinator.value()->Submit(request, AttemptId{200 + i});
    if (status.ok()) {
      ++accepted;
    } else {
      FP_CHECK_EQ(status.code(), StatusCode::kShuttingDown);
    }
  }
  FP_CHECK(coordinator.value()->ShutdownCancelling().ok());
  std::uint64_t committed = 0;
  for (std::uint64_t i = 0; i < kAttempts; ++i) {
    Result<AttemptOutcome> outcome = coordinator.value()->Await(AttemptId{200 + i});
    if (!outcome.ok()) continue;
    FP_CHECK(outcome.value().state != AttemptState::kRunning);
    FP_CHECK(outcome.value().state != AttemptState::kQueued);
    if (outcome.value().state == AttemptState::kCommitted) ++committed;
    if (outcome.value().state == AttemptState::kCancelled) {
      FP_CHECK_EQ(outcome.value().placement_id, PlacementId{});
    }
  }
  FP_CHECK(accepted > 0);
  const CoordinatorStats stats = coordinator.value()->stats();
  FP_CHECK_EQ(stats.queued, std::uint64_t{0});
  FP_CHECK_EQ(stats.active, std::uint64_t{0});

  Result<PlacementStore> store = PlacementStore::Open(dir.File("placements.log"), StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().placements_total, committed);
}

FP_TEST(runtime, restart_requires_revalidation_and_revalidate_verdicts_are_exact) {
  TempDir dir("restart");
  const std::string store_path = dir.File("placements.log");
  {
    Result<std::unique_ptr<PlacementCoordinator>> coordinator =
        PlacementCoordinator::Start(OptionsFor(store_path));
    FP_REQUIRE(coordinator.ok());
    Result<AttemptOutcome> outcome = coordinator.value()->PlaceNow(Baseline(3), AttemptId{11});
    FP_REQUIRE(outcome.ok());
    FP_CHECK_EQ(outcome.value().state, AttemptState::kCommitted);
    FP_CHECK(coordinator.value()->Shutdown().ok());
  }
  CoordinatorOptions options = OptionsFor(store_path);
  options.incarnation = IncarnationId{7777};
  Result<std::unique_ptr<PlacementCoordinator>> restarted =
      PlacementCoordinator::Start(options);
  FP_REQUIRE(restarted.ok());
  const RecoveryReport report = restarted.value()->recovery();
  FP_CHECK_EQ(report.placements_total, std::uint64_t{1});
  FP_CHECK_EQ(report.placements_requiring_revalidation, std::uint64_t{1});
  FP_CHECK(!report.liveness_restored);
  FP_CHECK_EQ(report.last_incarnation, IncarnationId{4242});
  FP_CHECK_EQ(restarted.value()->incarnation(), IncarnationId{7777});

  Result<RevalidationResult> unchanged = restarted.value()->Revalidate(Baseline(3));
  FP_REQUIRE(unchanged.ok());
  FP_CHECK_EQ(unchanged.value().verdict, RevalidationVerdict::kStillCurrent);
  FP_CHECK(unchanged.value().previous.has_value());

  PlacementRequest moved = Baseline(3);
  moved.policy.generation = PolicyGeneration{kPolicyGeneration + 1};
  moved.expected.policy = PolicyGeneration{kPolicyGeneration + 1};
  moved.candidates.paths[0].attributes.tier = PathTier::kEconomy;
  moved.policy.allowed_tiers = {PathTier::kStandard};
  Result<RevalidationResult> must_move = restarted.value()->Revalidate(moved);
  FP_REQUIRE(must_move.ok());
  FP_CHECK_EQ(must_move.value().verdict, RevalidationVerdict::kMustMove);

  PlacementRequest impossible = Baseline(3);
  for (PathCapacity& capacity : impossible.capacity.entries) capacity.residual_bytes = 1;
  Result<RevalidationResult> rejected = restarted.value()->Revalidate(impossible);
  FP_REQUIRE(rejected.ok());
  FP_CHECK_EQ(rejected.value().verdict, RevalidationVerdict::kRejected);

  PlacementRequest unknown = Baseline(3);
  unknown.flow = FlowId{999999};
  Result<RevalidationResult> missing = restarted.value()->Revalidate(unknown);
  FP_REQUIRE(missing.ok());
  FP_CHECK_EQ(missing.value().verdict, RevalidationVerdict::kUnknownFlow);
  FP_CHECK(restarted.value()->Shutdown().ok());
}

FP_TEST(runtime, prepare_after_a_terminal_state_is_refused) {
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor("", false));
  FP_REQUIRE(coordinator.ok());
  const AttemptId attempt{21};
  Result<AttemptOutcome> outcome = coordinator.value()->PlaceNow(Baseline(3), attempt);
  FP_REQUIRE(outcome.ok());
  FP_CHECK_EQ(outcome.value().state, AttemptState::kCommitted);
  // A replay of an attempt that already reached a terminal state is refused as
  // an idempotent replay; the outcome itself is available through Await.
  const Result<PreparedAttempt> again = coordinator.value()->Prepare(Baseline(3), attempt);
  FP_CHECK(!again.ok());
  FP_CHECK_EQ(again.status().code(), StatusCode::kAlreadyExists);
  Result<AttemptOutcome> replay = coordinator.value()->Await(attempt);
  FP_REQUIRE(replay.ok());
  FP_CHECK_EQ(replay.value().state, AttemptState::kCommitted);
  FP_CHECK(coordinator.value()->Shutdown().ok());
}

FP_TEST(runtime, await_on_a_prepared_attempt_terminates_after_shutdown) {
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor("", false));
  FP_REQUIRE(coordinator.ok());
  const AttemptId attempt{22};
  Result<PreparedAttempt> prepared = coordinator.value()->Prepare(Baseline(3), attempt);
  FP_REQUIRE(prepared.ok());
  FP_CHECK(coordinator.value()->Shutdown().ok());
  Result<AttemptOutcome> outcome = coordinator.value()->Await(attempt);
  FP_REQUIRE(outcome.ok());
  FP_CHECK(outcome.value().state != AttemptState::kRunning);
  FP_CHECK(outcome.value().state != AttemptState::kQueued);
  FP_CHECK_EQ(outcome.value().placement_id, PlacementId{});
}

FP_TEST(runtime, concurrent_shutdown_is_safe) {
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor("", false));
  FP_REQUIRE(coordinator.ok());
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&coordinator, &failures]() {
      if (!coordinator.value()->Shutdown().ok()) failures.fetch_add(1);
    });
  }
  for (std::thread& thread : threads) thread.join();
  FP_CHECK_EQ(failures.load(), 0);
  const CoordinatorStats stats = coordinator.value()->stats();
  FP_CHECK_EQ(stats.queued, std::uint64_t{0});
  FP_CHECK_EQ(stats.active, std::uint64_t{0});
  FP_CHECK(coordinator.value()->Shutdown().ok());
}

FP_TEST(runtime, retained_attempts_are_bounded) {
  CoordinatorOptions options = OptionsFor("", false);
  options.worker_threads = 2;
  options.queue_capacity = 256;
  options.max_retained_attempts = 16;
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(options);
  FP_REQUIRE(coordinator.ok());
  constexpr std::uint64_t kAttempts = 64;
  for (std::uint64_t i = 0; i < kAttempts; ++i) {
    PlacementRequest request = Baseline(2);
    request.flow = FlowId{7000 + i};
    request.flow_generation = FlowGeneration{i + 1};
    FP_REQUIRE(coordinator.value()->Submit(request, AttemptId{400 + i}).ok());
    // Draining between submissions makes every earlier attempt terminal before
    // the next registration runs its bounded reaping pass, so the assertion
    // below does not depend on scheduling.
    FP_REQUIRE(coordinator.value()->Drain().ok());
  }
  // The most recent attempts still report their outcome; the oldest were reaped
  // and are reported as unknown rather than silently replayed.
  Result<AttemptOutcome> recent = coordinator.value()->Await(AttemptId{400 + kAttempts - 1});
  FP_CHECK(recent.ok());
  Result<AttemptOutcome> oldest = coordinator.value()->Await(AttemptId{400});
  FP_CHECK(!oldest.ok());
  FP_CHECK_EQ(oldest.status().code(), StatusCode::kUnknownAttempt);
  FP_CHECK(coordinator.value()->Shutdown().ok());
}

FP_TEST(runtime, shutdown_never_reports_cancelled_for_a_crossed_boundary) {
  TempDir dir("crossed");
  CoordinatorOptions options = OptionsFor(dir.File("placements.log"));
  options.worker_threads = 2;
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(options);
  FP_REQUIRE(coordinator.ok());
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> placed{0};
  std::thread placer([&coordinator, &stop, &placed]() {
    std::uint64_t index = 0;
    while (!stop.load()) {
      PlacementRequest request = Baseline(2);
      request.flow = FlowId{9000 + index};
      request.flow_generation = FlowGeneration{index + 1};
      Result<AttemptOutcome> outcome =
          coordinator.value()->PlaceNow(request, AttemptId{5000 + index});
      if (outcome.ok() && outcome.value().state == AttemptState::kCommitted) {
        placed.fetch_add(1);
      }
      ++index;
      if (index > 400) break;
    }
  });
  std::this_thread::yield();
  FP_CHECK(coordinator.value()->ShutdownCancelling().ok());
  stop.store(true);
  placer.join();
  FP_CHECK(coordinator.value()->Shutdown().ok());

  Result<PlacementStore> store = PlacementStore::Open(dir.File("placements.log"), StoreOptions{});
  FP_REQUIRE(store.ok());
  const std::uint64_t durable = store.value().recovery().placements_total;
  FP_CHECK_EQ(durable, placed.load());
}

FP_TEST(runtime, committed_attempts_always_have_durable_records) {
  TempDir dir("fidelity");
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(dir.File("placements.log")));
  FP_REQUIRE(coordinator.ok());
  std::vector<AttemptId> attempts;
  for (std::uint64_t i = 0; i < 12; ++i) {
    PlacementRequest request = Baseline(1 + (i % 5));
    request.flow = FlowId{3000 + i};
    request.flow_generation = FlowGeneration{i + 1};
    const AttemptId attempt{300 + i};
    FP_REQUIRE(coordinator.value()->Submit(request, attempt).ok());
    attempts.push_back(attempt);
  }
  FP_CHECK(coordinator.value()->Drain().ok());
  std::uint64_t committed = 0;
  for (const AttemptId attempt : attempts) {
    Result<AttemptOutcome> outcome = coordinator.value()->Await(attempt);
    FP_REQUIRE(outcome.ok());
    if (outcome.value().state == AttemptState::kCommitted) {
      ++committed;
      FP_CHECK(outcome.value().decision.placed());
      FP_CHECK(outcome.value().placement_id.valid());
    }
  }
  Result<std::vector<PlacementRecord>> placements = coordinator.value()->LatestPlacements();
  FP_REQUIRE(placements.ok());
  FP_CHECK_EQ(placements.value().size(), static_cast<std::size_t>(committed));
  FP_CHECK(coordinator.value()->Shutdown().ok());
}

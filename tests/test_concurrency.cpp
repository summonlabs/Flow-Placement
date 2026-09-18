// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fixtures.hpp"
#include "harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
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
  static std::atomic<int> counter{0};
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("flowplace-concurrency-" + name + "-" + std::to_string(CurrentProcessId()) + "-" +
       std::to_string(counter.fetch_add(1)));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  return directory;
}

CoordinatorOptions OptionsFor(const std::string& store_path) {
  CoordinatorOptions options;
  options.store_path = store_path;
  options.worker_threads = 4;
  options.queue_capacity = 4096;
  options.initial_epoch = FabricEpoch{kEpoch};
  options.store.fsync_on_append = true;
  return options;
}

}  // namespace

FP_TEST(concurrency, many_threads_place_and_accounting_is_exact) {
  const std::filesystem::path directory = MakeTempDirectory("many");
  const std::string store_path = (directory / "placements.log").string();
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(store_path));
  FP_REQUIRE(coordinator.ok());
  constexpr std::uint64_t kThreads = 6;
  constexpr std::uint64_t kPerThread = 25;
  std::atomic<std::uint64_t> accepted{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::uint64_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([&coordinator, &accepted, t]() {
      for (std::uint64_t i = 0; i < kPerThread; ++i) {
        PlacementRequest request = Baseline(2 + (i % 5));
        request.flow = FlowId{10000 + t * 1000 + i};
        request.flow_generation = FlowGeneration{i + 1};
        const Status status =
            coordinator.value()->Submit(request, AttemptId{50000 + t * 1000 + i});
        if (status.ok()) accepted.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  FP_CHECK(coordinator.value()->Drain().ok());
  std::uint64_t committed = 0;
  for (std::uint64_t t = 0; t < kThreads; ++t) {
    for (std::uint64_t i = 0; i < kPerThread; ++i) {
      Result<AttemptOutcome> outcome =
          coordinator.value()->Await(AttemptId{50000 + t * 1000 + i});
      if (!outcome.ok()) continue;
      if (outcome.value().state == AttemptState::kCommitted) ++committed;
    }
  }
  FP_CHECK_EQ(committed, accepted.load());
  const CoordinatorStats stats = coordinator.value()->stats();
  FP_CHECK_EQ(stats.queued, std::uint64_t{0});
  FP_CHECK_EQ(stats.active, std::uint64_t{0});
  FP_CHECK_EQ(stats.placed, committed);
  Result<std::vector<PlacementRecord>> placements = coordinator.value()->LatestPlacements();
  FP_REQUIRE(placements.ok());
  FP_CHECK_EQ(placements.value().size(), static_cast<std::size_t>(committed));
  FP_CHECK(coordinator.value()->Shutdown().ok());

  Result<PlacementStore> store = PlacementStore::Open(store_path, StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().placements_total, committed);
  FP_CHECK_EQ(store.value().recovery().unfinished_attempts, std::uint64_t{0});
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

FP_TEST(concurrency, concurrent_placement_of_one_request_is_identical) {
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(""));
  FP_REQUIRE(coordinator.ok());
  const PlacementRequest request = Baseline(6);
  const PlacementEngine engine;
  const PlacementDecision reference = engine.Place(request);
  FP_REQUIRE(reference.intent.has_value());
  // The attempt identity is part of a placement intent, so decisions made for
  // different attempts are compared after normalising that field: everything
  // the placement decided must be identical.
  const auto Normalize = [](const PlacementIntent& intent) {
    PlacementIntent copy = intent;
    copy.attempt = AttemptId{};
    copy.digest = IntentDigest(copy);
    return copy;
  };
  const PlacementIntent expected = Normalize(*reference.intent);
  std::vector<std::thread> threads;
  std::atomic<std::uint64_t> mismatches{0};
  for (std::uint64_t t = 0; t < 8; ++t) {
    threads.emplace_back([&coordinator, &request, &expected, &Normalize, &mismatches, t]() {
      for (std::uint64_t i = 0; i < 15; ++i) {
        const AttemptId attempt{70000 + t * 100 + i};
        Result<AttemptOutcome> outcome = coordinator.value()->PlaceNow(request, attempt);
        if (!outcome.ok() || !outcome.value().decision.intent.has_value()) {
          mismatches.fetch_add(1);
          continue;
        }
        const PlacementIntent observed = Normalize(*outcome.value().decision.intent);
        if (!(observed == expected)) mismatches.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  FP_CHECK_EQ(mismatches.load(), std::uint64_t{0});
  FP_CHECK(coordinator.value()->Shutdown().ok());
}

FP_TEST(concurrency, cancellation_under_load_never_commits_cancelled_work) {
  const std::filesystem::path directory = MakeTempDirectory("cancel");
  const std::string store_path = (directory / "placements.log").string();
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(store_path));
  FP_REQUIRE(coordinator.ok());
  constexpr std::uint64_t kAttempts = 120;
  std::vector<std::thread> submitters;
  for (std::uint64_t t = 0; t < 3; ++t) {
    submitters.emplace_back([&coordinator, t]() {
      for (std::uint64_t i = t; i < kAttempts; i += 3) {
        PlacementRequest request = Baseline(8 + (i % 40));
        request.flow = FlowId{20000 + i};
        request.flow_generation = FlowGeneration{i + 1};
        static_cast<void>(coordinator.value()->Submit(request, AttemptId{80000 + i}));
      }
    });
  }
  std::thread canceller([&coordinator]() {
    for (std::uint64_t i = 0; i < kAttempts; ++i) {
      static_cast<void>(coordinator.value()->Cancel(AttemptId{80000 + i}));
    }
  });
  for (std::thread& thread : submitters) thread.join();
  canceller.join();
  FP_CHECK(coordinator.value()->Drain().ok());

  std::uint64_t committed = 0;
  std::uint64_t cancelled = 0;
  for (std::uint64_t i = 0; i < kAttempts; ++i) {
    Result<AttemptOutcome> outcome = coordinator.value()->Await(AttemptId{80000 + i});
    if (!outcome.ok()) continue;
    if (outcome.value().state == AttemptState::kCommitted) {
      ++committed;
      FP_CHECK(outcome.value().placement_id.valid());
    } else if (outcome.value().state == AttemptState::kCancelled) {
      ++cancelled;
      // A cancelled attempt must not carry a placement identity.
      FP_CHECK_EQ(outcome.value().placement_id, PlacementId{});
    }
  }
  FP_CHECK_EQ(committed + cancelled <= kAttempts, true);
  FP_CHECK(coordinator.value()->Shutdown().ok());

  Result<PlacementStore> store = PlacementStore::Open(store_path, StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().placements_total, committed);
  FP_CHECK_EQ(store.value().recovery().cancelled_attempts, cancelled);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

FP_TEST(concurrency, epoch_advance_under_load_keeps_state_consistent) {
  const std::filesystem::path directory = MakeTempDirectory("epoch");
  const std::string store_path = (directory / "placements.log").string();
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(OptionsFor(store_path));
  FP_REQUIRE(coordinator.ok());
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> advances{0};
  std::thread advancer([&coordinator, &stop, &advances]() {
    while (!stop.load()) {
      if (coordinator.value()->AdvanceEpoch().ok()) advances.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  constexpr std::uint64_t kAttempts = 200;
  std::uint64_t submitted = 0;
  for (std::uint64_t i = 0; i < kAttempts; ++i) {
    // Every request binds to whatever epoch is current when it is submitted.
    PlacementRequest request = Baseline(3 + (i % 6));
    request.flow = FlowId{30000 + i};
    request.flow_generation = FlowGeneration{i + 1};
    request.expected.fabric_epoch = coordinator.value()->epoch();
    const Status status = coordinator.value()->Submit(request, AttemptId{90000 + i});
    if (status.ok()) {
      ++submitted;
    } else {
      FP_CHECK(status.code() == StatusCode::kStaleFabricEpoch ||
               status.code() == StatusCode::kShuttingDown);
    }
  }
  stop.store(true);
  advancer.join();
  FP_CHECK(advances.load() > 0);
  const Status drained = coordinator.value()->Drain();
  FP_CHECK(drained.ok());

  std::uint64_t committed = 0;
  std::uint64_t fenced = 0;
  for (std::uint64_t i = 0; i < kAttempts; ++i) {
    Result<AttemptOutcome> outcome = coordinator.value()->Await(AttemptId{90000 + i});
    if (!outcome.ok()) continue;
    if (outcome.value().state == AttemptState::kCommitted) ++committed;
    if (outcome.value().state == AttemptState::kFenced) ++fenced;
  }
  (void)fenced;
  FP_CHECK(coordinator.value()->Shutdown().ok());
  Result<PlacementStore> store = PlacementStore::Open(store_path, StoreOptions{});
  FP_REQUIRE(store.ok());
  // Every committed attempt is durable, and no fenced or cancelled attempt is.
  FP_CHECK_EQ(store.value().recovery().placements_total, committed);
  FP_CHECK(submitted > 0);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

FP_TEST(concurrency, shutdown_during_load_is_safe_and_bounded) {
  const std::filesystem::path directory = MakeTempDirectory("shutdown");
  const std::string store_path = (directory / "placements.log").string();
  CoordinatorOptions options = OptionsFor(store_path);
  options.worker_threads = 3;
  options.queue_capacity = 32;
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(options);
  FP_REQUIRE(coordinator.ok());
  std::atomic<std::uint64_t> unexpected_failures{0};
  std::thread submitter([&coordinator, &unexpected_failures]() {
    for (std::uint64_t i = 0; i < 400; ++i) {
      PlacementRequest request = Baseline(20);
      request.flow = FlowId{40000 + i};
      request.flow_generation = FlowGeneration{i + 1};
      const Status status = coordinator.value()->Submit(request, AttemptId{95000 + i});
      if (!status.ok() && status.code() != StatusCode::kShuttingDown &&
          status.code() != StatusCode::kQueueFull) {
        unexpected_failures.fetch_add(1);
      }
    }
  });
  std::this_thread::yield();
  FP_CHECK(coordinator.value()->ShutdownCancelling().ok());
  submitter.join();
  FP_CHECK_EQ(unexpected_failures.load(), std::uint64_t{0});
  const CoordinatorStats stats = coordinator.value()->stats();
  FP_CHECK_EQ(stats.queued, std::uint64_t{0});
  FP_CHECK_EQ(stats.active, std::uint64_t{0});
  std::uint64_t committed = 0;
  for (std::uint64_t i = 0; i < 400; ++i) {
    Result<AttemptOutcome> outcome = coordinator.value()->Await(AttemptId{95000 + i});
    if (outcome.ok() && outcome.value().state == AttemptState::kCommitted) ++committed;
  }
  Result<PlacementStore> store = PlacementStore::Open(store_path, StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().placements_total, committed);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

FP_TEST(concurrency, bounded_queue_refuses_excess_work) {
  CoordinatorOptions options;
  options.worker_threads = 1;
  options.queue_capacity = 1;
  options.initial_epoch = FabricEpoch{kEpoch};
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(options);
  FP_REQUIRE(coordinator.ok());
  std::uint64_t queue_full = 0;
  for (std::uint64_t i = 0; i < 32; ++i) {
    PlacementRequest request = Baseline(20000);
    request.flow = FlowId{50000 + i};
    request.flow_generation = FlowGeneration{i + 1};
    const Status status = coordinator.value()->Submit(request, AttemptId{96000 + i});
    if (status.code() == StatusCode::kQueueFull) {
      ++queue_full;
      continue;
    }
    FP_CHECK(status.ok());
  }
  FP_CHECK(queue_full > 0);
  FP_CHECK_EQ(coordinator.value()->stats().rejected_queue_full, queue_full);
  FP_CHECK(coordinator.value()->ShutdownCancelling().ok());
}

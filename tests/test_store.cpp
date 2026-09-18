// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fixtures.hpp"
#include "harness.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "flowplace/codec.hpp"
#include "flowplace/process.hpp"
#include "flowplace/store.hpp"

using namespace flowplace;
using namespace fptest;

namespace {

std::filesystem::path MakeTempDirectory(const std::string& name) {
  static int counter = 0;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("flowplace-store-" + name + "-" + std::to_string(CurrentProcessId()) + "-" +
       std::to_string(++counter));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  return directory;
}

struct TempStore {
  std::filesystem::path directory;
  std::string path;

  explicit TempStore(const std::string& name)
      : directory(MakeTempDirectory(name)), path((directory / "placements.log").string()) {}

  ~TempStore() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};

PlacementIntent IntentFor(const PlacementRequest& request, const PlacementEngine& engine) {
  const PlacementDecision decision = engine.Place(request);
  return decision.intent.has_value() ? *decision.intent : PlacementIntent{};
}

PlacementRequest FlowRequest(std::uint64_t flow, std::uint64_t path_count = 3) {
  PlacementRequest request = Baseline(path_count);
  request.flow = FlowId{flow};
  request.flow_generation = FlowGeneration{flow};
  request.candidates.id = CandidateSetId{flow * 10};
  return request;
}

std::uint64_t FileSize(const std::string& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

}  // namespace

FP_TEST(store, commits_are_durable_and_recovered) {
  TempStore temp("recover");
  const PlacementEngine engine;
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    FP_CHECK(store.value().recovery().placements_total == 0);
    for (std::uint64_t flow = 1; flow <= 3; ++flow) {
      const PlacementRequest request = FlowRequest(flow);
      const PlacementIntent intent = IntentFor(request, engine);
      FP_REQUIRE(intent.path.valid());
      Result<PlacementRecord> record = store.value().CommitPlacement(
          intent, IncumbentDelta::kNoIncumbent, Digest{flow, flow}, IncarnationId{5}, FabricEpoch{2});
      FP_REQUIRE(record.ok());
      FP_CHECK_EQ(record.value().id, PlacementId{flow});
      FP_CHECK_EQ(record.value().generation, PlacementGeneration{1});
      FP_CHECK(!record.value().requires_revalidation);
    }
    FP_CHECK(store.value().Close().ok());
  }
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    const RecoveryReport& report = store.value().recovery();
    FP_CHECK_EQ(report.placements_total, std::uint64_t{3});
    FP_CHECK_EQ(report.latest_by_flow.size(), std::size_t{3});
    FP_CHECK_EQ(report.placements_requiring_revalidation, std::uint64_t{3});
    FP_CHECK(!report.liveness_restored);
    FP_CHECK(!report.truncated_tail);
    FP_CHECK_EQ(report.last_epoch, FabricEpoch{2});
    FP_CHECK_EQ(report.last_incarnation, IncarnationId{5});
    Result<std::optional<PlacementRecord>> latest = store.value().FindLatest(FlowId{2});
    FP_REQUIRE(latest.ok() && latest.value().has_value());
    FP_CHECK(latest.value()->requires_revalidation);
    FP_CHECK_EQ(latest.value()->intent.flow, FlowId{2});
    FP_CHECK_EQ(store.value().History(FlowId{2}).value().size(), std::size_t{1});
    FP_CHECK(store.value().Close().ok());
  }
}

FP_TEST(store, committing_twice_advances_the_placement_generation) {
  TempStore temp("generation");
  const PlacementEngine engine;
  Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
  FP_REQUIRE(store.ok());
  const PlacementRequest request = FlowRequest(1);
  const PlacementIntent first = IntentFor(request, engine);
  Result<PlacementRecord> record = store.value().CommitPlacement(
      first, IncumbentDelta::kNoIncumbent, Digest{1, 1}, IncarnationId{1}, FabricEpoch{1});
  FP_REQUIRE(record.ok());
  FP_CHECK_EQ(record.value().generation, PlacementGeneration{1});
  FP_CHECK_EQ(record.value().supersedes_generation, std::uint64_t{0});

  PlacementIntent second = first;
  second.path = PathId{2};
  second.reservation = ReservationRef{};
  second.digest = IntentDigest(second);
  record = store.value().CommitPlacement(second, IncumbentDelta::kMoved, Digest{1, 2},
                                        IncarnationId{1}, FabricEpoch{1});
  FP_REQUIRE(record.ok());
  FP_CHECK_EQ(record.value().generation, PlacementGeneration{2});
  FP_CHECK_EQ(record.value().supersedes_generation, std::uint64_t{1});
  const Status closed = store.value().Close();
  FP_CHECK(closed.ok());

  Result<PlacementStore> reopened = PlacementStore::Open(temp.path, StoreOptions{});
  FP_REQUIRE(reopened.ok());
  FP_CHECK_EQ(reopened.value().recovery().placements_total, std::uint64_t{2});
  FP_CHECK_EQ(reopened.value().recovery().superseded_placements, std::uint64_t{1});
  FP_CHECK_EQ(reopened.value().LatestPlacements().value().size(), std::size_t{1});
  FP_CHECK_EQ(reopened.value().LatestPlacements().value()[0].generation, PlacementGeneration{2});
}

FP_TEST(store, identity_replay_is_idempotent_and_conflicts_are_refused) {
  TempStore temp("replay");
  const PlacementEngine engine;
  Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
  FP_REQUIRE(store.ok());
  PlacementRequest request = FlowRequest(1);
  request.attempt = AttemptId{9};
  const PlacementIntent intent = IntentFor(request, engine);
  Result<PlacementRecord> first = store.value().CommitPlacement(
      intent, IncumbentDelta::kNoIncumbent, Digest{2, 2}, IncarnationId{1}, FabricEpoch{1});
  FP_REQUIRE(first.ok());
  Result<PlacementRecord> replay = store.value().CommitPlacement(
      intent, IncumbentDelta::kNoIncumbent, Digest{2, 2}, IncarnationId{1}, FabricEpoch{1});
  FP_REQUIRE(replay.ok());
  FP_CHECK_EQ(replay.value().id, first.value().id);
  FP_CHECK_EQ(store.value().recovery().placements_total, std::uint64_t{0});  // set at open only

  PlacementIntent conflicting = intent;
  conflicting.path = PathId{3};
  conflicting.reservation = ReservationRef{};
  conflicting.digest = IntentDigest(conflicting);
  const Result<PlacementRecord> conflict = store.value().CommitPlacement(
      conflicting, IncumbentDelta::kMoved, Digest{2, 3}, IncarnationId{1}, FabricEpoch{1});
  FP_CHECK(!conflict.ok());
  FP_CHECK_EQ(conflict.status().code(), StatusCode::kDuplicateAttemptConflict);
  FP_CHECK(store.value().Close().ok());

  Result<PlacementStore> reopened = PlacementStore::Open(temp.path, StoreOptions{});
  FP_REQUIRE(reopened.ok());
  FP_CHECK_EQ(reopened.value().recovery().placements_total, std::uint64_t{1});
}

FP_TEST(store, unfinished_attempts_are_reported_and_not_committed) {
  TempStore temp("unfinished");
  const PlacementEngine engine;
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    AttemptRecord attempt;
    attempt.attempt = AttemptId{1};
    attempt.incarnation = IncarnationId{3};
    attempt.epoch = FabricEpoch{1};
    attempt.flow = FlowId{7};
    attempt.request_digest = Digest{4, 4};
    FP_CHECK(store.value().AppendAttemptStart(attempt).ok());

    PlacementRequest request = FlowRequest(7);
    request.attempt = AttemptId{2};
    AttemptRecord committed_start = attempt;
    committed_start.attempt = AttemptId{2};
    FP_CHECK(store.value().AppendAttemptStart(committed_start).ok());
    Result<PlacementRecord> record = store.value().CommitPlacement(
        IntentFor(request, engine), IncumbentDelta::kNoIncumbent, Digest{5, 5}, IncarnationId{3},
        FabricEpoch{1});
    FP_REQUIRE(record.ok());
    AttemptRecord terminal = committed_start;
    terminal.phase = AttemptPhase::kCommitted;
    terminal.code = StatusCode::kOk;
    FP_CHECK(store.value().AppendAttemptTerminal(terminal).ok());
    FP_CHECK(store.value().Close().ok());
  }
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    const RecoveryReport& report = store.value().recovery();
    FP_CHECK_EQ(report.attempts_total, std::uint64_t{2});
    FP_CHECK_EQ(report.unfinished_attempts, std::uint64_t{1});
    FP_CHECK_EQ(report.committed_attempts, std::uint64_t{1});
    FP_CHECK_EQ(report.unfinished.size(), std::size_t{1});
    FP_CHECK_EQ(report.unfinished[0].attempt, AttemptId{1});
    FP_CHECK(report.unfinished[0].phase == AttemptPhase::kStarted);
    FP_CHECK_EQ(report.placements_total, std::uint64_t{1});
    Result<std::optional<PlacementRecord>> by_attempt = store.value().FindByAttempt(AttemptId{2});
    FP_CHECK(by_attempt.ok() && by_attempt.value().has_value());
    Result<std::optional<PlacementRecord>> missing = store.value().FindByAttempt(AttemptId{1});
    FP_CHECK(missing.ok() && !missing.value().has_value());
  }
}

FP_TEST(store, acknowledged_attempt_without_placement_is_corruption) {
  TempStore temp("orphan");
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    AttemptRecord attempt;
    attempt.attempt = AttemptId{4};
    attempt.incarnation = IncarnationId{1};
    attempt.epoch = FabricEpoch{1};
    attempt.flow = FlowId{1};
    attempt.phase = AttemptPhase::kCommitted;
    FP_CHECK(store.value().AppendAttemptTerminal(attempt).ok());
    FP_CHECK(store.value().Close().ok());
  }
  Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
  FP_CHECK(!store.ok());
  FP_CHECK_EQ(store.status().code(), StatusCode::kStoreCorrupt);
}

FP_TEST(store, torn_tail_is_truncated_and_reported) {
  TempStore temp("torn");
  const PlacementEngine engine;
  std::uint64_t good_size = 0;
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    for (std::uint64_t flow = 1; flow <= 3; ++flow) {
      FP_REQUIRE(store.value()
                     .CommitPlacement(IntentFor(FlowRequest(flow), engine),
                                      IncumbentDelta::kNoIncumbent, Digest{flow, flow},
                                      IncarnationId{1}, FabricEpoch{1})
                     .ok());
    }
    FP_CHECK(store.value().Close().ok());
    good_size = FileSize(temp.path);
  }
  // Simulate a torn append: keep the bytes but cut the last record in half.
  {
    std::FILE* file = std::fopen(temp.path.c_str(), "r+b");
    FP_REQUIRE(file != nullptr);
#ifdef _WIN32
    FP_CHECK(_chsize_s(_fileno(file), static_cast<long long>(good_size - 5)) == 0);
#else
    FP_CHECK_EQ(ftruncate(fileno(file), static_cast<off_t>(good_size - 5)), 0);
#endif
    std::fclose(file);
  }
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    FP_CHECK(store.value().recovery().truncated_tail);
    FP_CHECK(store.value().recovery().bytes_discarded > 0);
    FP_CHECK_EQ(store.value().recovery().placements_total, std::uint64_t{2});
    FP_CHECK(store.value().Close().ok());
  }
  // The repair is durable: reopening a second time finds a clean journal.
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    FP_CHECK(!store.value().recovery().truncated_tail);
    FP_CHECK_EQ(store.value().recovery().placements_total, std::uint64_t{2});
  }
}

FP_TEST(store, torn_tail_can_be_reported_without_repair) {
  TempStore temp("no-repair");
  const PlacementEngine engine;
  std::uint64_t size = 0;
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    FP_REQUIRE(store.value()
                   .CommitPlacement(IntentFor(FlowRequest(1), engine), IncumbentDelta::kNoIncumbent,
                                    Digest{1, 1}, IncarnationId{1}, FabricEpoch{1})
                   .ok());
    FP_CHECK(store.value().Close().ok());
    size = FileSize(temp.path);
  }
  {
    std::FILE* file = std::fopen(temp.path.c_str(), "r+b");
    FP_REQUIRE(file != nullptr);
#ifdef _WIN32
    FP_CHECK(_chsize_s(_fileno(file), static_cast<long long>(size - 3)) == 0);
#else
    FP_CHECK_EQ(ftruncate(fileno(file), static_cast<off_t>(size - 3)), 0);
#endif
    std::fclose(file);
  }
  StoreOptions options;
  options.repair_torn_tail = false;
  Result<PlacementStore> store = PlacementStore::Open(temp.path, options);
  FP_CHECK(!store.ok());
  FP_CHECK_EQ(store.status().code(), StatusCode::kStoreTruncatedTail);
  FP_CHECK_EQ(FileSize(temp.path), size - 3);  // untouched
}

FP_TEST(store, mid_file_corruption_fails_closed) {
  TempStore temp("midfile");
  const PlacementEngine engine;
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    std::uint64_t first_record_size = 0;
    for (std::uint64_t flow = 1; flow <= 3; ++flow) {
      if (flow == 2) first_record_size = FileSize(temp.path);
      FP_REQUIRE(store.value()
                     .CommitPlacement(IntentFor(FlowRequest(flow), engine),
                                      IncumbentDelta::kNoIncumbent, Digest{flow, flow},
                                      IncarnationId{1}, FabricEpoch{1})
                     .ok());
    }
    FP_CHECK(store.value().Close().ok());
    // Corrupt a byte inside the second record's payload, not its header, so
    // that a structurally valid record follows the damage.
    FP_CHECK(first_record_size > 0);
    std::FILE* file = std::fopen(temp.path.c_str(), "r+b");
    FP_REQUIRE(file != nullptr);
    FP_CHECK_EQ(std::fseek(file, static_cast<long>(first_record_size + 40), SEEK_SET), 0);
    const unsigned char corrupted = 0xFFu;
    FP_CHECK_EQ(std::fwrite(&corrupted, 1, 1, file), std::size_t{1});
    std::fclose(file);
  }
  Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
  FP_CHECK(!store.ok());
  FP_CHECK_EQ(store.status().code(), StatusCode::kStoreCorrupt);
}

FP_TEST(store, version_mismatch_is_refused) {
  TempStore temp("version");
  const PlacementEngine engine;
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    FP_REQUIRE(store.value()
                   .CommitPlacement(IntentFor(FlowRequest(1), engine), IncumbentDelta::kNoIncumbent,
                                    Digest{1, 1}, IncarnationId{1}, FabricEpoch{1})
                   .ok());
    FP_REQUIRE(store.value()
                   .CommitPlacement(IntentFor(FlowRequest(2), engine), IncumbentDelta::kNoIncumbent,
                                    Digest{2, 2}, IncarnationId{1}, FabricEpoch{1})
                   .ok());
    FP_CHECK(store.value().Close().ok());
  }
  {
    std::FILE* file = std::fopen(temp.path.c_str(), "r+b");
    FP_REQUIRE(file != nullptr);
    FP_CHECK_EQ(std::fseek(file, 4, SEEK_SET), 0);
    const unsigned char version = 9;
    FP_CHECK_EQ(std::fwrite(&version, 1, 1, file), std::size_t{1});
    std::fclose(file);
  }
  Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
  FP_CHECK(!store.ok());
  FP_CHECK_EQ(store.status().code(), StatusCode::kStoreCorrupt);
}

FP_TEST(store, compaction_preserves_state_and_resets_the_journal) {
  TempStore temp("compact");
  const PlacementEngine engine;
  Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
  FP_REQUIRE(store.ok());
  for (std::uint64_t flow = 1; flow <= 6; ++flow) {
    FP_REQUIRE(store.value()
                   .CommitPlacement(IntentFor(FlowRequest(flow), engine), IncumbentDelta::kNoIncumbent,
                                    Digest{flow, flow}, IncarnationId{1}, FabricEpoch{1})
                   .ok());
  }
  const std::uint64_t before = FileSize(temp.path);
  FP_CHECK(store.value().Compact().ok());
  FP_CHECK_EQ(store.value().stats().compactions, std::uint64_t{1});
  // The fresh journal holds exactly the crash-safe reset marker.
  FP_CHECK_EQ(FileSize(temp.path), std::uint64_t{36 + 8});
  FP_CHECK(FileSize(temp.path + ".snap") > 0);
  // Further commits append to the fresh journal with continued sequencing.
  FP_REQUIRE(store.value()
                 .CommitPlacement(IntentFor(FlowRequest(7), engine), IncumbentDelta::kNoIncumbent,
                                  Digest{7, 7}, IncarnationId{1}, FabricEpoch{1})
                 .ok());
  FP_CHECK(FileSize(temp.path) > 0);
  FP_CHECK(store.value().Close().ok());
  FP_CHECK(before > 0);

  Result<PlacementStore> reopened = PlacementStore::Open(temp.path, StoreOptions{});
  if (!reopened.ok()) {
    std::fprintf(stderr, "compaction reopen failed: %s\n", reopened.status().ToString().c_str());
  }
  FP_REQUIRE(reopened.ok());
  FP_CHECK(reopened.value().recovery().snapshot_loaded);
  FP_CHECK_EQ(reopened.value().recovery().placements_total, std::uint64_t{7});
  FP_CHECK_EQ(reopened.value().LatestPlacements().value().size(), std::size_t{7});
}

FP_TEST(store, replay_after_a_restart_is_idempotent_and_keeps_the_journal_readable) {
  TempStore temp("replay-restart");
  const PlacementEngine engine;
  PlacementRequest request = FlowRequest(1);
  request.attempt = AttemptId{77};
  const PlacementIntent intent = IntentFor(request, engine);
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    FP_REQUIRE(store.value()
                   .CommitPlacement(intent, IncumbentDelta::kNoIncumbent, Digest{7, 7},
                                    IncarnationId{1}, FabricEpoch{1})
                   .ok());
    FP_CHECK(store.value().Close().ok());
  }
  {
    // A fresh process has no in-memory attempt table: the durable one must
    // refuse a second start record for an attempt that already terminated.
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    AttemptRecord replay;
    replay.attempt = AttemptId{77};
    replay.incarnation = IncarnationId{2};
    replay.epoch = FabricEpoch{1};
    replay.flow = FlowId{1};
    const Status status = store.value().AppendAttemptStart(replay);
    FP_CHECK(!status.ok());
    FP_CHECK_EQ(status.code(), StatusCode::kAlreadyExists);
    FP_CHECK(store.value().Close().ok());
  }
  {
    // The journal is still readable: no terminal-then-start ordering exists.
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    FP_CHECK_EQ(store.value().recovery().placements_total, std::uint64_t{1});
  }
}

FP_TEST(store, auto_compaction_bounds_the_journal_and_rebases_the_sequence) {
  TempStore temp("auto-compact");
  const PlacementEngine engine;
  StoreOptions options;
  options.max_journal_records = 6;
  Result<PlacementStore> store = PlacementStore::Open(temp.path, options);
  FP_REQUIRE(store.ok());
  for (std::uint64_t flow = 1; flow <= 30; ++flow) {
    FP_REQUIRE(store.value()
                   .CommitPlacement(IntentFor(FlowRequest(flow), engine),
                                    IncumbentDelta::kNoIncumbent, Digest{flow, flow},
                                    IncarnationId{1}, FabricEpoch{1})
                   .ok());
  }
  const StoreStats stats = store.value().stats();
  FP_CHECK(stats.compactions > 0);
  FP_CHECK(stats.journal_records <= options.max_journal_records);
  FP_CHECK_EQ(store.value().Close().ok(), true);

  Result<PlacementStore> reopened = PlacementStore::Open(temp.path, options);
  FP_REQUIRE(reopened.ok());
  FP_CHECK(reopened.value().recovery().snapshot_loaded);
  FP_CHECK_EQ(reopened.value().recovery().placements_total, std::uint64_t{30});
  FP_CHECK_EQ(reopened.value().LatestPlacements().value().size(), std::size_t{30});
  FP_CHECK(reopened.value().recovery().max_sequence <= options.max_journal_records + 1);
}

FP_TEST(store, growth_bounds_and_oversize_records_are_refused) {
  TempStore temp("bounds");
  const PlacementEngine engine;
  StoreOptions options;
  options.max_total_records = 2;
  Result<PlacementStore> store = PlacementStore::Open(temp.path, options);
  FP_REQUIRE(store.ok());
  FP_REQUIRE(store.value()
                 .CommitPlacement(IntentFor(FlowRequest(1), engine), IncumbentDelta::kNoIncumbent,
                                  Digest{1, 1}, IncarnationId{1}, FabricEpoch{1})
                 .ok());
  FP_REQUIRE(store.value()
                 .CommitPlacement(IntentFor(FlowRequest(2), engine), IncumbentDelta::kNoIncumbent,
                                  Digest{2, 2}, IncarnationId{1}, FabricEpoch{1})
                 .ok());
  const Result<PlacementRecord> third =
      store.value().CommitPlacement(IntentFor(FlowRequest(3), engine), IncumbentDelta::kNoIncumbent,
                                    Digest{3, 3}, IncarnationId{1}, FabricEpoch{1});
  FP_CHECK(!third.ok());
  FP_CHECK_EQ(third.status().code(), StatusCode::kStoreGrowthExceeded);
  FP_CHECK(store.value().Close().ok());

  TempStore small("oversize");
  StoreOptions tiny;
  tiny.max_record_bytes = 32;
  Result<PlacementStore> store2 = PlacementStore::Open(small.path, tiny);
  FP_REQUIRE(store2.ok());
  const Result<PlacementRecord> refused =
      store2.value().CommitPlacement(IntentFor(FlowRequest(1), engine), IncumbentDelta::kNoIncumbent,
                                     Digest{1, 1}, IncarnationId{1}, FabricEpoch{1});
  FP_CHECK(!refused.ok());
  FP_CHECK_EQ(refused.status().code(), StatusCode::kOversizeRequest);
}

FP_TEST(store, read_only_open_never_mutates) {
  TempStore temp("readonly");
  const PlacementEngine engine;
  {
    Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
    FP_REQUIRE(store.ok());
    FP_REQUIRE(store.value()
                   .CommitPlacement(IntentFor(FlowRequest(1), engine), IncumbentDelta::kNoIncumbent,
                                    Digest{1, 1}, IncarnationId{1}, FabricEpoch{1})
                   .ok());
    FP_CHECK(store.value().Close().ok());
  }
  const std::uint64_t size = FileSize(temp.path);
  StoreOptions options;
  options.read_only = true;
  Result<PlacementStore> store = PlacementStore::Open(temp.path, options);
  FP_REQUIRE(store.ok());
  FP_CHECK_EQ(store.value().recovery().placements_total, std::uint64_t{1});
  const Result<PlacementRecord> refused =
      store.value().CommitPlacement(IntentFor(FlowRequest(2), engine), IncumbentDelta::kNoIncumbent,
                                    Digest{2, 2}, IncarnationId{1}, FabricEpoch{1});
  FP_CHECK(!refused.ok());
  FP_CHECK_EQ(refused.status().code(), StatusCode::kNotRunning);
  FP_CHECK_EQ(FileSize(temp.path), size);
}

FP_TEST(store, intents_must_be_self_consistent) {
  TempStore temp("intent");
  const PlacementEngine engine;
  Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
  FP_REQUIRE(store.ok());
  PlacementIntent intent = IntentFor(FlowRequest(1), engine);
  intent.path = PathId{99};  // digest no longer matches the contents
  const Result<PlacementRecord> refused = store.value().CommitPlacement(
      intent, IncumbentDelta::kNoIncumbent, Digest{1, 1}, IncarnationId{1}, FabricEpoch{1});
  FP_CHECK(!refused.ok());
  FP_CHECK_EQ(refused.status().code(), StatusCode::kInconsistentAuthorityGeneration);
  PlacementIntent empty;
  const Result<PlacementRecord> refused_empty = store.value().CommitPlacement(
      empty, IncumbentDelta::kNoIncumbent, Digest{1, 1}, IncarnationId{1}, FabricEpoch{1});
  FP_CHECK(!refused_empty.ok());
  FP_CHECK_EQ(refused_empty.status().code(), StatusCode::kInvalidArgument);
}

FP_TEST(store, closed_store_refuses_mutation) {
  TempStore temp("closed");
  const PlacementEngine engine;
  Result<PlacementStore> store = PlacementStore::Open(temp.path, StoreOptions{});
  FP_REQUIRE(store.ok());
  FP_CHECK(store.value().Close().ok());
  FP_CHECK_EQ(store.value().CommitPlacement(IntentFor(FlowRequest(1), engine),
                                            IncumbentDelta::kNoIncumbent, Digest{1, 1},
                                            IncarnationId{1}, FabricEpoch{1})
                  .status()
                  .code(),
              StatusCode::kNotRunning);
  AttemptRecord record;
  record.attempt = AttemptId{1};
  FP_CHECK_EQ(store.value().AppendAttemptStart(record).code(), StatusCode::kNotRunning);
}

// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Durable placement intent and history.
//
// Ordering contract for every durable mutation:
//   validate -> bind authority -> plan -> journal attempt start -> append
//   placement -> sync -> verify -> report committed.
// A committed mutation is never acknowledged before the bytes that carry it are
// durable. Recovery distinguishes durable history, committed authoritative
// state, unfinished attempts, and evidence that must be revalidated. Recovered
// state never restores process liveness, leases, worker authority, or telemetry
// freshness.

#ifndef FLOWPLACE_STORE_HPP
#define FLOWPLACE_STORE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flowplace/explain.hpp"
#include "flowplace/model.hpp"
#include "flowplace/status.hpp"

namespace flowplace {

enum class AttemptPhase : std::uint8_t {
  kStarted = 0,
  kCommitted = 1,
  kRejected = 2,
  kCancelled = 3,
  kFenced = 4,
};

std::string_view AttemptPhaseName(AttemptPhase v) noexcept;
std::optional<AttemptPhase> ParseAttemptPhase(std::string_view v) noexcept;

struct AttemptRecord {
  AttemptId attempt;
  IncarnationId incarnation;
  FabricEpoch epoch;
  FlowId flow;
  AttemptPhase phase = AttemptPhase::kStarted;
  StatusCode code = StatusCode::kOk;
  Digest request_digest;
  std::uint64_t sequence = 0;
  std::int64_t observed_unix_nanos = 0;  // informational only, never authority
};

struct PlacementRecord {
  PlacementId id;
  PlacementGeneration generation;
  PlacementIntent intent;
  IncumbentDelta delta = IncumbentDelta::kNoIncumbent;
  Digest decision_digest;
  IncarnationId committed_by;
  FabricEpoch commit_epoch;
  std::uint64_t sequence = 0;               // durable order of the commit
  std::uint64_t supersedes_generation = 0;  // prior generation for this flow, 0 = none
  std::int64_t commit_unix_nanos = 0;       // informational only, never authority
  // Recovery annotation. Not persisted: it states that this incarnation has not
  // yet revalidated the placement against current evidence.
  bool requires_revalidation = true;
};

struct RecoveryReport {
  // Durable state never restores liveness. This field is always false and
  // exists so that callers can assert on it.
  bool liveness_restored = false;
  bool truncated_tail = false;
  bool snapshot_loaded = false;
  std::uint64_t bytes_discarded = 0;
  std::uint64_t records_valid = 0;
  std::uint64_t placements_total = 0;
  std::uint64_t attempts_total = 0;
  std::uint64_t committed_attempts = 0;
  std::uint64_t rejected_attempts = 0;
  std::uint64_t cancelled_attempts = 0;
  std::uint64_t fenced_attempts = 0;
  std::uint64_t unfinished_attempts = 0;
  std::uint64_t superseded_placements = 0;
  std::uint64_t placements_requiring_revalidation = 0;
  // Attempt records that compaction retired once they aged out of the retained
  // window. Their durable placements and generations are unaffected.
  std::uint64_t retired_attempts = 0;
  std::uint64_t max_sequence = 0;
  FabricEpoch last_epoch;
  IncarnationId last_incarnation;
  std::vector<PlacementRecord> latest_by_flow;  // ascending by flow id
  std::vector<AttemptRecord> unfinished;        // bounded sample
  std::string detail;
};

struct StoreOptions {
  bool fsync_on_append = true;
  bool repair_torn_tail = true;
  bool read_only = false;
  std::uint64_t max_record_bytes = 1u << 20;    // 1 MiB bounded record payload
  std::uint64_t max_journal_records = 8192;     // compaction threshold
  std::uint64_t max_total_records = 1u << 22;   // bounded durable growth
};

struct StoreStats {
  std::uint64_t journal_records = 0;
  std::uint64_t journal_bytes = 0;
  std::uint64_t snapshot_bytes = 0;
  std::uint64_t compactions = 0;
  std::uint64_t appends = 0;
  std::uint64_t syncs = 0;
};

// A single-file append-only journal with an optional snapshot for compaction.
class PlacementStore {
 public:
  static Result<PlacementStore> Open(const std::string& path, StoreOptions options = {});
  ~PlacementStore();

  PlacementStore(const PlacementStore&) = delete;
  PlacementStore& operator=(const PlacementStore&) = delete;
  PlacementStore(PlacementStore&& other) noexcept;
  PlacementStore& operator=(PlacementStore&& other) noexcept;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] const StoreOptions& options() const noexcept { return options_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] StoreStats stats() const noexcept { return stats_; }
  [[nodiscard]] bool IsOpen() const noexcept { return file_ != nullptr; }

  // Journal an attempt before any authoritative work is attempted.
  Status AppendAttemptStart(const AttemptRecord& record);
  // Journal a terminal attempt state that did not commit.
  Status AppendAttemptTerminal(const AttemptRecord& record);

  // Durable commit. Allocates the placement identity and generation, appends
  // and syncs the record, and returns it. Returns kAlreadyCommitted (with the
  // existing record) when an identical attempt already committed, and
  // kDuplicateAttemptConflict when the same attempt commits a different intent.
  Result<PlacementRecord> CommitPlacement(const PlacementIntent& intent, IncumbentDelta delta,
                                          Digest decision_digest, IncarnationId committed_by,
                                          FabricEpoch commit_epoch);

  [[nodiscard]] Result<std::optional<PlacementRecord>> FindLatest(FlowId flow) const;
  [[nodiscard]] Result<std::vector<PlacementRecord>> History(FlowId flow) const;
  [[nodiscard]] Result<std::vector<PlacementRecord>> LatestPlacements() const;
  [[nodiscard]] Result<std::vector<AttemptRecord>> Attempts() const;
  [[nodiscard]] Result<std::optional<AttemptRecord>> FindAttempt(AttemptId attempt) const;
  [[nodiscard]] Result<std::optional<PlacementRecord>> FindByAttempt(AttemptId attempt) const;

  // Rewrite the journal into a snapshot and reset the journal. Crash-safe:
  // the snapshot is durable before the journal is replaced.
  Status Compact();
  // Flush pending bytes to stable storage.
  Status Sync();
  // Flush and close. Further mutation returns kNotRunning.
  Status Close();

 private:
  PlacementStore() = default;
  Status AppendRecordRaw(std::uint16_t type, const std::vector<std::uint8_t>& payload);
  // Folds the journal into a snapshot once it reaches its configured length.
  Status MaybeCompact();
  Status LoadSnapshot();
  Status LoadJournal();
  // Classifies unparseable journal bytes at |offset| as a torn tail (repairable
  // and reported) or as mid-file damage (fail closed).
  Status HandleDamage(std::uint64_t offset, std::uint64_t total_bytes);
  struct Impl;
  std::string path_;
  StoreOptions options_{};
  RecoveryReport recovery_{};
  StoreStats stats_{};
  void* file_ = nullptr;
  Impl* impl_ = nullptr;
};

}  // namespace flowplace

#endif  // FLOWPLACE_STORE_HPP

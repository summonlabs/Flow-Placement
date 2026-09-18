// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Bounded, fenced, cancellable coordination of placement work.
//
// The coordinator owns the durable store, a bounded worker pool, a bounded work
// queue, the current fabric epoch, this process incarnation, and the authority
// view that a placement must still match at commit time. Work that is
// cancelled, fenced, or bound to superseded authority never mutates
// authoritative state.

#ifndef FLOWPLACE_RUNTIME_HPP
#define FLOWPLACE_RUNTIME_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "flowplace/engine.hpp"
#include "flowplace/model.hpp"
#include "flowplace/status.hpp"
#include "flowplace/store.hpp"

namespace flowplace {

enum class AttemptState : std::uint8_t {
  kQueued = 0,
  kRunning = 1,
  kCommitted = 2,
  kRejected = 3,   // the engine declined; no durable mutation
  kCancelled = 4,  // cancellation was observed before the commit boundary
  kFenced = 5,     // epoch/incarnation/authority superseded before commit
  kFailed = 6,     // internal failure; no durable mutation
};

std::string_view AttemptStateName(AttemptState v) noexcept;

struct AttemptOutcome {
  AttemptId attempt;
  AttemptState state = AttemptState::kQueued;
  StatusCode code = StatusCode::kOk;
  PlacementDecision decision;
  PlacementId placement_id;
  PlacementGeneration placement_generation;
};

enum class RevalidationVerdict : std::uint8_t {
  kUnknownFlow = 0,   // no durable placement for this flow
  kStillCurrent = 1,  // the durable placement is still the engine's choice
  kMustMove = 2,      // a different path is now the correct choice
  kRejected = 3,      // no legal placement exists now
  kStaleInput = 4,    // the revalidation input itself is stale
};

std::string_view RevalidationVerdictName(RevalidationVerdict v) noexcept;

// A placement that has been computed but not yet committed. The commit boundary
// re-checks cancellation, epoch, and authority, so a prepared attempt that
// waited too long is fenced instead of committing stale work.
struct PreparedAttempt {
  AttemptId attempt;
  PlacementRequest request;
  PlacementDecision decision;
};

struct RevalidationResult {
  RevalidationVerdict verdict = RevalidationVerdict::kUnknownFlow;
  PlacementDecision decision;
  std::optional<PlacementRecord> previous;
};

struct CoordinatorOptions {
  Limits limits{};
  StoreOptions store{};
  std::string store_path;              // empty = in-memory coordination only
  std::uint32_t worker_threads = 4;    // bounded to [1, 64]
  std::uint64_t queue_capacity = 1024; // bounded to [1, 1<<20]
  // Bounded retention of attempt records. Terminal attempts are reaped oldest
  // first once this many attempts are retained; an attempt that has been reaped
  // is reported as unknown rather than silently replayed.
  std::uint64_t max_retained_attempts = 4096;
  FabricEpoch initial_epoch{1};
  IncarnationId incarnation;           // 0 = allocate a fresh incarnation
  std::uint64_t seed = 0;              // used only to derive a fresh incarnation
};

struct CoordinatorStats {
  std::uint64_t submitted = 0;
  std::uint64_t accepted = 0;
  std::uint64_t duplicate_replays = 0;
  std::uint64_t duplicate_conflicts = 0;
  std::uint64_t rejected_queue_full = 0;
  std::uint64_t rejected_shutting_down = 0;
  std::uint64_t placed = 0;
  std::uint64_t rejected = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t fenced = 0;
  std::uint64_t failed = 0;
  std::uint64_t queued = 0;   // instantaneous
  std::uint64_t active = 0;   // instantaneous
  std::uint64_t completed = 0;
};

class PlacementCoordinator {
 public:
  static Result<std::unique_ptr<PlacementCoordinator>> Start(CoordinatorOptions options);
  ~PlacementCoordinator();

  PlacementCoordinator(const PlacementCoordinator&) = delete;
  PlacementCoordinator& operator=(const PlacementCoordinator&) = delete;

  // Enqueue work. Returns kQueueFull when the bounded queue is full,
  // kShuttingDown when shutdown has begun, kAlreadyExists for an idempotent
  // replay of a recorded attempt, and kDuplicateAttemptConflict when the same
  // attempt identity is submitted with a different request.
  Status Submit(PlacementRequest request, AttemptId attempt);

  // Block until the attempt reaches a terminal state. Returns kUnknownAttempt
  // when no such attempt is known to this coordinator.
  Result<AttemptOutcome> Await(AttemptId attempt);

  // Two-phase placement. Prepare validates the request, journals the attempt,
  // and computes the decision without mutating authoritative state. Commit
  // applies the commit boundary: it re-checks cancellation, epoch, and
  // authority, and only then makes the placement durable.
  Result<PreparedAttempt> Prepare(PlacementRequest request, AttemptId attempt);
  Result<AttemptOutcome> Commit(PreparedAttempt prepared);

  // Request cancellation. Cancellation observed before the commit boundary
  // prevents the commit; cancellation observed after it is reported as
  // kAlreadyCommitted and does not undo durable state.
  Status Cancel(AttemptId attempt);

  // Synchronous placement on the calling thread: Prepare followed by Commit.
  // The same fencing, validation, and durable commit rules apply; only the
  // queue hop is skipped.
  Result<AttemptOutcome> PlaceNow(PlacementRequest request, AttemptId attempt);

  // Advance the fabric epoch. Work bound to the previous epoch is fenced at
  // commit time.
  Status AdvanceEpoch();
  [[nodiscard]] FabricEpoch epoch() const;
  [[nodiscard]] IncarnationId incarnation() const noexcept;

  // Replace the authority view. A placement whose authority expectation is no
  // longer current fails its commit with kStaleAuthorityGeneration.
  Status SetAuthority(AuthorityExpectation expected);
  [[nodiscard]] AuthorityExpectation authority() const;

  // Re-derive the correct placement from current evidence and compare it with
  // the durable placement for the flow.
  Result<RevalidationResult> Revalidate(PlacementRequest request);

  [[nodiscard]] const Limits& limits() const noexcept;
  [[nodiscard]] Result<std::vector<PlacementRecord>> LatestPlacements();
  [[nodiscard]] RecoveryReport recovery() const;
  [[nodiscard]] CoordinatorStats stats() const;
  [[nodiscard]] bool running() const;
  [[nodiscard]] std::uint32_t worker_count() const noexcept;

  // Wait until no work is queued or active.
  Status Drain();

  // Graceful shutdown: stop accepting, cancel queued work, let running work
  // reach its boundary, join workers, flush and close the store.
  Status Shutdown();
  // Shutdown that also requests cancellation of active work.
  Status ShutdownCancelling();

 private:
  PlacementCoordinator();
  Status ShutdownInternal(bool cancel_active);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace flowplace

#endif  // FLOWPLACE_RUNTIME_HPP

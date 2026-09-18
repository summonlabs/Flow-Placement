// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Coordinator runtime: bounded workers, a bounded queue, durable attempt
// journaling, a commit boundary that re-checks cancellation, epoch, and
// authority, and a shutdown path that never joins a worker while holding a
// lock that worker needs.
//
// Lock order (strict): commit_mu -> store_mu -> mu, and commit_mu -> mu.
// No path acquires mu before commit_mu or store_mu. No callback, hook, or user
// code is ever invoked while any lock is held.

#include "flowplace/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "flowplace/checked.hpp"
#include "flowplace/engine.hpp"
#include "flowplace/hash.hpp"
#include "flowplace/process.hpp"

namespace flowplace {

std::string_view AttemptStateName(AttemptState v) noexcept {
  switch (v) {
    case AttemptState::kQueued: return "QUEUED";
    case AttemptState::kRunning: return "RUNNING";
    case AttemptState::kCommitted: return "COMMITTED";
    case AttemptState::kRejected: return "REJECTED";
    case AttemptState::kCancelled: return "CANCELLED";
    case AttemptState::kFenced: return "FENCED";
    case AttemptState::kFailed: return "FAILED";
  }
  return "UNKNOWN_STATE";
}

std::string_view RevalidationVerdictName(RevalidationVerdict v) noexcept {
  switch (v) {
    case RevalidationVerdict::kUnknownFlow: return "UNKNOWN_FLOW";
    case RevalidationVerdict::kStillCurrent: return "STILL_CURRENT";
    case RevalidationVerdict::kMustMove: return "MUST_MOVE";
    case RevalidationVerdict::kRejected: return "REJECTED";
    case RevalidationVerdict::kStaleInput: return "STALE_INPUT";
  }
  return "UNKNOWN_VERDICT";
}

namespace {

constexpr std::uint32_t kMaxWorkerThreads = 64;
constexpr std::uint64_t kMaxQueueCapacity = 1u << 20;

IncarnationId AllocateIncarnation(std::uint64_t seed) {
  CanonicalHasher hasher;
  hasher.AddTag("incarnation");
  hasher.AddU64(static_cast<std::uint64_t>(CurrentProcessId()));
  hasher.AddI64(CurrentUnixNanos());
  hasher.AddU64(seed);
  const Digest digest = Digest::OfCanonical(hasher);
  std::uint64_t value = digest.lo ^ digest.hi;
  if (value == 0) value = 1;
  return IncarnationId{value};
}

struct Slot {
  AttemptId attempt;
  PlacementRequest request;
  Digest request_digest;
  bool terminal = false;
  bool commit_boundary_crossed = false;
  bool cancel_requested = false;
  bool running = false;    // a synchronous caller owns this slot
  bool in_worker = false;  // a worker is currently processing this slot
  bool journalled_start = false;
  // Set when the slot reached a terminal state before its attempt-start record
  // was durable; the registering thread journals the terminal phase instead, so
  // the journal can never show a terminal phase before the start.
  bool pending_terminal = false;
  AttemptPhase pending_phase = AttemptPhase::kCancelled;
  StatusCode pending_code = StatusCode::kCancelled;
  AttemptOutcome outcome;
};

enum class RunState { kRunning, kShuttingDown, kStopped };

struct CoordinatorState {
  CoordinatorOptions options;
  PlacementEngine engine;
  std::unique_ptr<PlacementStore> store;

  AuthorityExpectation authority;
  FabricEpoch epoch;
  IncarnationId incarnation;

  mutable std::mutex mu;
  std::condition_variable cv_work;
  std::condition_variable cv_done;
  RunState state = RunState::kRunning;
  std::deque<AttemptId> queue;
  std::deque<AttemptId> slot_order;  // insertion order for bounded reaping
  std::uint64_t queue_depth = 0;
  std::uint64_t active = 0;
  std::map<AttemptId, std::shared_ptr<Slot>> slots;
  CoordinatorStats stats;
  std::vector<std::thread> workers;

  // Serializes the commit boundary against epoch and authority changes.
  std::mutex commit_mu;
  // Serializes durable store access.
  std::mutex store_mu;

  // In-memory identity allocation and history (used when no store is open).
  std::uint64_t next_placement_id = 1;
  std::map<FlowId, PlacementRecord> latest_in_memory;

  std::uint32_t worker_count = 0;
};

// Requires mu to be held. Drops the oldest terminal attempts once the retained
// table exceeds its bound. An attempt that has been reaped is reported as
// unknown rather than silently replayed; its durable record is unaffected.
void ReapRetainedSlots(CoordinatorState& st) {
  const std::uint64_t limit = st.options.max_retained_attempts;
  std::size_t scanned = 0;
  while (st.slots.size() > limit && !st.slot_order.empty() && scanned < st.slot_order.size()) {
    const AttemptId oldest = st.slot_order.front();
    st.slot_order.pop_front();
    ++scanned;
    const auto it = st.slots.find(oldest);
    if (it == st.slots.end()) continue;
    if (!it->second->terminal) {
      // Still in flight: keep it and look at the next candidate.
      st.slot_order.push_back(oldest);
      continue;
    }
    st.slots.erase(it);
    scanned = 0;  // progress was made, so the scan budget restarts
  }
}

// Requires mu to be held.
void FinalizeLocked(CoordinatorState& st, const std::shared_ptr<Slot>& slot, AttemptState state,
                    StatusCode code) {
  if (slot->terminal) return;
  slot->outcome.state = state;
  slot->outcome.code = code;
  slot->terminal = true;
  switch (state) {
    case AttemptState::kCommitted: ++st.stats.placed; break;
    case AttemptState::kRejected: ++st.stats.rejected; break;
    case AttemptState::kCancelled: ++st.stats.cancelled; break;
    case AttemptState::kFenced: ++st.stats.fenced; break;
    case AttemptState::kFailed: ++st.stats.failed; break;
    case AttemptState::kQueued:
    case AttemptState::kRunning: break;
  }
  ++st.stats.completed;
  st.cv_done.notify_all();
  st.cv_work.notify_all();
}

// Requires mu to be held. Finalizes a slot that shutdown is cancelling and
// records whether its terminal phase must be journaled by the caller.
void FinalizeShutdownSlot(CoordinatorState& st, const std::shared_ptr<Slot>& slot,
                          std::vector<AttemptId>& journal_cancelled) {
  slot->cancel_requested = true;
  FinalizeLocked(st, slot, AttemptState::kCancelled, StatusCode::kShuttingDown);
  if (slot->journalled_start) {
    journal_cancelled.push_back(slot->attempt);
  } else {
    // The registering thread owns the journal until the start record is
    // durable; it writes this terminal phase afterwards.
    slot->pending_terminal = true;
    slot->pending_phase = AttemptPhase::kCancelled;
    slot->pending_code = StatusCode::kShuttingDown;
  }
}


void JournalTerminal(CoordinatorState& st, const std::shared_ptr<Slot>& slot, AttemptPhase phase,
                     StatusCode code) {
  if (!st.store) return;
  AttemptRecord record;
  record.attempt = slot->attempt;
  record.incarnation = st.incarnation;
  // The epoch the attempt was admitted under, never a later one: reading the
  // coordinator's current epoch here would race with an epoch advance and would
  // mis-stamp the record.
  record.epoch = slot->request.expected.fabric_epoch;
  record.flow = slot->request.flow;
  record.phase = phase;
  record.code = code;
  record.request_digest = slot->request_digest;
  record.observed_unix_nanos = CurrentUnixNanos();
  std::lock_guard<std::mutex> lock(st.store_mu);
  if (phase == AttemptPhase::kStarted) {
    static_cast<void>(st.store->AppendAttemptStart(record));
  } else {
    static_cast<void>(st.store->AppendAttemptTerminal(record));
  }
}

Result<PlacementRecord> CommitRecord(CoordinatorState& st, const PlacementIntent& intent,
                                     IncumbentDelta delta, Digest decision_digest,
                                     FabricEpoch commit_epoch) {
  if (st.store) {
    std::lock_guard<std::mutex> lock(st.store_mu);
    return st.store->CommitPlacement(intent, delta, decision_digest, st.incarnation, commit_epoch);
  }
  // In-memory authority is guarded by mu, exactly like every other reader.
  std::lock_guard<std::mutex> lock(st.mu);
  const auto existing = st.latest_in_memory.find(intent.flow);
  PlacementRecord record;
  record.id = PlacementId{st.next_placement_id};
  if (st.next_placement_id == std::numeric_limits<std::uint64_t>::max()) {
    return Status(StatusCode::kArithmeticOverflow, "in-memory placement identity exhausted");
  }
  ++st.next_placement_id;
  if (existing == st.latest_in_memory.end()) {
    record.generation = PlacementGeneration{1};
  } else {
    if (existing->second.generation.value() == std::numeric_limits<std::uint64_t>::max()) {
      return Status(StatusCode::kArithmeticOverflow, "placement generation exhausted");
    }
    record.supersedes_generation = existing->second.generation.value();
    record.generation = PlacementGeneration{existing->second.generation.value() + 1};
  }
  record.intent = intent;
  record.delta = delta;
  record.decision_digest = decision_digest;
  record.committed_by = st.incarnation;
  record.commit_epoch = commit_epoch;
  record.commit_unix_nanos = CurrentUnixNanos();
  record.requires_revalidation = false;
  st.latest_in_memory[intent.flow] = record;
  return record;
}

// A zero field in the coordinator's authority view means "unconstrained"; a
// non-zero field must match the attempt's expectation exactly.
bool AuthorityMatches(const AuthorityExpectation& current, const AuthorityExpectation& expected) {
  if (current.path_authority.valid() && current.path_authority != expected.path_authority) {
    return false;
  }
  if (current.candidate_set.valid() && current.candidate_set != expected.candidate_set) return false;
  if (current.capacity.valid() && current.capacity != expected.capacity) return false;
  if (current.policy.valid() && current.policy != expected.policy) return false;
  if (current.qos.valid() && current.qos != expected.qos) return false;
  if (current.evidence.valid() && current.evidence != expected.evidence) return false;
  return true;
}

// The commit boundary: cancellation, epoch, and authority are re-checked under
// commit_mu before anything becomes durable.
Status CommitSlot(CoordinatorState& st, const std::shared_ptr<Slot>& slot) {
  std::unique_lock<std::mutex> commit_lock(st.commit_mu);
  {
    std::lock_guard<std::mutex> lock(st.mu);
    if (slot->terminal) {
      return Status(StatusCode::kAlreadyCommitted, "attempt already reached a terminal state");
    }
    if (slot->cancel_requested) {
      return Status(StatusCode::kCancelled, "cancellation was observed before the commit boundary");
    }
    if (st.state != RunState::kRunning) {
      return Status(StatusCode::kShuttingDown, "coordinator is shutting down");
    }
    if (slot->request.expected.fabric_epoch != st.epoch) {
      return Status(StatusCode::kFencedStaleEpoch,
                    "fabric epoch advanced after this attempt was admitted");
    }
    if (!AuthorityMatches(st.authority, slot->request.expected)) {
      return Status(StatusCode::kStaleAuthorityGeneration,
                    "authority advanced after this attempt was admitted");
    }
    slot->commit_boundary_crossed = true;
  }

  PlacementDecision decision;
  {
    std::lock_guard<std::mutex> lock(st.mu);
    decision = slot->outcome.decision;
  }
  if (!decision.intent.has_value()) {
    return Status(StatusCode::kInternalInvariantViolation, "commit without a placement intent");
  }
  const PlacementIntent intent = *decision.intent;
  Result<PlacementRecord> record =
      CommitRecord(st, intent, decision.delta, decision.digest, slot->request.expected.fabric_epoch);
  if (!record.ok()) return record.status();

  {
    std::lock_guard<std::mutex> lock(st.mu);
    slot->outcome.placement_id = record.value().id;
    slot->outcome.placement_generation = record.value().generation;
  }
  JournalTerminal(st, slot, AttemptPhase::kCommitted, StatusCode::kOk);
  {
    std::lock_guard<std::mutex> lock(st.mu);
    FinalizeLocked(st, slot, AttemptState::kCommitted, StatusCode::kOk);
  }
  return Status::Ok();
}

AttemptState StateForFailure(StatusCode code) {
  switch (code) {
    case StatusCode::kCancelled: return AttemptState::kCancelled;
    case StatusCode::kFencedStaleEpoch:
    case StatusCode::kFencedStaleIncarnation:
    case StatusCode::kStaleAuthorityGeneration:
      return AttemptState::kFenced;
    case StatusCode::kShuttingDown: return AttemptState::kCancelled;
    default: return AttemptState::kFailed;
  }
}

AttemptPhase PhaseForState(AttemptState state) {
  switch (state) {
    case AttemptState::kCommitted: return AttemptPhase::kCommitted;
    case AttemptState::kCancelled: return AttemptPhase::kCancelled;
    case AttemptState::kFenced: return AttemptPhase::kFenced;
    default: return AttemptPhase::kRejected;
  }
}

// Pure decision computation with a pre-commit self-verification. No lock is
// held while the engine runs.
PlacementDecision ComputeDecision(CoordinatorState& st, const std::shared_ptr<Slot>& slot) {
  PlacementDecision decision;
  try {
    decision = st.engine.Place(slot->request);
  } catch (...) {
    decision = PlacementDecision{};
    decision.outcome = Outcome::kConflictingInput;
    decision.code = StatusCode::kInternalInvariantViolation;
    decision.explanation.notes.push_back("engine raised an exception");
    decision.digest = DecisionDigest(decision);
  }
  if (decision.placed()) {
    std::string reason;
    if (!st.engine.VerifyIntent(slot->request, *decision.intent, &reason)) {
      decision = PlacementDecision{};
      decision.outcome = Outcome::kConflictingInput;
      decision.code = StatusCode::kInternalInvariantViolation;
      decision.explanation.notes.push_back("pre-commit self-verification failed: " + reason);
      decision.digest = DecisionDigest(decision);
    }
  }
  return decision;
}

// Runs one attempt to its terminal state. Called by workers only: the
// synchronous path uses ComputeDecision followed by Commit.
void ProcessSlot(CoordinatorState& st, const std::shared_ptr<Slot>& slot) {
  const PlacementDecision decision = ComputeDecision(st, slot);
  {
    std::lock_guard<std::mutex> lock(st.mu);
    if (slot->terminal) return;
    slot->outcome.decision = decision;
  }
  if (!decision.placed()) {
    {
      std::lock_guard<std::mutex> lock(st.mu);
      FinalizeLocked(st, slot, AttemptState::kRejected, decision.code);
    }
    JournalTerminal(st, slot, AttemptPhase::kRejected, decision.code);
    return;
  }
  const Status status = CommitSlot(st, slot);
  if (status.ok()) return;
  const AttemptState state = StateForFailure(status.code());
  {
    std::lock_guard<std::mutex> lock(st.mu);
    FinalizeLocked(st, slot, state, status.code());
  }
  JournalTerminal(st, slot, PhaseForState(state), status.code());
}

std::shared_ptr<Slot> FindSlot(CoordinatorState& st, AttemptId attempt) {
  std::lock_guard<std::mutex> lock(st.mu);
  const auto it = st.slots.find(attempt);
  if (it == st.slots.end()) return nullptr;
  return it->second;
}

// Registers an attempt: validation, epoch binding, duplicate detection, durable
// attempt-start journaling, and slot creation.
Result<std::shared_ptr<Slot>> RegisterAttempt(CoordinatorState& st, PlacementRequest request,
                                              AttemptId attempt, bool enforce_queue_capacity) {
  if (!attempt.valid()) {
    return Status(StatusCode::kInvalidId, "attempt id must be non-zero");
  }
  if (request.attempt.valid() && request.attempt != attempt) {
    return Status(StatusCode::kInvalidArgument,
                  "request attempt id does not match the submitted attempt id");
  }
  const Status validation = ValidateRequest(request, st.options.limits);
  if (!validation.ok()) return validation;
  request.attempt = attempt;
  const Digest digest = RequestDigest(request, st.options.limits);

  std::shared_ptr<Slot> slot;
  {
    std::lock_guard<std::mutex> lock(st.mu);
    ++st.stats.submitted;
    if (st.state != RunState::kRunning) {
      ++st.stats.rejected_shutting_down;
      return Status(StatusCode::kShuttingDown, "coordinator is shutting down");
    }
    if (request.expected.fabric_epoch != st.epoch) {
      return Status(StatusCode::kStaleFabricEpoch,
                    "request is bound to a fabric epoch that is not current");
    }
    const auto existing = st.slots.find(attempt);
    if (existing != st.slots.end()) {
      if (existing->second->request_digest == digest) {
        ++st.stats.duplicate_replays;
        return Status(StatusCode::kAlreadyExists,
                      "attempt was already admitted with an identical request");
      }
      ++st.stats.duplicate_conflicts;
      return Status(StatusCode::kDuplicateAttemptConflict,
                    "attempt was already admitted with a different request");
    }
    if (enforce_queue_capacity && st.queue_depth >= st.options.queue_capacity) {
      ++st.stats.rejected_queue_full;
      return Status(StatusCode::kQueueFull, "placement queue is full");
    }
    slot = std::make_shared<Slot>();
    slot->attempt = attempt;
    slot->request = std::move(request);
    slot->request_digest = digest;
    slot->outcome.attempt = attempt;
    slot->outcome.state = AttemptState::kQueued;
    st.slots[attempt] = slot;
    st.slot_order.push_back(attempt);
    ++st.stats.accepted;
    ReapRetainedSlots(st);
  }

  if (st.store) {
    AttemptRecord record;
    record.attempt = attempt;
    record.incarnation = st.incarnation;
    record.epoch = slot->request.expected.fabric_epoch;
    record.flow = slot->request.flow;
    record.phase = AttemptPhase::kStarted;
    record.request_digest = slot->request_digest;
    record.observed_unix_nanos = CurrentUnixNanos();
    {
      std::lock_guard<std::mutex> lock(st.store_mu);
      const Status status = st.store->AppendAttemptStart(record);
      if (!status.ok()) {
        // The attempt is already finished durably (an idempotent replay across a
        // restart) or the store refused the write. Either way this attempt can
        // never commit; finalize it so no waiter is left without an outcome.
        std::lock_guard<std::mutex> slot_lock(st.mu);
        FinalizeLocked(st, slot, AttemptState::kRejected, status.code());
        if (status.code() == StatusCode::kAlreadyExists) {
          ++st.stats.duplicate_replays;
        }
        return status;
      }
    }
    bool journal_terminal = false;
    AttemptPhase phase = AttemptPhase::kCancelled;
    StatusCode code = StatusCode::kCancelled;
    {
      std::lock_guard<std::mutex> lock(st.mu);
      slot->journalled_start = true;
      if (slot->pending_terminal) {
        journal_terminal = true;
        phase = slot->pending_phase;
        code = slot->pending_code;
        slot->pending_terminal = false;
      }
    }
    if (journal_terminal) JournalTerminal(st, slot, phase, code);
  }
  return slot;
}

}  // namespace

struct PlacementCoordinator::Impl : CoordinatorState {};

PlacementCoordinator::PlacementCoordinator() : impl_(std::make_unique<Impl>()) {}

PlacementCoordinator::~PlacementCoordinator() {
  if (impl_ != nullptr) {
    static_cast<void>(Shutdown());
  }
}

Result<std::unique_ptr<PlacementCoordinator>> PlacementCoordinator::Start(
    CoordinatorOptions options) {
  auto coordinator = std::unique_ptr<PlacementCoordinator>(new PlacementCoordinator());
  Impl& st = *coordinator->impl_;
  st.options = options;
  st.options.worker_threads =
      std::max<std::uint32_t>(1, std::min(options.worker_threads, kMaxWorkerThreads));
  st.options.queue_capacity =
      std::max<std::uint64_t>(1, std::min(options.queue_capacity, kMaxQueueCapacity));
  st.options.store.max_total_records =
      std::max<std::uint64_t>(1, st.options.store.max_total_records);
  st.options.max_retained_attempts =
      std::max<std::uint64_t>(16, std::min(options.max_retained_attempts, kMaxQueueCapacity));
  st.worker_count = st.options.worker_threads;
  st.engine = PlacementEngine(st.options.limits);
  st.epoch = options.initial_epoch.valid() ? options.initial_epoch : FabricEpoch{1};
  st.incarnation =
      options.incarnation.valid() ? options.incarnation : AllocateIncarnation(options.seed);

  // The authority view starts unconstrained: a zero generation means "this
  // embedder has not declared a current generation for that domain". The
  // engine still enforces every generation the request itself declares, so no
  // placement is ever bound to an unchecked generation. SetAuthority installs
  // the embedder's view; from then on an attempt whose expectations no longer
  // match it is fenced at the commit boundary.
  st.authority = AuthorityExpectation{};
  st.authority.fabric_epoch = st.epoch;

  if (!st.options.store_path.empty()) {
    Result<PlacementStore> opened = PlacementStore::Open(st.options.store_path, st.options.store);
    if (!opened.ok()) return opened.status();
    st.store = std::make_unique<PlacementStore>(std::move(opened.value()));
  }

  const std::uint32_t worker_count = st.worker_count;
  st.workers.reserve(worker_count);
  for (std::uint32_t i = 0; i < worker_count; ++i) {
    st.workers.emplace_back([&st]() {
      for (;;) {
        std::shared_ptr<Slot> slot;
        {
          std::unique_lock<std::mutex> lock(st.mu);
          st.cv_work.wait(lock, [&st]() {
            return !st.queue.empty() || st.state != RunState::kRunning;
          });
          if (st.queue.empty()) {
            if (st.state != RunState::kRunning) break;
            continue;
          }
          const AttemptId id = st.queue.front();
          st.queue.pop_front();
          --st.queue_depth;
          const auto it = st.slots.find(id);
          if (it == st.slots.end()) {
            // The queue depth changed: waiters (Drain in particular) must be
            // woken even though this item produces no outcome.
            st.cv_work.notify_all();
            st.cv_done.notify_all();
            continue;
          }
          slot = it->second;
          if (slot->terminal) {
            // Cancelled while queued: the slot is already terminal and the
            // queue depth changed, so signal both condition variables.
            st.cv_work.notify_all();
            st.cv_done.notify_all();
            continue;
          }
          slot->outcome.state = AttemptState::kRunning;
          slot->in_worker = true;
          ++st.active;
        }
        ProcessSlot(st, slot);
        {
          std::lock_guard<std::mutex> lock(st.mu);
          slot->in_worker = false;
          if (st.active > 0) --st.active;
          st.cv_done.notify_all();
          st.cv_work.notify_all();
        }
      }
    });
  }
  return coordinator;
}

Status PlacementCoordinator::Submit(PlacementRequest request, AttemptId attempt) {
  Impl& st = *impl_;
  Result<std::shared_ptr<Slot>> registered =
      RegisterAttempt(st, std::move(request), attempt, /*enforce_queue_capacity=*/true);
  if (!registered.ok()) return registered.status();
  std::lock_guard<std::mutex> lock(st.mu);
  if (st.state != RunState::kRunning) {
    ++st.stats.rejected_shutting_down;
    FinalizeLocked(st, registered.value(), AttemptState::kCancelled, StatusCode::kShuttingDown);
    return Status(StatusCode::kShuttingDown, "coordinator is shutting down");
  }
  st.queue.push_back(attempt);
  ++st.queue_depth;
  st.cv_work.notify_one();
  return Status::Ok();
}

Result<PreparedAttempt> PlacementCoordinator::Prepare(PlacementRequest request,
                                                      AttemptId attempt) {
  Impl& st = *impl_;
  Result<std::shared_ptr<Slot>> registered =
      RegisterAttempt(st, std::move(request), attempt, /*enforce_queue_capacity=*/false);
  if (!registered.ok()) return registered.status();
  const std::shared_ptr<Slot> slot = registered.value();
  {
    std::lock_guard<std::mutex> lock(st.mu);
    if (slot->terminal) {
      // The attempt reached a terminal state while the start record was being
      // journaled (cancelled or fenced). Its outcome is already final and must
      // never be resurrected into RUNNING.
      return Status(slot->outcome.state == AttemptState::kCommitted
                        ? StatusCode::kAlreadyCommitted
                        : StatusCode::kAlreadyExists,
                    "attempt already reached a terminal state");
    }
    slot->running = true;
    slot->outcome.state = AttemptState::kRunning;
  }
  const PlacementDecision decision = ComputeDecision(st, slot);
  {
    std::lock_guard<std::mutex> lock(st.mu);
    slot->outcome.decision = decision;
  }
  PreparedAttempt prepared;
  prepared.attempt = attempt;
  prepared.request = slot->request;
  {
    std::lock_guard<std::mutex> lock(st.mu);
    prepared.decision = slot->outcome.decision;
  }
  return prepared;
}

Result<AttemptOutcome> PlacementCoordinator::Commit(PreparedAttempt prepared) {
  Impl& st = *impl_;
  const std::shared_ptr<Slot> slot = FindSlot(st, prepared.attempt);
  if (slot == nullptr) {
    return Status(StatusCode::kUnknownAttempt, "no such attempt is known to this coordinator");
  }
  if (!prepared.decision.placed()) {
    bool finalized_here = false;
    {
      std::lock_guard<std::mutex> lock(st.mu);
      if (!slot->terminal) {
        slot->outcome.decision = prepared.decision;
        FinalizeLocked(st, slot, AttemptState::kRejected, prepared.decision.code);
        finalized_here = true;
      }
    }
    if (finalized_here) JournalTerminal(st, slot, AttemptPhase::kRejected, prepared.decision.code);
    std::lock_guard<std::mutex> lock(st.mu);
    return slot->outcome;
  }
  PlacementDecision authoritative;
  {
    std::lock_guard<std::mutex> lock(st.mu);
    if (slot->terminal) return slot->outcome;
    // The decision that commits is the one this coordinator computed for this
    // slot's request; a caller-supplied decision is only accepted when it is
    // identical, so no unverified intent can reach durable state.
    if (slot->outcome.decision.digest != prepared.decision.digest) {
      return Status(StatusCode::kInvalidArgument,
                    "prepared decision does not match the decision computed for this attempt");
    }
    authoritative = slot->outcome.decision;
  }
  (void)authoritative;
  const Status status = CommitSlot(st, slot);
  if (!status.ok()) {
    bool finalized_here = false;
    const AttemptState state = StateForFailure(status.code());
    {
      std::lock_guard<std::mutex> lock(st.mu);
      if (!slot->terminal) {
        FinalizeLocked(st, slot, state, status.code());
        finalized_here = true;
      }
    }
    if (finalized_here) JournalTerminal(st, slot, PhaseForState(state), status.code());
  }
  std::lock_guard<std::mutex> lock(st.mu);
  return slot->outcome;
}

Result<AttemptOutcome> PlacementCoordinator::PlaceNow(PlacementRequest request, AttemptId attempt) {
  Result<PreparedAttempt> prepared = Prepare(std::move(request), attempt);
  if (!prepared.ok()) return prepared.status();
  return Commit(std::move(prepared.value()));
}

Result<AttemptOutcome> PlacementCoordinator::Await(AttemptId attempt) {
  Impl& st = *impl_;
  std::unique_lock<std::mutex> lock(st.mu);
  const auto it = st.slots.find(attempt);
  if (it == st.slots.end()) {
    return Status(StatusCode::kUnknownAttempt, "no such attempt is known to this coordinator");
  }
  const std::shared_ptr<Slot> slot = it->second;
  st.cv_done.wait(lock, [&st, &slot]() { return slot->terminal || st.state == RunState::kStopped; });
  if (!slot->terminal) {
    // The coordinator stopped without finalizing this attempt: it can never
    // commit now, so report a terminal failure instead of blocking forever.
    FinalizeLocked(st, slot, AttemptState::kFailed, StatusCode::kShuttingDown);
  }
  return slot->outcome;
}

Status PlacementCoordinator::Cancel(AttemptId attempt) {
  Impl& st = *impl_;
  std::shared_ptr<Slot> slot;
  bool journal_now = false;
  {
    std::lock_guard<std::mutex> lock(st.mu);
    const auto it = st.slots.find(attempt);
    if (it == st.slots.end()) {
      return Status(StatusCode::kUnknownAttempt, "no such attempt is known to this coordinator");
    }
    slot = it->second;
    if (slot->terminal) {
      return Status(slot->outcome.state == AttemptState::kCommitted ? StatusCode::kAlreadyCommitted
                                                                   : StatusCode::kAlreadyExists,
                    "attempt already reached a terminal state");
    }
    if (slot->commit_boundary_crossed) {
      return Status(StatusCode::kAlreadyCommitted,
                    "the commit boundary was already crossed; the placement stands");
    }
    slot->cancel_requested = true;
    if (!slot->running && !slot->in_worker) {
      FinalizeLocked(st, slot, AttemptState::kCancelled, StatusCode::kCancelled);
      if (slot->journalled_start) {
        journal_now = true;
      } else {
        // The registering thread owns the journal for this attempt until its
        // start record is durable; it journals this terminal phase afterwards.
        slot->pending_terminal = true;
        slot->pending_phase = AttemptPhase::kCancelled;
        slot->pending_code = StatusCode::kCancelled;
      }
    }
  }
  if (journal_now) {
    JournalTerminal(st, slot, AttemptPhase::kCancelled, StatusCode::kCancelled);
  }
  return Status::Ok();
}

Status PlacementCoordinator::AdvanceEpoch() {
  Impl& st = *impl_;
  std::lock_guard<std::mutex> commit_lock(st.commit_mu);
  std::lock_guard<std::mutex> lock(st.mu);
  if (!st.epoch.Advance()) {
    return Status(StatusCode::kArithmeticOverflow, "fabric epoch is exhausted");
  }
  st.authority.fabric_epoch = st.epoch;
  return Status::Ok();
}

FabricEpoch PlacementCoordinator::epoch() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->epoch;
}

IncarnationId PlacementCoordinator::incarnation() const noexcept { return impl_->incarnation; }

Status PlacementCoordinator::SetAuthority(AuthorityExpectation expected) {
  Impl& st = *impl_;
  std::lock_guard<std::mutex> commit_lock(st.commit_mu);
  std::lock_guard<std::mutex> lock(st.mu);
  if (expected.fabric_epoch < st.epoch) {
    return Status(StatusCode::kStaleFabricEpoch,
                  "authority view is bound to a superseded fabric epoch");
  }
  st.epoch = expected.fabric_epoch;
  st.authority = expected;
  return Status::Ok();
}

AuthorityExpectation PlacementCoordinator::authority() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->authority;
}

Result<RevalidationResult> PlacementCoordinator::Revalidate(PlacementRequest request) {
  Impl& st = *impl_;
  const Status validation = ValidateRequest(request, st.options.limits);
  if (!validation.ok()) return validation;

  RevalidationResult result;
  result.decision = st.engine.Place(request);
  if (st.store) {
    std::lock_guard<std::mutex> lock(st.store_mu);
    Result<std::optional<PlacementRecord>> previous = st.store->FindLatest(request.flow);
    if (!previous.ok()) return previous.status();
    if (previous.value().has_value()) result.previous = previous.value().value();
  } else {
    std::lock_guard<std::mutex> lock(st.mu);
    const auto it = st.latest_in_memory.find(request.flow);
    if (it != st.latest_in_memory.end()) result.previous = it->second;
  }

  if (!result.previous.has_value()) {
    result.verdict = RevalidationVerdict::kUnknownFlow;
    return result;
  }
  if (result.decision.outcome == Outcome::kStaleInput) {
    result.verdict = RevalidationVerdict::kStaleInput;
    return result;
  }
  if (!result.decision.placed()) {
    result.verdict = RevalidationVerdict::kRejected;
    return result;
  }
  const PlacementRecord& previous = *result.previous;
  if (result.decision.intent->path == previous.intent.path &&
      result.decision.intent->path_authority == previous.intent.path_authority) {
    result.verdict = RevalidationVerdict::kStillCurrent;
  } else {
    result.verdict = RevalidationVerdict::kMustMove;
  }
  return result;
}

const Limits& PlacementCoordinator::limits() const noexcept { return impl_->options.limits; }

Result<std::vector<PlacementRecord>> PlacementCoordinator::LatestPlacements() {
  Impl& st = *impl_;
  if (st.store) {
    std::lock_guard<std::mutex> lock(st.store_mu);
    return st.store->LatestPlacements();
  }
  std::lock_guard<std::mutex> lock(st.mu);
  std::vector<PlacementRecord> out;
  out.reserve(st.latest_in_memory.size());
  for (const auto& pair : st.latest_in_memory) out.push_back(pair.second);
  return out;
}

RecoveryReport PlacementCoordinator::recovery() const {
  Impl& st = *impl_;
  if (st.store) {
    std::lock_guard<std::mutex> lock(st.store_mu);
    return st.store->recovery();
  }
  RecoveryReport report;
  report.liveness_restored = false;
  report.detail = "in-memory coordination: no durable state exists to recover";
  return report;
}

CoordinatorStats PlacementCoordinator::stats() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  CoordinatorStats stats = impl_->stats;
  stats.queued = impl_->queue_depth;
  stats.active = impl_->active;
  return stats;
}

bool PlacementCoordinator::running() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->state == RunState::kRunning;
}

std::uint32_t PlacementCoordinator::worker_count() const noexcept { return impl_->worker_count; }

Status PlacementCoordinator::Drain() {
  Impl& st = *impl_;
  std::unique_lock<std::mutex> lock(st.mu);
  st.cv_work.wait(lock, [&st]() { return st.queue_depth == 0 && st.active == 0; });
  return Status::Ok();
}

Status PlacementCoordinator::Shutdown() { return ShutdownInternal(false); }

Status PlacementCoordinator::ShutdownCancelling() { return ShutdownInternal(true); }

Status PlacementCoordinator::ShutdownInternal(bool cancel_active) {
  Impl& st = *impl_;
  std::vector<AttemptId> journal_cancelled;
  {
    std::unique_lock<std::mutex> lock(st.mu);
    if (st.state == RunState::kStopped) return Status::Ok();
    if (st.state == RunState::kShuttingDown) {
      // Another thread owns the teardown; wait for it instead of joining the
      // same workers twice.
      st.cv_done.wait(lock, [&st]() { return st.state == RunState::kStopped; });
      return Status::Ok();
    }
    st.state = RunState::kShuttingDown;

    // Queued work is cancelled first so that no worker picks it up.
    for (const AttemptId id : st.queue) {
      const auto it = st.slots.find(id);
      if (it == st.slots.end() || it->second->terminal) continue;
      FinalizeShutdownSlot(st, it->second, journal_cancelled);
    }
    st.queue.clear();
    st.queue_depth = 0;

    // Every other slot that is not owned by a worker is finalized here: a
    // prepared attempt has no worker to finish it, and a slot whose boundary was
    // already crossed is left alone because its commit is in flight.
    for (auto& pair : st.slots) {
      const std::shared_ptr<Slot>& candidate = pair.second;
      if (candidate->terminal || candidate->in_worker || candidate->commit_boundary_crossed) {
        continue;
      }
      if (!candidate->running || cancel_active) {
        FinalizeShutdownSlot(st, candidate, journal_cancelled);
      }
    }
    st.cv_work.notify_all();
    st.cv_done.notify_all();
  }
  for (const AttemptId id : journal_cancelled) {
    const std::shared_ptr<Slot> slot = FindSlot(st, id);
    if (slot != nullptr) {
      JournalTerminal(st, slot, AttemptPhase::kCancelled, StatusCode::kShuttingDown);
    }
  }
  for (std::thread& worker : st.workers) {
    if (worker.joinable()) worker.join();
  }
  {
    std::lock_guard<std::mutex> lock(st.mu);
    st.state = RunState::kStopped;
    st.cv_done.notify_all();
    st.cv_work.notify_all();
  }
  if (st.store) {
    std::lock_guard<std::mutex> lock(st.store_mu);
    return st.store->Close();
  }
  return Status::Ok();
}

}  // namespace flowplace

# Flow Placement — design and decision semantics

This document states the rules the implementation actually applies. Every rule here is exercised by
the test suite.

## 1. Boundary

The engine owns per-flow path choice from an already-authorized candidate set. It never computes a
path, never establishes path legality, never owns route state, never sets path weights, never
performs adaptive routing or global traffic engineering, never schedules flows in time, never
reserves bandwidth, never enforces rates, and never installs forwarding state. A placement is an
**intent** bound to the generations that justified it.

## 2. Identities, generations and authority

All identities are distinct types over `std::uint64_t`; identity 0 means "absent". Generations are
monotone counters whose advancement is checked: a counter that would wrap reports failure instead.
The authority vector that binds a decision is:

```
PathAuthorityGeneration | CandidateSetId/Generation | CapacitySnapshotId/Generation
PolicyId/Generation | QosProfileId/Generation | EvidenceId/Generation | FabricEpoch
```

A request must declare the generations it expects. A request that omits them is malformed
(`MISSING_FIELD`); it is never silently bound to whatever the facts happen to carry.

## 3. Evaluation order

```
1. structural validation                       -> CONFLICTING_INPUT / (validation status)
2. request-level generation staleness          -> STALE_INPUT
3. per-candidate hard exclusions (fixed order) -> exclusions, in order, first failure recorded
4. classification of an empty legal set        -> STALE_INPUT | POLICY_REJECTED |
                                                  INSUFFICIENT_CAPACITY | NO_LEGAL_PATH
5. failure-domain diversity over legal set     -> NO_LEGAL_PATH (FAILURE_DOMAIN_DIVERSITY_UNMET)
6. deterministic lexicographic ranking
7. churn (incumbent stability) decision        -> PLACED | DEFERRED
8. admission comfort gate                      -> PLACED_DEGRADED | DEFERRED
9. self-verification of the chosen intent      -> CONFLICTING_INPUT (INTERNAL_INVARIANT_VIOLATION)
```

### 3.1 Hard exclusion order (one reason recorded per candidate)

The evaluation order is exactly the order of the `ExclusionReason` enumerators, so the recorded
reason, the recorded example constraint values, and the resulting outcome code are a function of the
candidate alone:

```
 0 path_authority_stale              9 capacity_unknown
 1 tier_not_allowed                 10 capacity_insufficient
 2 forbidden_label                  11 service_headroom_unmet
 3 missing_required_label           12 service_reservation_unmet
 4 forbidden_locality               13 reservation_affinity_unsatisfied
 5 locality_mismatch                14 latency_budget_exceeded
 6 forbidden_failure_domain         15 evidence_missing
 7 required_failure_domain_mismatch 16 evidence_stale
 8 failure_domain_occupied
```

The order is fixed so that the reported reason is a function of the candidate, not of the order in
which constraints were configured. Missing capacity evidence is UNKNOWN and never treated as
headroom: a path with no capacity entry is excluded with `capacity_unknown`, and a request whose
candidates all lack capacity evidence yields `INSUFFICIENT_CAPACITY` with code `NOT_FOUND`, whose
explanation says that absence is not capacity.

### 3.2 Classification of an empty legal set

| Condition | Outcome | Code |
| --- | --- | --- |
| candidate set is empty | `NO_LEGAL_PATH` | `NOT_FOUND` |
| every candidate excluded for path-authority staleness | `STALE_INPUT` | `STALE_AUTHORITY_GENERATION` |
| every exclusion reason is a staleness reason | `STALE_INPUT` | `STALE_EVIDENCE` |
| every exclusion reason is a policy reason | `POLICY_REJECTED` | `POLICY_REJECTED_ALL_PATHS` |
| every exclusion reason is a service-class reason | `NO_LEGAL_PATH` | `SERVICE_CLASS_UNSATISFIED` |
| every exclusion reason is a capacity reason | `INSUFFICIENT_CAPACITY` | `CAPACITY_INSUFFICIENT` (all unknown: `NOT_FOUND`) |
| every exclusion reason is a latency reason | `NO_LEGAL_PATH` | `LATENCY_BUDGET_UNMET` |
| mixed categories | `NO_LEGAL_PATH` | `NOT_FOUND` |

## 4. Service classes

| Service class | Headroom multiplier | Reservation required |
| --- | --- | --- |
| `best_effort` | 1x the flow requirement | no |
| `controlled` | 2x | no |
| `assured` | 3x | no |
| `reserved` | 3x | yes (the path must carry a reservation reference) |

The flow's own `required_residual_bytes` is never relaxed. Relaxation is explicit and bounded:
step 1 drops the multiplier, step 2 additionally drops the service-class reservation requirement.
A candidate legal only through relaxation produces `PLACED_DEGRADED` with the `service_relaxed`
flag and a binding constraint that records the required and available headroom.

## 5. Ranking

The policy states an ordered objective list; the engine applies no implicit default. Candidates are
compared lexicographically over the objective keys (direction-aware), and the path identity is the
final tie-break, which makes the order total. Congestion utilization is the only objective that can
be UNKNOWN; UNKNOWN sorts after every known value in both directions and is never coerced to zero.
No weights are multiplied, no floating point is used, and all externally influenced arithmetic is
checked.

## 6. Incumbent, churn and deferral

An incumbent is **eligible** only when all of its generations match the current facts and its path
is still in the authorized candidate set. An ineligible incumbent is never re-affirmed: the
decision either moves (`replaced_stale`) or defers; a stale incumbent never survives a generation
change.

With `prefer_incumbent`, the best candidate must improve on a legal incumbent by more than
`move_improvement_threshold` on the configured objective, otherwise the incumbent is kept and the
`churn_suppressed` flag is set. With `on_move = defer`, a required move becomes `DEFERRED`
(`POLICY_MOVE_SUPPRESSED`) and no intent is produced.

The incumbent delta is reported by every outcome, including ones that produce no placement: an
ineligible incumbent yields `replaced_stale` (even when the chosen path happens to be the
incumbent's path) and a decidable-but-not-yet-placed flow yields the relation that applies now.

`DEFERRED` has exactly two producers: the churn move gate and the admission comfort gate
(`POLICY_HEADROOM_GATE`).

## 7. Outcome degradation

`PLACED_DEGRADED` is produced when at least one *degrading* flag is present: service relaxed,
comfort gate crossed, locality preference unmet, reservation preference unmet, or a ranking key of
the chosen candidate was UNKNOWN. Informational flags (incumbent stale, churn suppressed, no
incumbent, incumbent superseded) do not degrade the outcome.

## 8. Determinism

* Unordered collections are canonically ordered before they influence anything: the candidate index,
  capacity and evidence lookups, exclusion samples, exclusion example values, and request digests.
* The decision digest covers the outcome, code, intent, authority vector, incumbent delta, ranking
  prefix, exclusion counters and samples, degradations, binding constraints, notes, and validation
  code. Equal digests mean equal decisions.
* The placement *intent* digest excludes nothing that the placement decided; the attempt identity is
  part of the intent, so two attempts that decide the same thing have equal intents after
  normalising the attempt field.
* Placement identity (`PlacementId`, `PlacementGeneration`) is allocated at commit time and is
  deliberately not part of the intent: intent is deterministic, identity allocation is not.

## 9. Commit boundary and fencing

```
validate -> bind authority -> plan -> journal attempt start -> append placement -> sync
        -> verify -> report committed
```

The commit boundary re-checks, under one lock:

1. the attempt is not terminal;
2. cancellation has not been observed;
3. the coordinator is not shutting down;
4. the attempt's fabric epoch is still the current epoch;
5. the coordinator's authority view still matches the attempt's expectations (a zero field in the
   view means "this embedder has not declared that generation").

Failing any of these prevents the durable mutation and reports `FENCED` or `CANCELLED`.
Cancellation observed *after* the boundary is reported as `ALREADY_COMMITTED` and does not undo
durable state.

Lock order is strictly `commit_mu -> store_mu -> mu` (and `commit_mu -> mu`); no path acquires
`mu` before `commit_mu`, no callback or user code runs under any lock, and shutdown never joins a
worker while holding a lock a worker needs which is reachable in the other order.

The coordinator's authority view starts **unconstrained**: a zero generation means "this embedder
has not declared that generation". The engine still enforces every generation the request itself
declares, so no placement is bound to an unchecked generation; `SetAuthority` installs the
embedder's view and from then on a mismatch fences the attempt.

Shutdown is single-owner: the first caller performs the teardown, later callers wait for it. Every
slot that is not owned by a worker - queued, or prepared and never committed - is finalized during
shutdown, so no waiter can be left without a terminal state, and a slot whose commit boundary was
already crossed is never re-labelled as cancelled. Attempt records are retained in a bounded table
(`CoordinatorOptions::max_retained_attempts`); an attempt that has been reaped is reported as
unknown rather than silently replayed.

## 10. Durability and recovery

### 10.1 Growth bounds

The journal is folded into a snapshot once it reaches `StoreOptions::max_journal_records`, so the
journal length - and therefore the work a crash can lose - is bounded by configuration rather than
by uptime. Compaction writes a journal-reset marker as the first record of the fresh journal; the
marker carries the sequence the snapshot covers, which makes the sequence rebase crash-safe in both
directions: a journal that was replaced is recognised, and a journal that was *not* yet replaced is
read with the plain "already covered by the snapshot" rule. Attempt records are retained only for a
bounded window; records that age out are counted in `RecoveryReport::retired_attempts` and never
affect durable placements or generations. Compaction runs only after the caller has applied the
record it just appended to the in-memory state, so a snapshot can never be written ahead of the
record it folds in.

* Single append-only journal, 28-byte header + payload + 8-byte trailer, CRC-32C on payload and on
  the whole record, explicit little-endian encoding, per-record format version.
* A commit is acknowledged only after the bytes are durable (`FlushFileBuffers`/`_commit` on
  Windows, `fsync` elsewhere when `fsync_on_append` is set).
* Compaction writes a snapshot to a temporary file, syncs it, atomically replaces the snapshot, then
  atomically resets the journal: the snapshot is durable before the journal is replaced.
* Recovery is fail-closed: damage followed by a structurally valid record is `STORE_CORRUPT`; a
  torn tail is truncated and reported with the exact discarded byte count; a start record after a
  terminal record for the same attempt is corruption; an acknowledged attempt with no durable
  placement is corruption.
* Recovery never restores liveness, leases, worker authority, or telemetry freshness
  (`liveness_restored` is always false). Recovered placements are marked
  `requires_revalidation`, and `Revalidate` re-derives the correct choice from current evidence
  and reports `STILL_CURRENT`, `MUST_MOVE`, `REJECTED`, `STALE_INPUT`, or `UNKNOWN_FLOW`.

## 11. Bounds

Every externally influenced dimension is bounded: candidate paths, capacity and evidence entries,
occupied domains, objectives, path labels, path reservations, policy id lists, affinity references,
string lengths, ranked explanation prefix, exclusion samples, exclusion reasons, notes, binding
constraints, record size, journal length, total durable records, retained attempt records, retained
coordinator attempts, worker threads, queue capacity, scenario document size, scenario line length,
scenario keys per directive, and frame size.
Exceeding a bound is a rejection with a precise code, never a truncation. Explanations stay bounded
at a million candidates, and counts that cannot fit a bounded sample are reported as exact counters
plus a bounded sample of the lowest path identities.

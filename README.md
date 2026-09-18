# Flow Placement

**Open-source, vendor-neutral C++20 runtime for deterministic, generation-bound placement of admitted network flows onto already-authorized candidate paths under locality, capacity, policy, and service constraints.**

Version 1.0.0 — Copyright 2026 Summon Software Labs — Apache License 2.0.

Given an admitted flow, an exact set of already-authorized candidate paths, current capacity and
service evidence, a placement policy, locality and failure-domain constraints, and the generations
that bind them, Flow Placement answers one question:

> Which path should carry this flow now, why, and when must that placement be rejected, moved,
> revalidated, superseded, or fenced as stale?

## Boundary

Flow Placement owns **per-flow path choice from an already-legal candidate set**. It does not:

* compute paths or establish path legality (the candidate set arrives authorized, with the path
  authority generation that authorizes it);
* own route state, path weights, or forwarding state;
* optimize traffic engineering globally or adaptively reroute;
* schedule flows in time, reserve bandwidth, or enforce rates;
* install anything into a data plane.

A `ReservationRef` is a *reference* to a reservation someone else established; this library never
creates, modifies, or enforces one. Placement is an **intent** bound to the generations that
justified it, not an installation.

## What it does

* **Deterministic ranking.** A policy declares an ordered objective list; candidates are ranked by a
  lexicographic key over those objectives with the path identity as the final tie-break. There is no
  weight arithmetic, no floating point, and no implicit default objective order.
* **Hard exclusions are separate from soft preferences.** Path authority staleness, tier, labels,
  locality, failure domain, occupancy, latency budget, reservation affinity, capacity evidence, and
  service-class requirements are evaluated in a fixed, documented order; comfort gates, locality
  affinity, reservation affinity and service relaxation are preferences that produce a *degraded*
  but legal placement.
* **Canonical decisions.** Unordered inputs (candidate order, capacity order, evidence order, label
  and reservation order, occupied-domain order) never change the decision, the chosen path, or the
  decision digest. Identical inputs produce identical decisions, including on concurrent callers.
* **Bounded explanations.** Every decision reports the ranking prefix, exact exclusion counts with
  a bounded sample, degradation flags, binding constraints, the intent, the incumbent delta, and
  the full authority vector. Explanations stay bounded at a million candidates.
* **Durable intent.** Committed placements are journalled in a versioned, CRC-32C checked,
  crash-safe store with atomic snapshot compaction; recovery distinguishes durable history,
  committed state, unfinished attempts, and evidence that must be revalidated.
* **Fenced runtime.** A bounded worker pool and bounded queue coordinate placement under a fabric
  epoch and process incarnation; the commit boundary re-checks cancellation, epoch and authority,
  so cancelled, fenced, or superseded work never mutates authoritative state.
* **Real multiprocess transport.** A coordinator process and worker processes exchange bounded,
  integrity-checked frames over TCP; a stale-epoch worker is rejected at the handshake.

## Requirements

* CMake 3.20 or newer, a C++20 compiler, and a threading library. No third-party dependencies.
* Validated on Windows with MSVC 19.44 (Visual Studio 2022, x64, Debug and Release, `/W4 /WX`,
  AddressSanitizer, and `/analyze`). The sources also build with GCC and Clang warning sets
  (`-Wall -Wextra -Wpedantic -Wshadow -Wcast-qual -Wconversion -Wsign-conversion -Werror`), but the
  POSIX socket and process paths have **not** been executed on this machine — see
  `docs/VALIDATION.md` for the exact REAL/SYNTHETIC/UNSUPPORTED surface.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful options: `FLOWPLACE_BUILD_TESTS`, `FLOWPLACE_BUILD_CLI`, `FLOWPLACE_INSTALL`,
`FLOWPLACE_WARNINGS_AS_ERRORS`, `FLOWPLACE_ENABLE_ASAN`, `FLOWPLACE_ENABLE_ANALYZE`.

## Install and consume

```sh
cmake --install build --prefix /some/prefix
```

```cmake
find_package(flowplace 1.0 REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE flowplace::flowplace)
```

The `tests/consumer` project is an independent downstream consumer used to validate the exported
package.

## Library use

```cpp
#include "flowplace/engine.hpp"

flowplace::PlacementEngine engine;
flowplace::PlacementDecision decision = engine.Place(request);

switch (decision.outcome) {
  case flowplace::Outcome::kPlaced:
  case flowplace::Outcome::kPlacedDegraded:
    // decision.intent->path, bound to decision.authority
    break;
  default:
    // decision.code names the precise blocker; decision.explanation explains it
    break;
}
```

The engine is pure and stateless: it performs no I/O, never throws, and is safe to call
concurrently. Durable commits, epochs, cancellation and fencing live in
`flowplace::PlacementCoordinator`; see `include/flowplace/runtime.hpp`.

## Outcomes

| Outcome | Meaning |
| --- | --- |
| `PLACED` | A legal candidate was chosen; every soft preference was satisfied and no service requirement needed relaxing. |
| `PLACED_DEGRADED` | A legal candidate was chosen, but a documented degradation applies (service relaxation, comfort gate crossed, locality or reservation preference unmet, or a ranking key was UNKNOWN). |
| `DEFERRED` | Policy declines to decide now: the admission comfort gate is crossed with a defer action, or the churn policy forbids the required move. |
| `NO_LEGAL_PATH` | No candidate survived the hard constraints (or the diversity requirement failed, or the latency budget excluded everything). |
| `INSUFFICIENT_CAPACITY` | Every candidate failed a capacity constraint against supplied evidence. Absence of capacity evidence is not capacity: it is reported as unknown, never as headroom. |
| `POLICY_REJECTED` | Policy removed every candidate that is still authorized. |
| `STALE_INPUT` | The request, the evidence, or every candidate path is bound to a superseded generation or epoch. |
| `CONFLICTING_INPUT` | The request is malformed, oversized, or internally contradictory (duplicate path identities, two authority generations for one path, a required locality that is also forbidden, an unsatisfiable diversity requirement, unrepresentable arithmetic). |

## CLI

```
flowplace version
flowplace validate  --scenario FILE
flowplace place     --scenario FILE [--store FILE] [--no-commit]
flowplace explain   --scenario FILE [--store FILE] [--no-commit]
flowplace roundtrip --scenario FILE
flowplace bench     [--fanout N] [--iterations N] [--seed N] ...
flowplace serve     --store FILE [--port N] [--workers N] [--epoch N] [--max-requests N]
flowplace worker    --scenario FILE --port N [--repeat N] [--epoch N] [--expect-committed N]
flowplace store-inspect --store FILE
flowplace store-compact --store FILE
```

Exit codes: `0` success or placed, `1` usage/IO/protocol error, `2` a non-placed outcome,
`3` rejected worker handshake. Every command accepts `--report FILE` to write its report to a file.

## Storage format

The durable store is a single append-only journal with an optional snapshot:

```
0  u32 magic 'FPRC'      12 u64 sequence          28 payload
4  u32 format version    20 u32 payload length    +  u32 record CRC-32C
8  u16 record type       24 u32 payload CRC-32C   +  u32 end magic 'FEND'
```

A record is durable before it is acknowledged. Recovery is fail-closed: damage followed by valid
records is an error, a torn tail is truncated and reported with its exact byte count, and durable
state never restores liveness, leases, worker authority, or telemetry freshness.

## Repository layout

```
include/flowplace/   public headers (engine, model, explain, store, runtime, wire, transport, ...)
src/                 implementation
apps/                the flowplace command line tool
tests/               unit, property, adversarial, concurrency, persistence, multiprocess suites
tests/consumer/      independent find_package consumer
docs/                design and validation record
scripts/             developer and release helpers
```

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

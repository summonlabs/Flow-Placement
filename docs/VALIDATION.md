# Flow Placement 1.0.0 - validation record

Everything in this file was produced by the commands shown, on the machine described below. Nothing
here is projected or estimated.

## Environment

| Item | Value |
| --- | --- |
| Host | Windows 10/11 x64, single machine |
| Compiler | MSVC 19.44.35222 (Visual Studio 2022 Build Tools, toolset 14.44.35207) |
| CMake | 4.3.2, Ninja generator |
| Configurations | Debug, Release, Debug + AddressSanitizer, Release + `/analyze` |

## Proof surface: REAL / SYNTHETIC / UNSUPPORTED

**REAL (measured on this machine, by these commands)**

* The library, the command line tool, the test suite, the installed package, and an independent
  downstream `find_package` consumer build and run.
* Durable storage: real files, real `FlushFileBuffers`/`_commit` syncs, real truncation and
  corruption injected into real journal files, real recovery.
* Multiprocess coordination: the coordinator and its workers are separate operating system
  processes, exchanging bounded, CRC-checked frames over real TCP sockets on `127.0.0.1`. A
  coordinator process is killed with `TerminateProcess` and its store is reopened afterwards.
* Threading: real worker threads, real concurrent submitters, cancellers, and epoch advancers.
* AddressSanitizer: the whole suite runs under `/fsanitize=address` with no findings.
* MSVC static analysis: `/analyze` with `/WX` reports no first-party findings.

**SYNTHETIC (in-process generated populations, never network measurements)**

* `flowplace bench` generates candidate sets, capacity evidence, congestion evidence, incumbents,
  and policies in process. Its output is labelled `SYNTHETIC` and the renderer states that it is not
  a network measurement. No result from it describes a physical network.
* The randomized property tests generate requests from a seeded PRNG; they are synthetic inputs to a
  deterministic function, not traffic.

**UNSUPPORTED (explicitly not validated here)**

* No physical network, switch, NIC, DPU, RDMA, NVLink, or optical fabric was involved in any test.
  Path attributes, path authorities, capacity snapshots, congestion evidence, and reservations are
  all supplied by the caller; this library never measures them.
* The POSIX socket and process paths compile but were not executed on this host: every multiprocess
  and transport test ran on Windows. `TryWaitForExit`, `SpawnProcess`, and `TcpListener`/
  `TcpConnection` contain POSIX implementations that are unvalidated here.
* GCC and Clang builds were not executed on this host; the warning set they receive is configured in
  `CMakeLists.txt` but only MSVC was run.
* ThreadSanitizer is not available with this toolset; data races were addressed by design review,
  by locking discipline (documented lock order), and by functional concurrency tests rather than by
  a race detector.

## Commands and results

```
# Debug
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/dev
build/dev/flowplace_tests.exe                     -> PASSED 152/152 tests

# Release
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
build/release/flowplace_tests.exe                 -> PASSED 152/152 tests

# AddressSanitizer (Debug)
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFLOWPLACE_ENABLE_ASAN=ON
cmake --build build/asan
build/asan/flowplace_tests.exe                    -> PASSED 152/152 tests, no sanitizer reports

# Static analysis (Release, /W4 /WX /analyze)
cmake -S . -B build/analyze -G Ninja -DCMAKE_BUILD_TYPE=Release -DFLOWPLACE_ENABLE_ANALYZE=ON
cmake --build build/analyze                       -> no first-party findings

# Install and downstream consumer
cmake --install build/release --prefix build/install
scripts\validate_consumer.cmd                     -> flowplace_consumer: OK
```

Test distribution: 152 tests across 13 suites - unit (identities, digests, matrices of hard and soft
constraints, staleness, policy), property (seeded determinism, order independence, reentrancy,
verification of tampered intents), adversarial (grammar, truncation, single-byte corruption, random
byte fuzzing of every decoder, frame chunking, hostile sizes), codec/wire round trips, durable store
(commit, replay, torn tail, mid-file damage, version mismatch, compaction, growth bounds, read-only),
coordinator runtime (fencing, cancellation, duplicates, shutdown, revalidation), concurrency (barrier
load, epoch churn, cancellation under load, bounded queue), multiprocess (TCP worker/coordinator,
stale-epoch handshake rejection, killed coordinator recovery), and scenario/benchmark.

## Synthetic benchmark (Release, x64, this machine)

`flowplace bench`, 300 completed placements per point, policy with 4 objectives, churn pressure 25%:

| fan-out | p50 | p99 | placements/s |
| --- | --- | --- | --- |
| 2 | 3.1 us | 11.1 us | 274,499 |
| 8 | 6.7 us | 10.5 us | 143,844 |
| 32 | 18.3 us | 104.3 us | 36,193 |
| 64 | 33.9 us | 163.3 us | 23,652 |

`flowplace bench`, 40 completed placements per point, policy with 6 objectives, churn pressure 50%:

| fan-out | p50 | p99 | placements/s |
| --- | --- | --- | --- |
| 256 | 166.9 us | 195.5 us | 5,960 |
| 1024 | 842.5 us | 7.54 ms | 885 |
| 4096 | 3.35 ms | 8.93 ms | 261 |

These numbers measure **completed placements**: a decision fully computed, self-verified, and (when
committed through the coordinator) made durable. They are synthetic in-process populations and say
nothing about a physical network.

## Fresh clone closure

The release candidate was cloned into a clean directory and built, tested, installed, and consumed
from the clone alone - see the release report for the exact transcript. No file outside the
repository is required to build or test it.

## Hardening findings fixed during this release

Two independent audits were run against the first green build: a locking/cancellation/shutdown audit
and an adversarial review of the engine and its boundary. Every reproducible finding was fixed and
covered by a regression test:

* untrusted evidence (entries supplied without a trusted bundle generation) could be ranked as a
  known utilization;
* the hard-exclusion evaluation order deviated from the documented order for latency and affinity;
* the chosen reservation reference depended on the order the path listed its reservations;
* outcomes that produced no placement reported the incumbent relation as "no incumbent";
* an ineligible incumbent whose path happened to be the best candidate was reported as kept;
* `VerifyIntent` accepted an evidence binding the engine would never produce;
* a failed durable attempt-terminal append was discarded instead of being counted;
* the service-headroom requirement could be silently zeroed on overflow;
* the missing-required-label test was quadratic in policy size;
* the scenario parser was quadratic in the token count of one line and split lists before bounding
  them;
* three public decoders accepted trailing bytes, and status codes were decoded without validation;
* configured decoder bounds were truncated by narrowing casts;
* an absent (zero) failure domain could match the occupied-domain set;
* concurrent `Shutdown` calls could join the same workers twice;
* `Prepare` could resurrect a terminal attempt into `RUNNING`;
* `Await` could block forever if its slot was erased or left prepared at shutdown;
* the coordinator's epoch could be read without its lock when journaling attempt records;
* the in-memory authoritative placement map was written under one lock and read under another;
* a replay of an already-terminated attempt after a restart wrote a start record after its terminal
  record, which made the journal unreadable on the next open;
* the server joined connection threads that could be blocked in a receive forever, and never reaped
  finished connection threads;
* the durable record budget was cumulative, so a long-running store could wedge; compaction is now
  automatic, crash-safe across the sequence rebase, and retains a bounded attempt window;
* auto-compaction could recurse (stack overflow) and could snapshot before the record it was
  folding in was applied, losing one placement;
* cancellation could be reported for an attempt whose commit boundary had already been crossed;
* the negotiated frame bound was not applied to received frames;
* validator and engine duplicated the service-class requirement table, and enum-typed request fields
  were not range-checked.

## Remaining limitations

* Single-host validation only; the POSIX paths are compiled but unexercised here.
* No ThreadSanitizer; race freedom rests on the documented lock order and the concurrency tests.
* Synthetic benchmarks are single-threaded placements; coordinator throughput under real socket load
  is exercised for correctness, not measured for throughput.
* The store retains the latest placement of every flow plus a bounded attempt window; a fabric with
  an unbounded number of distinct flows grows the snapshot linearly with that number.

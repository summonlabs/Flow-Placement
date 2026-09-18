// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Synthetic placement benchmark.
//
// The populations measured here are generated in process. They are SYNTHETIC:
// they are not physical-network measurements, and no result produced by this
// harness may be presented as one. The harness measures completed placements
// (decisions fully computed and verified), never enqueue latency.

#ifndef FLOWPLACE_BENCH_HPP
#define FLOWPLACE_BENCH_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "flowplace/engine.hpp"
#include "flowplace/model.hpp"

namespace flowplace {

struct BenchConfig {
  std::uint64_t fanout = 64;        // candidate paths per flow
  std::uint64_t resources = 1024;   // distinct resources/locality ids
  std::uint64_t domains = 16;       // distinct failure domains
  std::uint64_t churn_percent = 0;  // percentage of flows with an incumbent
  std::uint64_t objectives = 3;     // policy objective complexity, 1..8
  std::uint64_t iterations = 200;   // completed placements to measure
  std::uint64_t warmup = 10;
  std::uint64_t seed = 1;
  Limits limits{};
};

struct BenchSample {
  std::uint64_t fanout = 0;
  std::uint64_t iterations = 0;
  std::uint64_t placed = 0;
  std::uint64_t placed_degraded = 0;
  std::uint64_t no_legal_path = 0;
  std::uint64_t insufficient_capacity = 0;
  std::uint64_t policy_rejected = 0;
  std::uint64_t deferred = 0;
  std::uint64_t stale_input = 0;
  std::uint64_t conflicting_input = 0;
  std::uint64_t total_nanos = 0;
  std::uint64_t p50_nanos = 0;
  std::uint64_t p99_nanos = 0;
  std::uint64_t max_nanos = 0;
  std::uint64_t decision_digest_accumulator = 0;
  std::uint64_t candidates_examined = 0;
  double placements_per_second = 0.0;
};

struct BenchResult {
  BenchConfig config;
  std::vector<BenchSample> samples;  // one per fan-out point
  std::string label = "SYNTHETIC";
};

// Runs the benchmark over a geometric sweep of fan-out values derived from
// |config.fanout| and |config.iterations|.
[[nodiscard]] BenchResult RunSyntheticPlacementBenchmark(const BenchConfig& config);

[[nodiscard]] std::string RenderBenchResult(const BenchResult& result);

}  // namespace flowplace

#endif  // FLOWPLACE_BENCH_HPP

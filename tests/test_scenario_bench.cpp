// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fixtures.hpp"
#include "harness.hpp"

#include <string>
#include <vector>

#include "flowplace/bench.hpp"
#include "flowplace/scenario.hpp"

using namespace flowplace;
using namespace fptest;

namespace {

Digest PlaceDigest(const PlacementRequest& request) {
  const PlacementEngine engine;
  return engine.Place(request).digest;
}

}  // namespace

FP_TEST(scenario_bench, benchmark_labels_itself_synthetic_and_measures_completed_work) {
  BenchConfig config;
  config.fanout = 32;
  config.iterations = 25;
  config.warmup = 2;
  config.seed = 12345;
  const BenchResult result = RunSyntheticPlacementBenchmark(config);
  FP_CHECK_EQ(result.label, std::string("SYNTHETIC"));
  FP_REQUIRE(!result.samples.empty());
  const BenchSample& last = result.samples.back();
  FP_CHECK_EQ(last.fanout, std::uint64_t{32});
  FP_CHECK_EQ(last.iterations, std::uint64_t{25});
  FP_CHECK_EQ(last.placed + last.placed_degraded + last.no_legal_path + last.insufficient_capacity +
                  last.policy_rejected + last.deferred + last.stale_input + last.conflicting_input,
              std::uint64_t{25});
  FP_CHECK_EQ(last.candidates_examined, std::uint64_t{25} * 32);
  FP_CHECK(last.total_nanos > 0);
  FP_CHECK(last.placements_per_second > 0.0);
  FP_CHECK(last.p50_nanos <= last.p99_nanos);
  FP_CHECK(last.p99_nanos <= last.max_nanos);
  const std::string rendered = RenderBenchResult(result);
  FP_CHECK(rendered.find("SYNTHETIC") != std::string::npos);
  FP_CHECK(rendered.find("not a network measurement") != std::string::npos);
}

FP_TEST(scenario_bench, benchmark_is_reproducible_for_a_seed) {
  BenchConfig config;
  config.fanout = 16;
  config.iterations = 12;
  config.warmup = 0;
  config.seed = 777;
  config.churn_percent = 40;
  const BenchResult first = RunSyntheticPlacementBenchmark(config);
  const BenchResult second = RunSyntheticPlacementBenchmark(config);
  FP_REQUIRE(first.samples.size() == second.samples.size());
  for (std::size_t i = 0; i < first.samples.size(); ++i) {
    FP_CHECK_EQ(first.samples[i].decision_digest_accumulator,
                second.samples[i].decision_digest_accumulator);
    FP_CHECK_EQ(first.samples[i].placed, second.samples[i].placed);
    FP_CHECK_EQ(first.samples[i].placed_degraded, second.samples[i].placed_degraded);
    FP_CHECK_EQ(first.samples[i].candidates_examined, second.samples[i].candidates_examined);
  }
}

FP_TEST(scenario_bench, benchmark_sweeps_fan_out_and_churn) {
  BenchConfig config;
  config.fanout = 128;
  config.iterations = 8;
  config.warmup = 0;
  config.churn_percent = 50;
  config.domains = 4;
  config.resources = 64;
  config.objectives = 4;
  const BenchResult result = RunSyntheticPlacementBenchmark(config);
  FP_CHECK(result.samples.size() >= 2);
  FP_CHECK_EQ(result.samples.front().fanout < result.samples.back().fanout, true);
  FP_CHECK_EQ(result.samples.back().fanout, std::uint64_t{128});
  for (const BenchSample& sample : result.samples) {
    FP_CHECK_EQ(sample.iterations, std::uint64_t{8});
    FP_CHECK(sample.placed + sample.placed_degraded <= sample.iterations);
  }
}

FP_TEST(scenario_bench, scenario_writer_and_parser_agree_on_a_minimal_document) {
  const PlacementRequest request = Baseline(2);
  const std::string text = WriteScenario(request);
  const Result<PlacementRequest> parsed = ParseScenario(text, Limits{});
  FP_REQUIRE(parsed.ok());
  FP_CHECK_EQ(PlaceDigest(parsed.value()), PlaceDigest(request));
  // A minimal hand-written document with the same content produces the same
  // decision.
  const std::string handwritten =
      "version 1\n"
      "flow id 100 gen 3\n"
      "candidateset id 200 gen 2\n"
      "path id 1 auth 7 tier standard locality 10 scope rack domain 20 cost 100 latency 1000 hops 1\n"
      "path id 2 auth 7 tier standard locality 11 scope rack domain 21 cost 110 latency 1100 hops 2\n"
      "capacity id 300 gen 5\n"
      "entry path 1 total 10000 residual 5000\n"
      "entry path 2 total 10000 residual 5000\n"
      "qos id 400 gen 4 class best_effort priority normal required 100 maxlatency 0\n"
      "evidence id 600 gen 9\n"
      "util path 1 ppb 100000000\n"
      "util path 2 ppb 101000000\n"
      "policy id 500 gen 6 objectives cost:min churn-threshold-objective 0\n"
      "expected pathauth 7 cset 2 cap 5 policy 6 qos 4 epoch 11 evidence 9\n";
  const Result<PlacementRequest> handwritten_parsed = ParseScenario(handwritten, Limits{});
  FP_REQUIRE(handwritten_parsed.ok());
  FP_CHECK_EQ(PlaceDigest(handwritten_parsed.value()), PlaceDigest(request));
}

FP_TEST(scenario_bench, scenario_parser_rejects_a_document_without_authority) {
  const std::string text =
      "version 1\n"
      "flow id 1 gen 1\n"
      "candidateset id 1 gen 1\n"
      "capacity id 1 gen 1\n"
      "qos id 1 gen 1 class best_effort\n"
      "policy id 1 gen 1 objectives cost:min\n";
  const Result<PlacementRequest> parsed = ParseScenario(text, Limits{});
  FP_CHECK(!parsed.ok());
  FP_CHECK_EQ(parsed.status().code(), StatusCode::kMissingField);
}

FP_TEST(scenario_bench, scenario_comments_and_blank_lines_are_ignored) {
  std::string text = "# a comment\n\n";
  text += WriteScenario(Baseline(1));
  text += "\n# trailing comment\n";
  const Result<PlacementRequest> parsed = ParseScenario(text, Limits{});
  FP_REQUIRE(parsed.ok());
  FP_CHECK_EQ(PlaceDigest(parsed.value()), PlaceDigest(Baseline(1)));
}

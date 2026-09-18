// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Strict, bounded, line-oriented scenario grammar used by the command line to
// build a PlacementRequest. The grammar is deliberately narrow: unknown
// directives, duplicate keys, out-of-range values, oversized inputs, and
// trailing garbage are all rejected with a precise status code.

#ifndef FLOWPLACE_SCENARIO_HPP
#define FLOWPLACE_SCENARIO_HPP

#include <string>
#include <string_view>

#include "flowplace/model.hpp"
#include "flowplace/status.hpp"

namespace flowplace {

inline constexpr std::uint64_t kMaxScenarioBytes = 64u << 20;
// A single directive line is bounded independently of the document size, so a
// hostile line cannot force quadratic parsing work.
inline constexpr std::uint64_t kMaxScenarioLineBytes = 64u << 10;

// Parse a scenario document. The returned Status is never kOk without a
// complete, validated request.
[[nodiscard]] Result<PlacementRequest> ParseScenario(std::string_view text,
                                                     const Limits& limits = Limits{});

// Canonical rendering. ParseScenario(WriteScenario(r)) preserves every field
// that participates in RequestDigest.
[[nodiscard]] std::string WriteScenario(const PlacementRequest& request);

}  // namespace flowplace

#endif  // FLOWPLACE_SCENARIO_HPP

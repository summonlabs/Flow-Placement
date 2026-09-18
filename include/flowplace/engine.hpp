// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Request validation and the deterministic placement engine.
//
// The engine is pure: it holds no mutable state, performs no I/O, and never
// throws across this boundary. Identical requests produce identical decisions,
// including identical digests. It can therefore be called concurrently on a
// single instance, which the test suite exercises directly.

#ifndef FLOWPLACE_ENGINE_HPP
#define FLOWPLACE_ENGINE_HPP

#include <string>

#include "flowplace/explain.hpp"
#include "flowplace/model.hpp"
#include "flowplace/status.hpp"

namespace flowplace {

// Structural validation of a request. Returns Ok when the request is
// well-formed, bounded, and internally consistent. Contradictions that are
// detectable without ranking (duplicate path ids, a required locality that is
// also forbidden, an unsatisfiable diversity requirement, inconsistent
// authority generations for one path, unrepresentable arithmetic) are reported
// here with their precise code.
[[nodiscard]] Status ValidateRequest(const PlacementRequest& request, const Limits& limits);

class PlacementEngine {
 public:
  PlacementEngine() = default;
  explicit PlacementEngine(Limits limits) : limits_(limits) {}

  [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

  // Deterministic placement. The returned decision is fully explained and
  // always states an outcome, even for rejected input.
  [[nodiscard]] PlacementDecision Place(const PlacementRequest& request) const;

  // Independent re-derivation of the invariants that a committed placement
  // must satisfy: the chosen path is in the exact authorized candidate set, its
  // authority generation is the expected one, and all hard capacity, service,
  // locality, failure-domain, and policy constraints hold. Returns false and
  // fills |reason| when any invariant fails. Used by the coordinator before
  // commit and by the test suite on hand-built placements.
  [[nodiscard]] bool VerifyIntent(const PlacementRequest& request, const PlacementIntent& intent,
                                  std::string* reason) const;

 private:
  Limits limits_{};
};

}  // namespace flowplace

#endif  // FLOWPLACE_ENGINE_HPP

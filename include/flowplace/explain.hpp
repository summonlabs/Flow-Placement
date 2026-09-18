// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Decision outcomes and bounded explanations.
//
// A decision always states: the outcome, the precise reason code, the exact
// authority vector that justified it, the bounded ranking that produced it, and
// why every other candidate was excluded. Explanations are bounded: they carry
// exact counters for every exclusion reason plus a bounded sample of path ids,
// so a placement over a million candidates produces a bounded explanation.

#ifndef FLOWPLACE_EXPLAIN_HPP
#define FLOWPLACE_EXPLAIN_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "flowplace/ids.hpp"
#include "flowplace/model.hpp"
#include "flowplace/status.hpp"

namespace flowplace {

enum class Outcome : std::uint8_t {
  kPlaced = 0,
  kPlacedDegraded = 1,
  kDeferred = 2,
  kNoLegalPath = 3,
  kInsufficientCapacity = 4,
  kPolicyRejected = 5,
  kStaleInput = 6,
  kConflictingInput = 7,
};

std::string_view OutcomeName(Outcome v) noexcept;

// Why a candidate was removed from consideration. Exclusion reason names are
// part of the explanation contract.
enum class ExclusionReason : std::uint8_t {
  kPathAuthorityStale = 0,
  kTierNotAllowed = 1,
  kForbiddenLabel = 2,
  kMissingRequiredLabel = 3,
  kForbiddenLocality = 4,
  kLocalityMismatch = 5,
  kForbiddenFailureDomain = 6,
  kRequiredFailureDomainMismatch = 7,
  kFailureDomainOccupied = 8,
  kCapacityUnknown = 9,
  kCapacityInsufficient = 10,
  kServiceHeadroomUnmet = 11,
  kServiceReservationUnmet = 12,
  kReservationAffinityUnsatisfied = 13,
  kLatencyBudgetExceeded = 14,
  kEvidenceMissing = 15,
  kEvidenceStale = 16,
  kDuplicatePathEntry = 17,
};

std::string_view ExclusionReasonName(ExclusionReason v) noexcept;
// True when the reason is a capacity/service feasibility failure.
[[nodiscard]] bool IsCapacityReason(ExclusionReason v) noexcept;
// True when the reason is an authority/evidence staleness failure.
[[nodiscard]] bool IsStalenessReason(ExclusionReason v) noexcept;
// True when the reason is a policy eligibility failure.
[[nodiscard]] bool IsPolicyReason(ExclusionReason v) noexcept;
// True when the reason is a service-class requirement failure.
[[nodiscard]] bool IsServiceReason(ExclusionReason v) noexcept;
// True when the reason is a quality-of-service requirement failure.
[[nodiscard]] bool IsQosReason(ExclusionReason v) noexcept;

// Why the placement is not ideal even though it is legal.
enum class DegradationFlag : std::uint8_t {
  kIncumbentSuperseded = 0,     // a legal incumbent was replaced (churn paid)
  kIncumbentStale = 1,          // the incumbent's authority was stale
  kServiceRelaxed = 2,          // a service-class requirement was relaxed
  kHeadroomBelowComfort = 3,    // admission comfort gate was crossed
  kLocalityPreferenceUnmet = 4, // preferred locality not available
  kReservationPreferenceUnmet = 5,
  kEvidenceUnknown = 6,         // ranking used an UNKNOWN key
  kChurnSuppressed = 7,         // incumbent retained to avoid churn
  kNoIncumbent = 8,             // flow had no incumbent
};

std::string_view DegradationFlagName(DegradationFlag v) noexcept;
// True when the flag means the placement is legal but not ideal, and therefore
// makes the outcome PLACED_DEGRADED rather than PLACED. Informational flags
// (an incumbent that was stale, churn that was suppressed, a flow that had no
// incumbent) do not degrade the outcome.
[[nodiscard]] bool IsDegradingFlag(DegradationFlag v) noexcept;

struct ExclusionSummary {
  ExclusionReason reason = ExclusionReason::kPathAuthorityStale;
  std::uint64_t count = 0;                 // exact, unbounded count
  std::vector<PathId> sample;              // bounded sample, ascending
};

// One ranking key of one candidate. UNKNOWN is a first-class state: a key that
// has no trustworthy evidence is not a zero.
struct ObjectiveValue {
  ObjectiveKind kind = ObjectiveKind::kCost;
  Direction direction = Direction::kMinimize;
  bool known = false;
  std::uint64_t value = 0;
};

struct RankedCandidate {
  PathId path;
  PathAuthorityGeneration authority;
  std::uint32_t rank = 0;         // 0 = best
  bool is_incumbent = false;
  std::vector<ObjectiveValue> keys;
};

// A constraint that actually bound the decision.
struct BindingConstraint {
  ExclusionReason kind = ExclusionReason::kCapacityInsufficient;
  PathId path;                    // the candidate the constraint applied to
  std::uint64_t required = 0;
  std::uint64_t available = 0;
  std::string label;
};

struct Explanation {
  bool truncated = false;                        // true if any part hit a bound
  std::uint64_t candidate_count = 0;
  std::uint64_t legal_candidate_count = 0;
  std::uint64_t excluded_candidate_count = 0;
  std::uint64_t unknown_evidence_count = 0;
  std::uint64_t distinct_failure_domains = 0;
  std::vector<RankedCandidate> ranked;           // bounded prefix, best first
  std::vector<ExclusionSummary> exclusions;      // bounded, canonical order
  std::vector<DegradationFlag> degradations;     // canonical order
  std::vector<BindingConstraint> binding;        // bounded
  std::vector<std::string> notes;                // bounded, canonical
};

// The exact path choice, bound to the generations that justified it. A
// PlacementIntent is deterministic: identical inputs produce an identical
// intent and an identical digest. Identity allocation (PlacementId and
// PlacementGeneration) happens at commit time and is deliberately not part of
// the intent.
struct PlacementIntent {
  FlowId flow;
  FlowGeneration flow_generation;
  PathId path;
  PathAuthorityGeneration path_authority;
  CandidateSetId candidate_set;
  CandidateSetGeneration candidate_set_generation;
  CapacitySnapshotId capacity;
  CapacitySnapshotGeneration capacity_generation;
  PolicyId policy;
  PolicyGeneration policy_generation;
  QosProfileId qos;
  QosGeneration qos_generation;
  EvidenceId evidence;
  EvidenceGeneration evidence_generation;  // 0 = no evidence was used
  FabricEpoch fabric_epoch;
  ReservationRef reservation;              // optional (id 0 = none)
  AttemptId attempt;                       // 0 = unattributed
  std::uint64_t revalidate_after_nanos = 0;
  Digest digest;                           // canonical digest of the intent

  friend bool operator==(const PlacementIntent& a, const PlacementIntent& b) noexcept;
};

enum class IncumbentDelta : std::uint8_t {
  kNoIncumbent = 0,   // the flow had no incumbent
  kKept = 1,          // placed on the incumbent path
  kMoved = 2,         // placed on a different path than a legal incumbent
  kReplacedStale = 3, // placed on a different path because the incumbent was stale
};

std::string_view IncumbentDeltaName(IncumbentDelta v) noexcept;

struct PlacementDecision {
  Outcome outcome = Outcome::kNoLegalPath;
  StatusCode code = StatusCode::kOk;
  std::optional<PlacementIntent> intent;
  AuthorityVector authority;
  IncumbentDelta delta = IncumbentDelta::kNoIncumbent;
  Explanation explanation;
  Digest digest;          // canonical digest of the whole decision
  Status validation;      // Ok, or the precise input rejection

  [[nodiscard]] bool placed() const noexcept {
    return outcome == Outcome::kPlaced || outcome == Outcome::kPlacedDegraded;
  }
};

// Canonical digests. Two decisions with equal digests are equal decisions.
[[nodiscard]] Digest IntentDigest(const PlacementIntent& intent);
[[nodiscard]] Digest DecisionDigest(const PlacementDecision& decision);
// Canonical, stable, human-readable rendering used by the CLI and by tests.
[[nodiscard]] std::string RenderDecision(const PlacementDecision& decision);

}  // namespace flowplace

#endif  // FLOWPLACE_EXPLAIN_HPP

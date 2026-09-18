// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Domain model for generation-bound, per-flow path placement.
//
// BOUNDARY. This model describes a flow, an already-authorized candidate set,
// capacity/service evidence, placement policy, and the generations that bind
// them. It deliberately has no representation for route computation, route
// state, path weights, rate enforcement, bandwidth reservation, scheduling, or
// forwarding state: those are adjacent systems this library must not absorb.
// A ReservationRef is a *reference* to a reservation someone else established;
// nothing here creates, modifies, or enforces one.

#ifndef FLOWPLACE_MODEL_HPP
#define FLOWPLACE_MODEL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "flowplace/hash.hpp"
#include "flowplace/ids.hpp"

namespace flowplace {

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

enum class ServiceClass : std::uint8_t {
  kBestEffort = 0,
  kControlled = 1,
  kAssured = 2,
  kReserved = 3,
};

enum class PriorityClass : std::uint8_t {
  kLow = 0,
  kNormal = 1,
  kHigh = 2,
  kCritical = 3,
};

// Service tier of a path as classified by the path authority (opaque ordering:
// lower ordinal is the more capable tier).
enum class PathTier : std::uint8_t {
  kPremium = 0,
  kStandard = 1,
  kEconomy = 2,
};

enum class LocalityScope : std::uint8_t {
  kNode = 0,
  kRack = 1,
  kZone = 2,
  kRegion = 3,
  kGlobal = 4,
};

// Ranking objectives. A policy states the objective order explicitly; the
// engine has no implicit default ordering.
enum class ObjectiveKind : std::uint8_t {
  kCost = 0,
  kLatency = 1,
  kResidualCapacity = 2,
  kHopCount = 3,
  kCongestionUtilization = 4,
  kTier = 5,
  kLocalityAffinity = 6,
  kReservationAffinity = 7,
  kIncumbentStability = 8,
};

enum class Direction : std::uint8_t {
  kMinimize = 0,
  kMaximize = 1,
};

// What to do when the evidence an objective requires is absent or stale.
enum class EvidencePolicyMode : std::uint8_t {
  kRankWorst = 0,   // candidate stays legal, its key is UNKNOWN (sorts last)
  kHardExclude = 1, // candidate is excluded
};

enum class GateAction : std::uint8_t {
  kPlace = 0,       // place and flag a degradation
  kDefer = 1,       // defer the decision
};

enum class ChurnAction : std::uint8_t {
  kAllowMove = 0,
  kDefer = 1,
};

// Service-class headroom multiplier: how much residual capacity evidence must
// remain available for the flow's own requirement. This is the single source of
// truth for the requirement table; validation and ranking both use it.
[[nodiscard]] std::uint64_t ServiceHeadroomMultiplier(ServiceClass v) noexcept;
// True when the service class requires the chosen path to carry a reservation
// reference.
[[nodiscard]] bool ServiceRequiresReservation(ServiceClass v) noexcept;

std::string_view ServiceClassName(ServiceClass v) noexcept;
std::string_view PriorityClassName(PriorityClass v) noexcept;
std::string_view PathTierName(PathTier v) noexcept;
std::string_view LocalityScopeName(LocalityScope v) noexcept;
std::string_view ObjectiveKindName(ObjectiveKind v) noexcept;
std::string_view DirectionName(Direction v) noexcept;
std::string_view EvidencePolicyModeName(EvidencePolicyMode v) noexcept;
std::string_view GateActionName(GateAction v) noexcept;
std::string_view ChurnActionName(ChurnAction v) noexcept;

std::optional<ServiceClass> ParseServiceClass(std::string_view v) noexcept;
std::optional<PriorityClass> ParsePriorityClass(std::string_view v) noexcept;
std::optional<PathTier> ParsePathTier(std::string_view v) noexcept;
std::optional<LocalityScope> ParseLocalityScope(std::string_view v) noexcept;
std::optional<ObjectiveKind> ParseObjectiveKind(std::string_view v) noexcept;
std::optional<Direction> ParseDirection(std::string_view v) noexcept;
std::optional<EvidencePolicyMode> ParseEvidencePolicyMode(std::string_view v) noexcept;
std::optional<GateAction> ParseGateAction(std::string_view v) noexcept;
std::optional<ChurnAction> ParseChurnAction(std::string_view v) noexcept;

// ---------------------------------------------------------------------------
// Bounds. Every externally influenced dimension is bounded; a request that
// exceeds a bound is rejected (kOversizeRequest) instead of being truncated.
// ---------------------------------------------------------------------------
struct Limits {
  std::uint64_t max_candidate_paths = 1u << 20;        // 1,048,576
  std::uint64_t max_capacity_entries = 1u << 21;       // 2,097,152
  std::uint64_t max_evidence_entries = 1u << 21;
  std::uint64_t max_occupied_failure_domains = 1u << 20;
  std::uint64_t max_objectives = 8;
  std::uint64_t max_path_labels = 32;
  std::uint64_t max_path_reservations = 16;
  std::uint64_t max_policy_id_lists = 4096;            // per id list in a policy
  std::uint64_t max_affinity_refs = 256;
  std::uint64_t max_string_length = 256;
  std::uint64_t max_ranked_explained = 32;             // bounded explanation prefix
  std::uint64_t max_exclusion_samples = 4;             // bounded samples per reason
  std::uint64_t max_exclusion_reasons = 64;
};

// ---------------------------------------------------------------------------
// Facts supplied by the caller
// ---------------------------------------------------------------------------

// A reference to a reservation established outside this library.
struct ReservationRef {
  ReservationId id;
  ReservationGeneration generation;

  [[nodiscard]] bool valid() const noexcept { return id.valid() && generation.valid(); }
  friend bool operator==(const ReservationRef& a, const ReservationRef& b) noexcept {
    return a.id == b.id && a.generation == b.generation;
  }
  friend bool operator<(const ReservationRef& a, const ReservationRef& b) noexcept {
    return a.id != b.id ? a.id < b.id : a.generation < b.generation;
  }
  [[nodiscard]] std::string ToString() const;
};

struct PathAttributes {
  PathTier tier = PathTier::kStandard;
  LocalityId locality;
  LocalityScope locality_scope = LocalityScope::kNode;
  FailureDomainId failure_domain;
  std::uint64_t cost_micro = 0;      // externally supplied, fixed point
  std::uint64_t latency_nanos = 0;   // externally supplied
  std::uint16_t hop_count = 0;
  std::vector<PolicyLabelId> labels;              // canonical (sorted, unique) after validation
  std::vector<ReservationRef> reservations;       // canonical (sorted, unique) after validation
};

struct CandidatePath {
  PathId id;
  PathAuthorityGeneration authority;
  PathAttributes attributes;
};

struct CandidateSet {
  CandidateSetId id;
  CandidateSetGeneration generation;
  std::vector<CandidatePath> paths;
};

struct PathCapacity {
  PathId path;
  std::uint64_t capacity_bytes = 0;
  std::uint64_t residual_bytes = 0;
};

struct CapacitySnapshot {
  CapacitySnapshotId id;
  CapacitySnapshotGeneration generation;
  std::vector<PathCapacity> entries;
};

struct CongestionEvidenceEntry {
  PathId path;
  std::uint32_t utilization_ppb = 0;  // parts per billion, 0..1e9
};

struct EvidenceBundle {
  EvidenceId id;
  EvidenceGeneration generation;
  std::vector<CongestionEvidenceEntry> entries;
};

struct QosProfile {
  QosProfileId id;
  QosGeneration generation;
  ServiceClass service_class = ServiceClass::kBestEffort;
  PriorityClass priority = PriorityClass::kNormal;
  // Residual capacity this flow itself requires to be present and to remain
  // present after it is carried. This is a feasibility requirement checked
  // against supplied evidence; it is not a reservation and not a rate.
  std::uint64_t required_residual_bytes = 0;
  std::uint64_t max_latency_nanos = 0;  // 0 = unconstrained
};

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

struct Objective {
  ObjectiveKind kind = ObjectiveKind::kCost;
  Direction direction = Direction::kMinimize;
};

struct LocalityConstraint {
  std::optional<LocalityId> required_locality;   // hard
  std::optional<LocalityId> preferred_locality;  // soft (ranking affinity)
  std::vector<LocalityId> forbidden_localities;  // hard
};

struct FailureDomainConstraint {
  std::optional<FailureDomainId> required_domain;  // hard
  std::vector<FailureDomainId> forbidden_domains;  // hard
  bool avoid_occupied_domains = false;             // hard, uses request occupied set
  // The flow's diversity requirement: its legal candidate set must span at
  // least this many distinct failure domains. 0 = no diversity requirement.
  std::uint32_t min_distinct_domains = 0;
};

struct ReservationAffinity {
  bool required = false;                    // hard when true
  std::vector<ReservationRef> refs;         // exact (id, generation) matches
};

struct AdmissionGate {
  std::uint64_t min_residual_headroom_bytes = 0;
  GateAction on_below = GateAction::kPlace;
};

struct ServicePolicy {
  // Relaxation is explicit and bounded: step 1 drops the service-class headroom
  // multiplier, step 2 additionally drops the service-class reservation
  // requirement. The flow's own required_residual_bytes is never relaxed.
  bool allow_degraded = false;
  std::uint8_t max_relaxation_steps = 0;
};

struct ChurnPolicy {
  ChurnAction on_move = ChurnAction::kAllowMove;
  // If the improvement of the best candidate over a legal incumbent, measured on
  // objectives[threshold_objective_index], is <= this threshold, the incumbent
  // is retained and churn is avoided.
  bool prefer_incumbent = false;
  std::uint64_t move_improvement_threshold = 0;
  std::uint32_t threshold_objective_index = 0;
};

struct EvidencePolicy {
  EvidencePolicyMode on_missing = EvidencePolicyMode::kHardExclude;
  EvidencePolicyMode on_stale = EvidencePolicyMode::kHardExclude;
};

struct PlacementPolicy {
  PolicyId id;
  PolicyGeneration generation;
  std::vector<Objective> objectives;             // must be non-empty
  std::vector<PathTier> allowed_tiers;           // empty = all tiers allowed
  std::vector<PolicyLabelId> required_labels;    // hard: path must carry all
  std::vector<PolicyLabelId> forbidden_labels;   // hard
  LocalityConstraint locality;
  FailureDomainConstraint failure_domains;
  ReservationAffinity reservation_affinity;
  AdmissionGate admission;
  ServicePolicy service;
  ChurnPolicy churn;
  EvidencePolicy evidence;
  // Dynamic evidence must be revalidated after this interval. 0 = no
  // revalidation requirement is expressed by policy.
  std::uint64_t revalidate_after_nanos = 0;
};

// ---------------------------------------------------------------------------
// Incumbent and authority
// ---------------------------------------------------------------------------

struct IncumbentPlacement {
  PlacementId id;
  PlacementGeneration generation;
  PathId path;
  PathAuthorityGeneration path_authority;
  CandidateSetGeneration candidate_set_generation;
  CapacitySnapshotGeneration capacity_generation;
  PolicyGeneration policy_generation;
  QosGeneration qos_generation;
  FabricEpoch fabric_epoch;
  ReservationRef reservation;  // optional
};

// What the caller asserts is current. A request that does not declare these
// cannot be checked and is rejected as malformed.
struct AuthorityExpectation {
  PathAuthorityGeneration path_authority;
  CandidateSetGeneration candidate_set;
  CapacitySnapshotGeneration capacity;
  PolicyGeneration policy;
  QosGeneration qos;
  FabricEpoch fabric_epoch;
  // 0 means "no evidence generation is trusted"; any supplied evidence is then
  // treated as untrusted.
  EvidenceGeneration evidence;
};

struct AuthorityVector {
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
  EvidenceGeneration evidence_generation;
  FabricEpoch fabric_epoch;

  friend bool operator==(const AuthorityVector& a, const AuthorityVector& b) noexcept;
  [[nodiscard]] std::string ToString() const;
};

struct PlacementRequest {
  FlowId flow;
  FlowGeneration flow_generation;
  CandidateSet candidates;
  CapacitySnapshot capacity;
  QosProfile qos;
  PlacementPolicy policy;
  EvidenceBundle evidence;
  std::optional<IncumbentPlacement> incumbent;
  // Failure domains occupied by other placements, as observed by the caller.
  std::vector<FailureDomainId> occupied_failure_domains;
  AuthorityExpectation expected;
  Provenance provenance;
  // Caller-supplied attempt identity used for duplicate detection. 0 = none.
  AttemptId attempt;
};

// ---------------------------------------------------------------------------
// Canonical digests
// ---------------------------------------------------------------------------
[[nodiscard]] Digest CandidateSetDigest(const CandidateSet& set);
[[nodiscard]] Digest CapacitySnapshotDigest(const CapacitySnapshot& snapshot);
[[nodiscard]] Digest EvidenceDigest(const EvidenceBundle& evidence);
[[nodiscard]] Digest QosProfileDigest(const QosProfile& qos);
[[nodiscard]] Digest PolicyDigest(const PlacementPolicy& policy);
[[nodiscard]] Digest RequestDigest(const PlacementRequest& request, const Limits& limits);
[[nodiscard]] Digest AuthorityVectorDigest(const AuthorityVector& authority);

}  // namespace flowplace

#endif  // FLOWPLACE_MODEL_HPP

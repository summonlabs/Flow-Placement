// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/model.hpp"

#include <algorithm>

#include "flowplace/hash.hpp"

namespace flowplace {
namespace {

template <class T, std::size_t N>
std::optional<T> ParseEnum(const std::pair<std::string_view, T> (&table)[N],
                           std::string_view v) noexcept {
  for (std::size_t i = 0; i < N; ++i) {
    if (table[i].first == v) return table[i].second;
  }
  return std::nullopt;
}

constexpr std::pair<std::string_view, ServiceClass> kServiceClassNames[] = {
    {"best_effort", ServiceClass::kBestEffort},
    {"controlled", ServiceClass::kControlled},
    {"assured", ServiceClass::kAssured},
    {"reserved", ServiceClass::kReserved},
};

constexpr std::pair<std::string_view, PriorityClass> kPriorityNames[] = {
    {"low", PriorityClass::kLow},
    {"normal", PriorityClass::kNormal},
    {"high", PriorityClass::kHigh},
    {"critical", PriorityClass::kCritical},
};

constexpr std::pair<std::string_view, PathTier> kTierNames[] = {
    {"premium", PathTier::kPremium},
    {"standard", PathTier::kStandard},
    {"economy", PathTier::kEconomy},
};

constexpr std::pair<std::string_view, LocalityScope> kScopeNames[] = {
    {"node", LocalityScope::kNode},   {"rack", LocalityScope::kRack},
    {"zone", LocalityScope::kZone},   {"region", LocalityScope::kRegion},
    {"global", LocalityScope::kGlobal},
};

constexpr std::pair<std::string_view, ObjectiveKind> kObjectiveNames[] = {
    {"cost", ObjectiveKind::kCost},
    {"latency", ObjectiveKind::kLatency},
    {"residual_capacity", ObjectiveKind::kResidualCapacity},
    {"hop_count", ObjectiveKind::kHopCount},
    {"congestion", ObjectiveKind::kCongestionUtilization},
    {"tier", ObjectiveKind::kTier},
    {"locality_affinity", ObjectiveKind::kLocalityAffinity},
    {"reservation_affinity", ObjectiveKind::kReservationAffinity},
    {"incumbent_stability", ObjectiveKind::kIncumbentStability},
};

constexpr std::pair<std::string_view, Direction> kDirectionNames[] = {
    {"min", Direction::kMinimize},
    {"max", Direction::kMaximize},
};

constexpr std::pair<std::string_view, EvidencePolicyMode> kEvidenceModeNames[] = {
    {"rank_worst", EvidencePolicyMode::kRankWorst},
    {"hard_exclude", EvidencePolicyMode::kHardExclude},
};

constexpr std::pair<std::string_view, GateAction> kGateNames[] = {
    {"place", GateAction::kPlace},
    {"defer", GateAction::kDefer},
};

constexpr std::pair<std::string_view, ChurnAction> kChurnNames[] = {
    {"allow_move", ChurnAction::kAllowMove},
    {"defer", ChurnAction::kDefer},
};

void HashReservation(CanonicalHasher& h, const ReservationRef& r) {
  h.AddTag("reservation");
  h.AddU64(r.id.value());
  h.AddU64(r.generation.value());
}

// Labels and reservation references are sets: their digest is computed over a
// sorted copy so that the order in which a caller listed them cannot change the
// identity of a request.
void HashPathAttributes(CanonicalHasher& h, const PathAttributes& a) {
  h.AddTag("path_attributes");
  h.AddU8(static_cast<std::uint8_t>(a.tier));
  h.AddU64(a.locality.value());
  h.AddU8(static_cast<std::uint8_t>(a.locality_scope));
  h.AddU64(a.failure_domain.value());
  h.AddU64(a.cost_micro);
  h.AddU64(a.latency_nanos);
  h.AddU16(a.hop_count);
  std::vector<PolicyLabelId> labels = a.labels;
  std::sort(labels.begin(), labels.end());
  labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
  h.AddU32(static_cast<std::uint32_t>(labels.size()));
  for (const PolicyLabelId label : labels) h.AddU64(label.value());
  std::vector<ReservationRef> reservations = a.reservations;
  std::sort(reservations.begin(), reservations.end());
  reservations.erase(std::unique(reservations.begin(), reservations.end()), reservations.end());
  h.AddU32(static_cast<std::uint32_t>(reservations.size()));
  for (const ReservationRef& r : reservations) HashReservation(h, r);
}

// Hashes an unordered collection in a canonical order, so that the order in
// which a caller supplied it cannot change the identity of the request.
template <class T, class KeyFn, class WriteFn>
void HashSorted(const std::vector<T>& input, KeyFn key, WriteFn write, CanonicalHasher& h) {
  std::vector<const T*> sorted;
  sorted.reserve(input.size());
  for (const T& item : input) sorted.push_back(&item);
  std::sort(sorted.begin(), sorted.end(),
            [&key](const T* a, const T* b) { return key(*a) < key(*b); });
  h.AddU32(static_cast<std::uint32_t>(sorted.size()));
  for (const T* item : sorted) write(*item, h);
}

void HashIncumbent(CanonicalHasher& h, const IncumbentPlacement& inc) {
  h.AddTag("incumbent");
  h.AddU64(inc.id.value());
  h.AddU64(inc.generation.value());
  h.AddU64(inc.path.value());
  h.AddU64(inc.path_authority.value());
  h.AddU64(inc.candidate_set_generation.value());
  h.AddU64(inc.capacity_generation.value());
  h.AddU64(inc.policy_generation.value());
  h.AddU64(inc.qos_generation.value());
  h.AddU64(inc.fabric_epoch.value());
  HashReservation(h, inc.reservation);
}

template <class IdT>
void HashIdSet(CanonicalHasher& h, std::string_view tag, const std::vector<IdT>& ids) {
  h.AddTag(tag);
  std::vector<IdT> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  h.AddU32(static_cast<std::uint32_t>(sorted.size()));
  for (const IdT id : sorted) h.AddU64(id.value());
}

void HashReservationSet(CanonicalHasher& h, std::string_view tag,
                        const std::vector<ReservationRef>& refs) {
  h.AddTag(tag);
  std::vector<ReservationRef> sorted = refs;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  h.AddU32(static_cast<std::uint32_t>(sorted.size()));
  for (const ReservationRef& ref : sorted) HashReservation(h, ref);
}

}  // namespace

std::uint64_t ServiceHeadroomMultiplier(ServiceClass v) noexcept {
  switch (v) {
    case ServiceClass::kBestEffort: return 1;
    case ServiceClass::kControlled: return 2;
    case ServiceClass::kAssured: return 3;
    case ServiceClass::kReserved: return 3;
  }
  return 1;
}

bool ServiceRequiresReservation(ServiceClass v) noexcept {
  return v == ServiceClass::kReserved;
}

std::string_view ServiceClassName(ServiceClass v) noexcept {
  switch (v) {
    case ServiceClass::kBestEffort: return "best_effort";
    case ServiceClass::kControlled: return "controlled";
    case ServiceClass::kAssured: return "assured";
    case ServiceClass::kReserved: return "reserved";
  }
  return "unknown";
}

std::string_view PriorityClassName(PriorityClass v) noexcept {
  switch (v) {
    case PriorityClass::kLow: return "low";
    case PriorityClass::kNormal: return "normal";
    case PriorityClass::kHigh: return "high";
    case PriorityClass::kCritical: return "critical";
  }
  return "unknown";
}

std::string_view PathTierName(PathTier v) noexcept {
  switch (v) {
    case PathTier::kPremium: return "premium";
    case PathTier::kStandard: return "standard";
    case PathTier::kEconomy: return "economy";
  }
  return "unknown";
}

std::string_view LocalityScopeName(LocalityScope v) noexcept {
  switch (v) {
    case LocalityScope::kNode: return "node";
    case LocalityScope::kRack: return "rack";
    case LocalityScope::kZone: return "zone";
    case LocalityScope::kRegion: return "region";
    case LocalityScope::kGlobal: return "global";
  }
  return "unknown";
}

std::string_view ObjectiveKindName(ObjectiveKind v) noexcept {
  switch (v) {
    case ObjectiveKind::kCost: return "cost";
    case ObjectiveKind::kLatency: return "latency";
    case ObjectiveKind::kResidualCapacity: return "residual_capacity";
    case ObjectiveKind::kHopCount: return "hop_count";
    case ObjectiveKind::kCongestionUtilization: return "congestion";
    case ObjectiveKind::kTier: return "tier";
    case ObjectiveKind::kLocalityAffinity: return "locality_affinity";
    case ObjectiveKind::kReservationAffinity: return "reservation_affinity";
    case ObjectiveKind::kIncumbentStability: return "incumbent_stability";
  }
  return "unknown";
}

std::string_view DirectionName(Direction v) noexcept {
  switch (v) {
    case Direction::kMinimize: return "min";
    case Direction::kMaximize: return "max";
  }
  return "unknown";
}

std::string_view EvidencePolicyModeName(EvidencePolicyMode v) noexcept {
  switch (v) {
    case EvidencePolicyMode::kRankWorst: return "rank_worst";
    case EvidencePolicyMode::kHardExclude: return "hard_exclude";
  }
  return "unknown";
}

std::string_view GateActionName(GateAction v) noexcept {
  switch (v) {
    case GateAction::kPlace: return "place";
    case GateAction::kDefer: return "defer";
  }
  return "unknown";
}

std::string_view ChurnActionName(ChurnAction v) noexcept {
  switch (v) {
    case ChurnAction::kAllowMove: return "allow_move";
    case ChurnAction::kDefer: return "defer";
  }
  return "unknown";
}

std::optional<ServiceClass> ParseServiceClass(std::string_view v) noexcept {
  return ParseEnum(kServiceClassNames, v);
}
std::optional<PriorityClass> ParsePriorityClass(std::string_view v) noexcept {
  return ParseEnum(kPriorityNames, v);
}
std::optional<PathTier> ParsePathTier(std::string_view v) noexcept {
  return ParseEnum(kTierNames, v);
}
std::optional<LocalityScope> ParseLocalityScope(std::string_view v) noexcept {
  return ParseEnum(kScopeNames, v);
}
std::optional<ObjectiveKind> ParseObjectiveKind(std::string_view v) noexcept {
  return ParseEnum(kObjectiveNames, v);
}
std::optional<Direction> ParseDirection(std::string_view v) noexcept {
  return ParseEnum(kDirectionNames, v);
}
std::optional<EvidencePolicyMode> ParseEvidencePolicyMode(std::string_view v) noexcept {
  return ParseEnum(kEvidenceModeNames, v);
}
std::optional<GateAction> ParseGateAction(std::string_view v) noexcept {
  return ParseEnum(kGateNames, v);
}
std::optional<ChurnAction> ParseChurnAction(std::string_view v) noexcept {
  return ParseEnum(kChurnNames, v);
}

std::string ReservationRef::ToString() const {
  if (!valid()) return "-";
  return id.ToString() + ":" + generation.ToString();
}

bool operator==(const AuthorityVector& a, const AuthorityVector& b) noexcept {
  return a.path_authority == b.path_authority && a.candidate_set == b.candidate_set &&
         a.candidate_set_generation == b.candidate_set_generation && a.capacity == b.capacity &&
         a.capacity_generation == b.capacity_generation && a.policy == b.policy &&
         a.policy_generation == b.policy_generation && a.qos == b.qos &&
         a.qos_generation == b.qos_generation && a.evidence == b.evidence &&
         a.evidence_generation == b.evidence_generation && a.fabric_epoch == b.fabric_epoch;
}

std::string AuthorityVector::ToString() const {
  std::string out;
  out.reserve(160);
  out += "pathauth=" + path_authority.ToString();
  out += " cset=" + candidate_set.ToString() + "/" + candidate_set_generation.ToString();
  out += " cap=" + capacity.ToString() + "/" + capacity_generation.ToString();
  out += " policy=" + policy.ToString() + "/" + policy_generation.ToString();
  out += " qos=" + qos.ToString() + "/" + qos_generation.ToString();
  out += " evidence=" + evidence.ToString() + "/" + evidence_generation.ToString();
  out += " epoch=" + std::to_string(fabric_epoch.value());
  return out;
}

Digest CandidateSetDigest(const CandidateSet& set) {
  CanonicalHasher h;
  h.AddTag("candidate_set");
  h.AddU64(set.id.value());
  h.AddU64(set.generation.value());
  HashSorted(set.paths, [](const CandidatePath& path) { return path.id; },
             [](const CandidatePath& path, CanonicalHasher& hasher) {
               hasher.AddU64(path.id.value());
               hasher.AddU64(path.authority.value());
               HashPathAttributes(hasher, path.attributes);
             },
             h);
  return Digest::OfCanonical(h);
}

Digest CapacitySnapshotDigest(const CapacitySnapshot& snapshot) {
  CanonicalHasher h;
  h.AddTag("capacity_snapshot");
  h.AddU64(snapshot.id.value());
  h.AddU64(snapshot.generation.value());
  HashSorted(snapshot.entries, [](const PathCapacity& entry) { return entry.path; },
             [](const PathCapacity& entry, CanonicalHasher& hasher) {
               hasher.AddU64(entry.path.value());
               hasher.AddU64(entry.capacity_bytes);
               hasher.AddU64(entry.residual_bytes);
             },
             h);
  return Digest::OfCanonical(h);
}

Digest EvidenceDigest(const EvidenceBundle& evidence) {
  CanonicalHasher h;
  h.AddTag("evidence");
  h.AddU64(evidence.id.value());
  h.AddU64(evidence.generation.value());
  HashSorted(evidence.entries, [](const CongestionEvidenceEntry& entry) { return entry.path; },
             [](const CongestionEvidenceEntry& entry, CanonicalHasher& hasher) {
               hasher.AddU64(entry.path.value());
               hasher.AddU32(entry.utilization_ppb);
             },
             h);
  return Digest::OfCanonical(h);
}

Digest QosProfileDigest(const QosProfile& qos) {
  CanonicalHasher h;
  h.AddTag("qos");
  h.AddU64(qos.id.value());
  h.AddU64(qos.generation.value());
  h.AddU8(static_cast<std::uint8_t>(qos.service_class));
  h.AddU8(static_cast<std::uint8_t>(qos.priority));
  h.AddU64(qos.required_residual_bytes);
  h.AddU64(qos.max_latency_nanos);
  return Digest::OfCanonical(h);
}

Digest PolicyDigest(const PlacementPolicy& policy) {
  CanonicalHasher h;
  h.AddTag("policy");
  h.AddU64(policy.id.value());
  h.AddU64(policy.generation.value());
  h.AddU32(static_cast<std::uint32_t>(policy.objectives.size()));
  for (const Objective& objective : policy.objectives) {
    h.AddU8(static_cast<std::uint8_t>(objective.kind));
    h.AddU8(static_cast<std::uint8_t>(objective.direction));
  }
  {
    std::vector<PathTier> tiers = policy.allowed_tiers;
    std::sort(tiers.begin(), tiers.end());
    tiers.erase(std::unique(tiers.begin(), tiers.end()), tiers.end());
    h.AddU32(static_cast<std::uint32_t>(tiers.size()));
    for (const PathTier tier : tiers) h.AddU8(static_cast<std::uint8_t>(tier));
  }
  HashIdSet(h, "required_labels", policy.required_labels);
  HashIdSet(h, "forbidden_labels", policy.forbidden_labels);
  h.AddTag("locality");
  h.AddBool(policy.locality.required_locality.has_value());
  if (policy.locality.required_locality) h.AddU64(policy.locality.required_locality->value());
  h.AddBool(policy.locality.preferred_locality.has_value());
  if (policy.locality.preferred_locality) h.AddU64(policy.locality.preferred_locality->value());
  HashIdSet(h, "forbidden_localities", policy.locality.forbidden_localities);
  h.AddTag("failure_domains");
  h.AddBool(policy.failure_domains.required_domain.has_value());
  if (policy.failure_domains.required_domain) h.AddU64(policy.failure_domains.required_domain->value());
  HashIdSet(h, "forbidden_domains", policy.failure_domains.forbidden_domains);
  h.AddBool(policy.failure_domains.avoid_occupied_domains);
  h.AddU32(policy.failure_domains.min_distinct_domains);
  h.AddTag("reservation_affinity");
  h.AddBool(policy.reservation_affinity.required);
  HashReservationSet(h, "affinity_refs", policy.reservation_affinity.refs);
  h.AddTag("admission");
  h.AddU64(policy.admission.min_residual_headroom_bytes);
  h.AddU8(static_cast<std::uint8_t>(policy.admission.on_below));
  h.AddTag("service");
  h.AddBool(policy.service.allow_degraded);
  h.AddU8(policy.service.max_relaxation_steps);
  h.AddTag("churn");
  h.AddU8(static_cast<std::uint8_t>(policy.churn.on_move));
  h.AddBool(policy.churn.prefer_incumbent);
  h.AddU64(policy.churn.move_improvement_threshold);
  h.AddU32(policy.churn.threshold_objective_index);
  h.AddTag("evidence_policy");
  h.AddU8(static_cast<std::uint8_t>(policy.evidence.on_missing));
  h.AddU8(static_cast<std::uint8_t>(policy.evidence.on_stale));
  h.AddU64(policy.revalidate_after_nanos);
  return Digest::OfCanonical(h);
}

Digest RequestDigest(const PlacementRequest& request, const Limits& limits) {
  (void)limits;  // limits do not participate in identity
  CanonicalHasher h;
  h.AddTag("request");
  h.AddU64(request.flow.value());
  h.AddU64(request.flow_generation.value());
  const Digest candidates = CandidateSetDigest(request.candidates);
  const Digest capacity = CapacitySnapshotDigest(request.capacity);
  const Digest evidence = EvidenceDigest(request.evidence);
  const Digest qos = QosProfileDigest(request.qos);
  const Digest policy = PolicyDigest(request.policy);
  h.AddU64(candidates.lo);
  h.AddU64(candidates.hi);
  h.AddU64(capacity.lo);
  h.AddU64(capacity.hi);
  h.AddU64(evidence.lo);
  h.AddU64(evidence.hi);
  h.AddU64(qos.lo);
  h.AddU64(qos.hi);
  h.AddU64(policy.lo);
  h.AddU64(policy.hi);
  h.AddBool(request.incumbent.has_value());
  if (request.incumbent) HashIncumbent(h, *request.incumbent);
  HashIdSet(h, "occupied_failure_domains", request.occupied_failure_domains);
  h.AddTag("expected");
  h.AddU64(request.expected.path_authority.value());
  h.AddU64(request.expected.candidate_set.value());
  h.AddU64(request.expected.capacity.value());
  h.AddU64(request.expected.policy.value());
  h.AddU64(request.expected.qos.value());
  h.AddU64(request.expected.evidence.value());
  h.AddU64(request.expected.fabric_epoch.value());
  // Descriptive provenance participates in identity; the informational
  // observation timestamp deliberately does not, so that a request observed at
  // two different times still digests identically.
  h.AddTag("provenance");
  h.AddString(request.provenance.producer);
  h.AddString(request.provenance.producer_version);
  h.AddU64(request.provenance.source_sequence);
  h.AddU64(request.attempt.value());
  return Digest::OfCanonical(h);
}

Digest AuthorityVectorDigest(const AuthorityVector& authority) {
  CanonicalHasher h;
  h.AddTag("authority_vector");
  h.AddU64(authority.path_authority.value());
  h.AddU64(authority.candidate_set.value());
  h.AddU64(authority.candidate_set_generation.value());
  h.AddU64(authority.capacity.value());
  h.AddU64(authority.capacity_generation.value());
  h.AddU64(authority.policy.value());
  h.AddU64(authority.policy_generation.value());
  h.AddU64(authority.qos.value());
  h.AddU64(authority.qos_generation.value());
  h.AddU64(authority.evidence.value());
  h.AddU64(authority.evidence_generation.value());
  h.AddU64(authority.fabric_epoch.value());
  return Digest::OfCanonical(h);
}

}  // namespace flowplace

// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The deterministic placement engine.
//
// Order of evaluation (fixed and documented; see DESIGN.md):
//   1. structural validation
//   2. request-level generation staleness
//   3. per-candidate hard exclusions, in a fixed reason order
//   4. classification of an empty legal set
//   5. failure-domain diversity over the legal set
//   6. deterministic lexicographic ranking
//   7. churn (incumbent stability) decision
//   8. admission comfort gate
//   9. self-verification of the chosen intent
//
// The engine never constructs a path, never falls back to an unlisted path,
// and never consults a default policy: every ranking key is derived from the
// supplied policy and from supplied evidence.

#include "flowplace/engine.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "flowplace/checked.hpp"

namespace flowplace {
namespace {

struct ServiceRequirement {
  std::uint64_t headroom_multiplier = 1;
  bool reservation_required = false;
};

ServiceRequirement RequirementFor(ServiceClass service_class) {
  ServiceRequirement requirement;
  requirement.headroom_multiplier = ServiceHeadroomMultiplier(service_class);
  requirement.reservation_required = ServiceRequiresReservation(service_class);
  return requirement;
}

// Sorted, de-duplicated copy of a path's label set.
std::vector<PolicyLabelId> SortedUnique(const std::vector<PolicyLabelId>& labels) {
  std::vector<PolicyLabelId> copy = labels;
  std::sort(copy.begin(), copy.end());
  copy.erase(std::unique(copy.begin(), copy.end()), copy.end());
  return copy;
}

template <class IdT>
bool ContainsSorted(const std::vector<IdT>& sorted, IdT value) {
  return std::binary_search(sorted.begin(), sorted.end(), value);
}

template <class T, class KeyFn>
std::vector<const T*> SortPointersById(const std::vector<T>& input, KeyFn key) {
  std::vector<const T*> out;
  out.reserve(input.size());
  for (const T& item : input) out.push_back(&item);
  std::sort(out.begin(), out.end(), [&key](const T* a, const T* b) { return key(*a) < key(*b); });
  return out;
}

template <class IdT, class T, class KeyFn>
const T* LookupById(const std::vector<const T*>& sorted, KeyFn key, IdT id) {
  const auto it = std::lower_bound(sorted.begin(), sorted.end(), id,
                                   [&key](const T* item, IdT value) { return key(*item) < value; });
  if (it == sorted.end() || !(key(**it) == id)) return nullptr;
  return *it;
}

struct PreparedRequest {
  const PlacementRequest& request;
  const Limits& limits;
  ServiceRequirement service;
  bool evidence_bundle_present = false;
  bool evidence_trusted = false;
  bool congestion_objective_used = false;
  bool incumbent_present = false;
  bool incumbent_eligible = false;
  bool headroom_requirement_overflow = false;
  std::uint64_t required_base_bytes = 0;
  std::uint64_t required_headroom_bytes = 0;
  std::vector<const CandidatePath*> candidates_by_id;
  std::vector<const PathCapacity*> capacity_by_path;
  std::vector<const CongestionEvidenceEntry*> evidence_by_path;
  std::vector<LocalityId> forbidden_localities;
  std::vector<FailureDomainId> forbidden_domains;
  std::vector<FailureDomainId> occupied_domains;
  std::vector<PolicyLabelId> required_labels;
  std::vector<PolicyLabelId> forbidden_labels;
  std::vector<ReservationRef> affinity_refs;
  std::vector<PathTier> allowed_tiers;
};

PreparedRequest Prepare(const PlacementRequest& request, const Limits& limits) {
  PreparedRequest prepared{request, limits};
  prepared.service = RequirementFor(request.qos.service_class);
  prepared.congestion_objective_used =
      std::any_of(request.policy.objectives.begin(), request.policy.objectives.end(),
                  [](const Objective& objective) {
                    return objective.kind == ObjectiveKind::kCongestionUtilization;
                  });
  prepared.required_base_bytes = request.qos.required_residual_bytes;
  const auto headroom = CheckedMul(prepared.required_base_bytes, prepared.service.headroom_multiplier);
  if (headroom.has_value()) {
    prepared.required_headroom_bytes = *headroom;
  } else {
    // UNKNOWN must not be coerced: an unrepresentable requirement is a hard
    // failure, never a disabled gate.
    prepared.headroom_requirement_overflow = true;
    prepared.required_headroom_bytes = prepared.required_base_bytes;
  }

  prepared.candidates_by_id =
      SortPointersById(request.candidates.paths, [](const CandidatePath& p) { return p.id; });
  prepared.capacity_by_path =
      SortPointersById(request.capacity.entries, [](const PathCapacity& e) { return e.path; });
  prepared.evidence_by_path = SortPointersById(
      request.evidence.entries, [](const CongestionEvidenceEntry& e) { return e.path; });

  prepared.evidence_bundle_present = request.evidence.generation.valid();
  prepared.evidence_trusted = prepared.evidence_bundle_present &&
                              request.expected.evidence.valid() &&
                              request.evidence.generation == request.expected.evidence;

  prepared.forbidden_localities = request.policy.locality.forbidden_localities;
  std::sort(prepared.forbidden_localities.begin(), prepared.forbidden_localities.end());
  prepared.forbidden_domains = request.policy.failure_domains.forbidden_domains;
  std::sort(prepared.forbidden_domains.begin(), prepared.forbidden_domains.end());
  prepared.occupied_domains = request.occupied_failure_domains;
  std::sort(prepared.occupied_domains.begin(), prepared.occupied_domains.end());
  prepared.occupied_domains.erase(
      std::unique(prepared.occupied_domains.begin(), prepared.occupied_domains.end()),
      prepared.occupied_domains.end());
  prepared.required_labels = request.policy.required_labels;
  std::sort(prepared.required_labels.begin(), prepared.required_labels.end());
  prepared.forbidden_labels = request.policy.forbidden_labels;
  std::sort(prepared.forbidden_labels.begin(), prepared.forbidden_labels.end());
  prepared.affinity_refs = request.policy.reservation_affinity.refs;
  std::sort(prepared.affinity_refs.begin(), prepared.affinity_refs.end());
  prepared.allowed_tiers = request.policy.allowed_tiers;
  std::sort(prepared.allowed_tiers.begin(), prepared.allowed_tiers.end());

  if (request.incumbent) {
    prepared.incumbent_present = true;
    const IncumbentPlacement& incumbent = *request.incumbent;
    prepared.incumbent_eligible =
        incumbent.path_authority == request.expected.path_authority &&
        incumbent.candidate_set_generation == request.expected.candidate_set &&
        incumbent.capacity_generation == request.capacity.generation &&
        incumbent.policy_generation == request.policy.generation &&
        incumbent.qos_generation == request.qos.generation &&
        incumbent.fabric_epoch == request.expected.fabric_epoch &&
        LookupById(prepared.candidates_by_id,
                   [](const CandidatePath& p) { return p.id; }, incumbent.path) != nullptr;
  }
  return prepared;
}

struct CandidateVerdict {
  bool legal = false;
  ExclusionReason reason = ExclusionReason::kPathAuthorityStale;
  bool multiplier_relaxed = false;
  bool reservation_relaxed = false;
  bool evidence_known = false;
  bool evidence_stale = false;
  std::uint64_t required = 0;
  std::uint64_t available = 0;
};

// Evaluates the fixed hard-exclusion order for one candidate. The order is the
// order of the ExclusionReason enumerators, so the reported reason is a
// function of the candidate and never of the order constraints were configured.
CandidateVerdict Evaluate(const PreparedRequest& prepared, const CandidatePath& path) {
  const PlacementRequest& request = prepared.request;
  const PlacementPolicy& policy = request.policy;
  CandidateVerdict verdict;

  if (path.authority != request.expected.path_authority) {
    verdict.reason = ExclusionReason::kPathAuthorityStale;
    return verdict;
  }
  if (!prepared.allowed_tiers.empty() &&
      !ContainsSorted(prepared.allowed_tiers, path.attributes.tier)) {
    verdict.reason = ExclusionReason::kTierNotAllowed;
    return verdict;
  }
  // Labels are normalised once per candidate: the policy label lists are sets,
  // so an intersection test is enough and the cost stays bounded by
  // |path.labels| * log(|path.labels|) rather than by the policy list size.
  const std::vector<PolicyLabelId> labels = SortedUnique(path.attributes.labels);
  for (const PolicyLabelId label : labels) {
    if (ContainsSorted(prepared.forbidden_labels, label)) {
      verdict.reason = ExclusionReason::kForbiddenLabel;
      return verdict;
    }
  }
  for (const PolicyLabelId required : prepared.required_labels) {
    if (!ContainsSorted(labels, required)) {
      verdict.reason = ExclusionReason::kMissingRequiredLabel;
      return verdict;
    }
  }
  if (path.attributes.locality.valid() &&
      ContainsSorted(prepared.forbidden_localities, path.attributes.locality)) {
    verdict.reason = ExclusionReason::kForbiddenLocality;
    return verdict;
  }
  if (policy.locality.required_locality &&
      path.attributes.locality != *policy.locality.required_locality) {
    verdict.reason = ExclusionReason::kLocalityMismatch;
    return verdict;
  }
  if (path.attributes.failure_domain.valid() &&
      ContainsSorted(prepared.forbidden_domains, path.attributes.failure_domain)) {
    verdict.reason = ExclusionReason::kForbiddenFailureDomain;
    return verdict;
  }
  if (policy.failure_domains.required_domain &&
      path.attributes.failure_domain != *policy.failure_domains.required_domain) {
    verdict.reason = ExclusionReason::kRequiredFailureDomainMismatch;
    return verdict;
  }
  if (policy.failure_domains.avoid_occupied_domains && path.attributes.failure_domain.valid() &&
      ContainsSorted(prepared.occupied_domains, path.attributes.failure_domain)) {
    verdict.reason = ExclusionReason::kFailureDomainOccupied;
    return verdict;
  }
  const PathCapacity* capacity = LookupById(
      prepared.capacity_by_path, [](const PathCapacity& e) { return e.path; }, path.id);
  if (capacity == nullptr) {
    // Missing capacity evidence is UNKNOWN, and UNKNOWN never becomes
    // positive authority.
    verdict.reason = ExclusionReason::kCapacityUnknown;
    return verdict;
  }
  if (capacity->residual_bytes < prepared.required_base_bytes) {
    verdict.reason = ExclusionReason::kCapacityInsufficient;
    verdict.required = prepared.required_base_bytes;
    verdict.available = capacity->residual_bytes;
    return verdict;
  }
  verdict.required = prepared.required_headroom_bytes;
  verdict.available = capacity->residual_bytes;
  if (capacity->residual_bytes < prepared.required_headroom_bytes) {
    if (policy.service.allow_degraded && policy.service.max_relaxation_steps >= 1) {
      verdict.multiplier_relaxed = true;
      verdict.required = prepared.required_base_bytes;
    } else {
      verdict.reason = ExclusionReason::kServiceHeadroomUnmet;
      return verdict;
    }
  }
  if (prepared.service.reservation_required && path.attributes.reservations.empty()) {
    if (policy.service.allow_degraded && policy.service.max_relaxation_steps >= 2) {
      verdict.reservation_relaxed = true;
    } else {
      verdict.reason = ExclusionReason::kServiceReservationUnmet;
      return verdict;
    }
  }
  if (policy.reservation_affinity.required) {
    bool matched = false;
    for (const ReservationRef& ref : path.attributes.reservations) {
      if (std::binary_search(prepared.affinity_refs.begin(), prepared.affinity_refs.end(), ref)) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      verdict.reason = ExclusionReason::kReservationAffinityUnsatisfied;
      return verdict;
    }
  }
  if (request.qos.max_latency_nanos != 0 &&
      path.attributes.latency_nanos > request.qos.max_latency_nanos) {
    verdict.reason = ExclusionReason::kLatencyBudgetExceeded;
    verdict.required = request.qos.max_latency_nanos;
    verdict.available = path.attributes.latency_nanos;
    return verdict;
  }
  if (prepared.congestion_objective_used) {
    const CongestionEvidenceEntry* entry = LookupById(
        prepared.evidence_by_path,
        [](const CongestionEvidenceEntry& e) { return e.path; }, path.id);
    if (!prepared.evidence_trusted) {
      // The request did not authorize this evidence: a bundle that was supplied
      // without a matching expected generation is stale, and an absent bundle is
      // missing. Neither may be ranked as a known utilization.
      if (prepared.evidence_bundle_present) {
        verdict.evidence_stale = true;
        if (policy.evidence.on_stale == EvidencePolicyMode::kHardExclude) {
          verdict.reason = ExclusionReason::kEvidenceStale;
          return verdict;
        }
      } else if (policy.evidence.on_missing == EvidencePolicyMode::kHardExclude) {
        verdict.reason = ExclusionReason::kEvidenceMissing;
        return verdict;
      }
    } else if (entry == nullptr) {
      if (policy.evidence.on_missing == EvidencePolicyMode::kHardExclude) {
        verdict.reason = ExclusionReason::kEvidenceMissing;
        return verdict;
      }
    } else {
      verdict.evidence_known = true;
    }
  }
  verdict.legal = true;
  return verdict;
}

std::uint64_t TierOrdinal(PathTier tier) { return static_cast<std::uint64_t>(tier); }

struct RankEntry {
  const CandidatePath* path = nullptr;
  const PathCapacity* capacity = nullptr;
  bool evidence_known = false;
  std::uint32_t utilization_ppb = 0;
  bool is_incumbent = false;
  bool multiplier_relaxed = false;
  bool reservation_relaxed = false;
  bool has_unknown = false;
  std::vector<ObjectiveValue> keys;
};

RankEntry BuildRankEntry(const PreparedRequest& prepared, const CandidateVerdict& verdict,
                         const CandidatePath& path) {
  const PlacementRequest& request = prepared.request;
  const PlacementPolicy& policy = request.policy;
  RankEntry entry;
  entry.path = &path;
  entry.capacity = LookupById(prepared.capacity_by_path,
                              [](const PathCapacity& e) { return e.path; }, path.id);
  const CongestionEvidenceEntry* evidence = LookupById(
      prepared.evidence_by_path, [](const CongestionEvidenceEntry& e) { return e.path; }, path.id);
  entry.evidence_known = verdict.evidence_known && evidence != nullptr;
  entry.utilization_ppb = evidence != nullptr ? evidence->utilization_ppb : 0;
  entry.is_incumbent =
      prepared.incumbent_present && request.incumbent->path == path.id && prepared.incumbent_eligible;
  entry.multiplier_relaxed = verdict.multiplier_relaxed;
  entry.reservation_relaxed = verdict.reservation_relaxed;

  entry.keys.reserve(policy.objectives.size());
  for (const Objective& objective : policy.objectives) {
    ObjectiveValue value;
    value.kind = objective.kind;
    value.direction = objective.direction;
    value.known = true;
    switch (objective.kind) {
      case ObjectiveKind::kCost:
        value.value = path.attributes.cost_micro;
        break;
      case ObjectiveKind::kLatency:
        value.value = path.attributes.latency_nanos;
        break;
      case ObjectiveKind::kResidualCapacity:
        value.value = entry.capacity != nullptr ? entry.capacity->residual_bytes : 0;
        break;
      case ObjectiveKind::kHopCount:
        value.value = path.attributes.hop_count;
        break;
      case ObjectiveKind::kCongestionUtilization:
        if (entry.evidence_known) {
          value.value = entry.utilization_ppb;
        } else {
          value.known = false;
        }
        break;
      case ObjectiveKind::kTier:
        value.value = TierOrdinal(path.attributes.tier);
        break;
      case ObjectiveKind::kLocalityAffinity:
        value.value = (policy.locality.preferred_locality &&
                       path.attributes.locality != *policy.locality.preferred_locality)
                          ? 1u
                          : 0u;
        break;
      case ObjectiveKind::kReservationAffinity: {
        bool matched = prepared.affinity_refs.empty();
        for (const ReservationRef& ref : path.attributes.reservations) {
          if (std::binary_search(prepared.affinity_refs.begin(), prepared.affinity_refs.end(), ref)) {
            matched = true;
            break;
          }
        }
        value.value = matched ? 0u : 1u;
        break;
      }
      case ObjectiveKind::kIncumbentStability:
        value.value = entry.is_incumbent ? 0u : (prepared.incumbent_eligible ? 1u : 0u);
        break;
    }
    if (!value.known) entry.has_unknown = true;
    entry.keys.push_back(value);
  }
  return entry;
}

bool RankLess(const RankEntry& a, const RankEntry& b) {
  const std::size_t count = std::min(a.keys.size(), b.keys.size());
  for (std::size_t i = 0; i < count; ++i) {
    const ObjectiveValue& x = a.keys[i];
    const ObjectiveValue& y = b.keys[i];
    if (x.known != y.known) return x.known;  // UNKNOWN sorts last, always
    if (x.known && x.value != y.value) {
      return x.direction == Direction::kMinimize ? x.value < y.value : y.value < x.value;
    }
  }
  return a.path->id < b.path->id;  // canonical total-order tie-break
}

class ExclusionRecorder {
 public:
  ExclusionRecorder(std::uint64_t max_reasons, std::uint64_t max_samples)
      : max_reasons_(max_reasons), max_samples_(max_samples) {}

  // The example constraint values are taken from the candidate with the lowest
  // path id, so that the recorded explanation does not depend on the order in
  // which candidates were supplied.
  void Add(ExclusionReason reason, const CandidateVerdict& verdict, PathId path) {
    auto it = entries_.find(reason);
    if (it == entries_.end()) {
      if (entries_.size() >= max_reasons_) {
        truncated_ = true;
        return;
      }
      Entry entry;
      entry.summary.reason = reason;
      entry.required = verdict.required;
      entry.available = verdict.available;
      entry.example = path;
      it = entries_.emplace(reason, std::move(entry)).first;
    } else if (path < it->second.example) {
      it->second.example = path;
      it->second.required = verdict.required;
      it->second.available = verdict.available;
    }
    it->second.summary.count += 1;
    std::vector<PathId>& sample = it->second.summary.sample;
    if (sample.size() < max_samples_) {
      sample.push_back(path);
      std::sort(sample.begin(), sample.end());
    } else {
      // The sample keeps the lowest path ids seen so far, so that the
      // explanation does not depend on the order in which candidates arrived.
      truncated_ = true;
      if (!sample.empty() && path < sample.back()) {
        sample.back() = path;
        std::sort(sample.begin(), sample.end());
      }
    }
  }

  [[nodiscard]] std::uint64_t Count(ExclusionReason reason) const {
    const auto it = entries_.find(reason);
    return it == entries_.end() ? 0 : it->second.summary.count;
  }
  [[nodiscard]] bool Truncated() const { return truncated_; }

  [[nodiscard]] std::vector<ExclusionSummary> Summaries() const {
    std::vector<ExclusionSummary> out;
    out.reserve(entries_.size());
    for (const auto& pair : entries_) out.push_back(pair.second.summary);
    return out;  // std::map iteration is ordered by reason ordinal: canonical
  }

  [[nodiscard]] std::uint64_t FirstRequired(ExclusionReason reason) const {
    const auto it = entries_.find(reason);
    return it == entries_.end() ? 0 : it->second.required;
  }
  [[nodiscard]] std::uint64_t FirstAvailable(ExclusionReason reason) const {
    const auto it = entries_.find(reason);
    return it == entries_.end() ? 0 : it->second.available;
  }
  [[nodiscard]] PathId FirstPath(ExclusionReason reason) const {
    const auto it = entries_.find(reason);
    if (it == entries_.end()) return PathId{};
    return it->second.example;
  }
  [[nodiscard]] bool ReasonIsOnly(ExclusionReason reason) const {
    return entries_.size() == 1 && entries_.find(reason) != entries_.end();
  }

 private:
  struct Entry {
    ExclusionSummary summary;
    std::uint64_t required = 0;
    std::uint64_t available = 0;
    PathId example;
  };
  std::map<ExclusionReason, Entry> entries_;
  std::uint64_t max_reasons_;
  std::uint64_t max_samples_;
  bool truncated_ = false;
};

AuthorityVector BuildAuthority(const PlacementRequest& request,
                               const AuthorityExpectation& expected) {
  AuthorityVector authority;
  authority.path_authority = expected.path_authority;
  authority.candidate_set = request.candidates.id;
  authority.candidate_set_generation = request.candidates.generation;
  authority.capacity = request.capacity.id;
  authority.capacity_generation = request.capacity.generation;
  authority.policy = request.policy.id;
  authority.policy_generation = request.policy.generation;
  authority.qos = request.qos.id;
  authority.qos_generation = request.qos.generation;
  authority.evidence = request.evidence.id;
  authority.evidence_generation = request.evidence.generation;
  authority.fabric_epoch = expected.fabric_epoch;
  return authority;
}

void AddNote(Explanation* explanation, std::string note) {
  constexpr std::size_t kMaxNotes = 16;
  if (explanation->notes.size() >= kMaxNotes) {
    explanation->truncated = true;
    return;
  }
  explanation->notes.push_back(std::move(note));
}

void AddDegradation(Explanation* explanation, DegradationFlag flag) {
  if (std::find(explanation->degradations.begin(), explanation->degradations.end(), flag) ==
      explanation->degradations.end()) {
    explanation->degradations.push_back(flag);
    std::sort(explanation->degradations.begin(), explanation->degradations.end());
  }
}

void AddBinding(Explanation* explanation, ExclusionReason kind, PathId path, std::uint64_t required,
                std::uint64_t available, std::string label) {
  constexpr std::size_t kMaxBinding = 8;
  if (explanation->binding.size() >= kMaxBinding) {
    explanation->truncated = true;
    return;
  }
  BindingConstraint constraint;
  constraint.kind = kind;
  constraint.path = path;
  constraint.required = required;
  constraint.available = available;
  constraint.label = std::move(label);
  explanation->binding.push_back(std::move(constraint));
}

Outcome OutcomeForValidationCode(StatusCode code) {
  switch (code) {
    case StatusCode::kStaleAuthorityGeneration:
    case StatusCode::kStaleCandidateSet:
    case StatusCode::kStaleCapacitySnapshot:
    case StatusCode::kStalePolicy:
    case StatusCode::kStaleQos:
    case StatusCode::kStaleFabricEpoch:
    case StatusCode::kStaleEvidence:
      return Outcome::kStaleInput;
    default:
      return Outcome::kConflictingInput;
  }
}

PlacementDecision Reject(StatusCode code, std::string message) {
  PlacementDecision decision;
  decision.outcome = OutcomeForValidationCode(code);
  decision.code = code;
  decision.validation = Status(code, message);
  AddNote(&decision.explanation, std::move(message));
  decision.digest = DecisionDigest(decision);
  return decision;
}

}  // namespace

PlacementDecision PlacementEngine::Place(const PlacementRequest& request) const {
  const Status validation = ValidateRequest(request, limits_);
  if (!validation.ok()) {
    PlacementDecision decision = Reject(validation.code(), std::string(validation.message()));
    decision.authority = BuildAuthority(request, request.expected);
    decision.digest = DecisionDigest(decision);
    return decision;
  }

  const AuthorityExpectation& expected = request.expected;
  PlacementDecision decision;
  decision.authority = BuildAuthority(request, expected);
  decision.explanation.candidate_count = request.candidates.paths.size();

  // ---- request-level generation staleness (fixed order) --------------------
  struct StaleCheck {
    bool stale;
    StatusCode code;
    const char* message;
  };
  const StaleCheck checks[] = {
      {request.candidates.generation != expected.candidate_set, StatusCode::kStaleCandidateSet,
       "candidate set generation is not the expected generation"},
      {request.capacity.generation != expected.capacity, StatusCode::kStaleCapacitySnapshot,
       "capacity snapshot generation is not the expected generation"},
      {request.policy.generation != expected.policy, StatusCode::kStalePolicy,
       "policy generation is not the expected generation"},
      {request.qos.generation != expected.qos, StatusCode::kStaleQos,
       "qos generation is not the expected generation"},
  };
  for (const StaleCheck& check : checks) {
    if (check.stale) {
      decision.outcome = Outcome::kStaleInput;
      decision.code = check.code;
      decision.validation = Status(check.code, check.message);
      AddNote(&decision.explanation, check.message);
      decision.digest = DecisionDigest(decision);
      return decision;
    }
  }

  const PreparedRequest prepared = Prepare(request, limits_);
  const PlacementPolicy& policy = request.policy;
  if (prepared.headroom_requirement_overflow) {
    decision.outcome = Outcome::kConflictingInput;
    decision.code = StatusCode::kArithmeticOverflow;
    decision.validation = Status(StatusCode::kArithmeticOverflow,
                                 "service-class headroom requirement is not representable");
    AddNote(&decision.explanation,
            "the service-class headroom requirement overflows 64-bit capacity arithmetic");
    decision.digest = DecisionDigest(decision);
    return decision;
  }
  // The incumbent relation is reported by every outcome, including the ones
  // that produce no placement, so it is resolved before any early return.
  if (!prepared.incumbent_present) {
    decision.delta = IncumbentDelta::kNoIncumbent;
  } else if (prepared.incumbent_eligible) {
    decision.delta = IncumbentDelta::kKept;
  } else {
    // An ineligible incumbent is never re-affirmed, so the relation is
    // "replaced" even when the flow ends up unplaced.
    decision.delta = IncumbentDelta::kReplacedStale;
  }

  // ---- per-candidate hard exclusions --------------------------------------
  ExclusionRecorder exclusions(limits_.max_exclusion_reasons, limits_.max_exclusion_samples);
  std::vector<RankEntry> legal;
  legal.reserve(request.candidates.paths.size());
  std::vector<FailureDomainId> legal_domains;

  for (const CandidatePath& path : request.candidates.paths) {
    const CandidateVerdict verdict = Evaluate(prepared, path);
    if (!verdict.legal) {
      exclusions.Add(verdict.reason, verdict, path.id);
      continue;
    }
    RankEntry entry = BuildRankEntry(prepared, verdict, path);
    if (entry.has_unknown) {
      decision.explanation.unknown_evidence_count += 1;
    }
    if (path.attributes.failure_domain.valid()) {
      legal_domains.push_back(path.attributes.failure_domain);
    }
    legal.push_back(std::move(entry));
  }
  std::sort(legal_domains.begin(), legal_domains.end());
  legal_domains.erase(std::unique(legal_domains.begin(), legal_domains.end()), legal_domains.end());
  decision.explanation.distinct_failure_domains = legal_domains.size();
  decision.explanation.legal_candidate_count = legal.size();
  decision.explanation.excluded_candidate_count =
      decision.explanation.candidate_count - legal.size();
  decision.explanation.exclusions = exclusions.Summaries();
  decision.explanation.truncated = decision.explanation.truncated || exclusions.Truncated();
  if (!prepared.evidence_trusted && prepared.evidence_bundle_present) {
    AddNote(&decision.explanation,
            "supplied evidence bundle is not bound to the expected evidence generation and was "
            "treated as untrusted");
  }
  if (policy.evidence.on_missing == EvidencePolicyMode::kRankWorst &&
      prepared.congestion_objective_used && !prepared.evidence_bundle_present) {
    AddNote(&decision.explanation, "congestion evidence is absent; ranking keys are UNKNOWN");
  }

  // ---- empty legal set ----------------------------------------------------
  // Classification is by category, not by a single reason: independent hard
  // constraints may remove different candidates for different reasons, and the
  // outcome must still name the category that actually blocked the flow.
  if (legal.empty()) {
    const std::vector<ExclusionSummary>& summaries = decision.explanation.exclusions;
    bool all_staleness = !summaries.empty();
    bool all_policy = !summaries.empty();
    bool all_capacity = !summaries.empty();
    bool all_service = !summaries.empty();
    bool all_qos = !summaries.empty();
    for (const ExclusionSummary& summary : summaries) {
      all_staleness = all_staleness && IsStalenessReason(summary.reason);
      all_policy = all_policy && IsPolicyReason(summary.reason);
      all_capacity = all_capacity && IsCapacityReason(summary.reason);
      all_service = all_service && IsServiceReason(summary.reason);
      all_qos = all_qos && IsQosReason(summary.reason);
    }
    const std::uint64_t stale_paths = exclusions.Count(ExclusionReason::kPathAuthorityStale);
    if (decision.explanation.candidate_count == 0) {
      decision.outcome = Outcome::kNoLegalPath;
      decision.code = StatusCode::kNotFound;
      AddNote(&decision.explanation, "the authorized candidate set is empty");
    } else if (stale_paths == decision.explanation.candidate_count) {
      decision.outcome = Outcome::kStaleInput;
      decision.code = StatusCode::kStaleAuthorityGeneration;
      AddNote(&decision.explanation,
              "no candidate carries the expected path authority generation");
    } else if (all_staleness || exclusions.ReasonIsOnly(ExclusionReason::kEvidenceStale)) {
      decision.outcome = Outcome::kStaleInput;
      decision.code = StatusCode::kStaleEvidence;
      AddNote(&decision.explanation, "every candidate was excluded by stale evidence");
    } else if (all_policy) {
      decision.outcome = Outcome::kPolicyRejected;
      decision.code = StatusCode::kPolicyRejectedAllPaths;
      AddNote(&decision.explanation, "policy excluded every candidate that is still authorized");
    } else if (all_service) {
      decision.outcome = Outcome::kNoLegalPath;
      decision.code = StatusCode::kServiceClassUnsatisfied;
      AddNote(&decision.explanation,
              "no candidate satisfies the service-class requirements of this flow");
    } else if (all_capacity) {
      decision.outcome = Outcome::kInsufficientCapacity;
      if (exclusions.ReasonIsOnly(ExclusionReason::kCapacityUnknown)) {
        decision.code = StatusCode::kNotFound;
        AddNote(&decision.explanation,
                "capacity evidence is absent for every candidate; absence is not capacity");
      } else {
        decision.code = StatusCode::kCapacityInsufficient;
        AddNote(&decision.explanation, "no candidate reports sufficient residual capacity");
      }
    } else if (all_qos) {
      decision.outcome = Outcome::kNoLegalPath;
      decision.code = StatusCode::kLatencyBudgetUnmet;
      AddNote(&decision.explanation, "no candidate meets the latency budget");
    } else {
      decision.outcome = Outcome::kNoLegalPath;
      decision.code = StatusCode::kNotFound;
      AddNote(&decision.explanation,
              "no candidate is legal; independent hard constraints removed every candidate");
    }
    const ExclusionSummary* dominant = nullptr;
    for (const ExclusionSummary& summary : decision.explanation.exclusions) {
      if (dominant == nullptr || summary.count > dominant->count) dominant = &summary;
    }
    if (dominant != nullptr) {
      AddBinding(&decision.explanation, dominant->reason, exclusions.FirstPath(dominant->reason),
                 exclusions.FirstRequired(dominant->reason),
                 exclusions.FirstAvailable(dominant->reason),
                 "excluded=" + std::to_string(dominant->count) + " of " +
                     std::to_string(decision.explanation.candidate_count));
    }
    decision.digest = DecisionDigest(decision);
    return decision;
  }

  // ---- failure-domain diversity over the legal set ------------------------
  if (policy.failure_domains.min_distinct_domains != 0 &&
      decision.explanation.distinct_failure_domains < policy.failure_domains.min_distinct_domains) {
    decision.outcome = Outcome::kNoLegalPath;
    decision.code = StatusCode::kFailureDomainDiversityUnmet;
    AddNote(&decision.explanation,
            "legal candidates span " +
                std::to_string(decision.explanation.distinct_failure_domains) +
                " distinct failure domains but the policy requires " +
                std::to_string(policy.failure_domains.min_distinct_domains));
    AddBinding(&decision.explanation, ExclusionReason::kForbiddenFailureDomain, PathId{},
               policy.failure_domains.min_distinct_domains,
               decision.explanation.distinct_failure_domains, "diversity requirement");
    decision.digest = DecisionDigest(decision);
    return decision;
  }

  // ---- deterministic ranking ---------------------------------------------
  std::sort(legal.begin(), legal.end(), RankLess);
  const std::size_t explained = static_cast<std::size_t>(std::min<std::uint64_t>(
      legal.size(), limits_.max_ranked_explained));
  decision.explanation.ranked.reserve(explained);
  for (std::size_t i = 0; i < explained; ++i) {
    RankedCandidate ranked;
    ranked.path = legal[i].path->id;
    ranked.authority = legal[i].path->authority;
    ranked.rank = static_cast<std::uint32_t>(i);
    ranked.is_incumbent = legal[i].is_incumbent;
    ranked.keys = legal[i].keys;
    decision.explanation.ranked.push_back(std::move(ranked));
  }
  if (explained < legal.size()) decision.explanation.truncated = true;

  // ---- churn / incumbent stability ---------------------------------------
  const bool incumbent_present = prepared.incumbent_present;
  const bool incumbent_eligible = prepared.incumbent_eligible;
  std::size_t chosen_index = 0;
  if (incumbent_present) {
    if (!incumbent_eligible) {
      AddDegradation(&decision.explanation, DegradationFlag::kIncumbentStale);
      const IncumbentPlacement& incumbent = *request.incumbent;
      std::string why = "incumbent placement " + incumbent.id.ToString() +
                        " is not eligible: stale authority generation or path no longer authorized";
      AddNote(&decision.explanation, why);
    } else if (policy.churn.prefer_incumbent) {
      const std::size_t threshold_index = policy.churn.threshold_objective_index;
      std::size_t incumbent_index = legal.size();
      for (std::size_t i = 0; i < legal.size(); ++i) {
        if (legal[i].path->id == request.incumbent->path) {
          incumbent_index = i;
          break;
        }
      }
      if (incumbent_index != 0 && incumbent_index < legal.size()) {
        const ObjectiveValue& best = legal[0].keys[threshold_index];
        const ObjectiveValue& current = legal[incumbent_index].keys[threshold_index];
        std::uint64_t improvement = 0;
        if (best.known && current.known) {
          improvement = best.direction == Direction::kMinimize
                            ? (current.value > best.value ? current.value - best.value : 0)
                            : (best.value > current.value ? best.value - current.value : 0);
        }
        if (improvement <= policy.churn.move_improvement_threshold) {
          chosen_index = incumbent_index;
          AddDegradation(&decision.explanation, DegradationFlag::kChurnSuppressed);
          AddNote(&decision.explanation,
                  "churn suppressed: the best candidate improves the incumbent by " +
                      std::to_string(improvement) + " which does not exceed the move threshold " +
                      std::to_string(policy.churn.move_improvement_threshold));
        }
      }
    }
  } else {
    AddDegradation(&decision.explanation, DegradationFlag::kNoIncumbent);
  }

  const RankEntry& chosen = legal[chosen_index];
  const bool moving = incumbent_present && chosen.path->id != request.incumbent->path;

  // ---- churn gate: a policy that forbids moves defers instead ------------
  if (moving && policy.churn.prefer_incumbent && policy.churn.on_move == ChurnAction::kDefer) {
    const std::size_t incumbent_index = [&]() {
      for (std::size_t i = 0; i < legal.size(); ++i) {
        if (legal[i].path->id == request.incumbent->path) return i;
      }
      return legal.size();
    }();
    const bool incumbent_is_chosen = incumbent_index == chosen_index;
    if (!incumbent_is_chosen) {
      decision.outcome = Outcome::kDeferred;
      decision.code = StatusCode::kPolicyMoveSuppressed;
      decision.delta = incumbent_eligible ? IncumbentDelta::kMoved : IncumbentDelta::kReplacedStale;
      AddNote(&decision.explanation,
              "policy defers moves away from the incumbent; the flow remains unplaced");
      decision.digest = DecisionDigest(decision);
      return decision;
    }
  }

  // ---- admission comfort gate --------------------------------------------
  const std::uint64_t residual =
      chosen.capacity != nullptr ? chosen.capacity->residual_bytes : 0;
  if (policy.admission.min_residual_headroom_bytes != 0 &&
      residual < policy.admission.min_residual_headroom_bytes) {
    if (policy.admission.on_below == GateAction::kDefer) {
      decision.outcome = Outcome::kDeferred;
      decision.code = StatusCode::kPolicyHeadroomGate;
      AddBinding(&decision.explanation, ExclusionReason::kCapacityInsufficient, chosen.path->id,
                 policy.admission.min_residual_headroom_bytes, residual, "admission gate");
      AddNote(&decision.explanation,
              "residual capacity headroom is below the admission gate; the decision is deferred");
      decision.digest = DecisionDigest(decision);
      return decision;
    }
    AddDegradation(&decision.explanation, DegradationFlag::kHeadroomBelowComfort);
    AddBinding(&decision.explanation, ExclusionReason::kCapacityInsufficient, chosen.path->id,
               policy.admission.min_residual_headroom_bytes, residual, "admission gate");
  }

  // ---- degradation flags --------------------------------------------------
  if (chosen.multiplier_relaxed || chosen.reservation_relaxed) {
    AddDegradation(&decision.explanation, DegradationFlag::kServiceRelaxed);
    AddBinding(&decision.explanation, ExclusionReason::kServiceHeadroomUnmet, chosen.path->id,
               prepared.required_headroom_bytes, residual, "service-class headroom relaxed");
  }
  if (policy.locality.preferred_locality &&
      chosen.path->attributes.locality != *policy.locality.preferred_locality) {
    AddDegradation(&decision.explanation, DegradationFlag::kLocalityPreferenceUnmet);
  }
  if (!prepared.affinity_refs.empty()) {
    bool matched = false;
    for (const ReservationRef& ref : chosen.path->attributes.reservations) {
      if (std::binary_search(prepared.affinity_refs.begin(), prepared.affinity_refs.end(), ref)) {
        matched = true;
        break;
      }
    }
    if (!matched) AddDegradation(&decision.explanation, DegradationFlag::kReservationPreferenceUnmet);
  }
  if (chosen.has_unknown) {
    AddDegradation(&decision.explanation, DegradationFlag::kEvidenceUnknown);
  }

  // ---- incumbent delta ----------------------------------------------------
  if (!incumbent_present) {
    decision.delta = IncumbentDelta::kNoIncumbent;
  } else if (chosen.path->id == request.incumbent->path) {
    // The path happens to be the incumbent's, but a stale incumbent is not
    // re-affirmed: the relation is reported as replaced.
    decision.delta =
        incumbent_eligible ? IncumbentDelta::kKept : IncumbentDelta::kReplacedStale;
  } else if (incumbent_eligible) {
    decision.delta = IncumbentDelta::kMoved;
    AddDegradation(&decision.explanation, DegradationFlag::kIncumbentSuperseded);
  } else {
    decision.delta = IncumbentDelta::kReplacedStale;
  }

  // ---- intent -------------------------------------------------------------
  PlacementIntent intent;
  intent.flow = request.flow;
  intent.flow_generation = request.flow_generation;
  intent.path = chosen.path->id;
  intent.path_authority = chosen.path->authority;
  intent.candidate_set = request.candidates.id;
  intent.candidate_set_generation = request.candidates.generation;
  intent.capacity = request.capacity.id;
  intent.capacity_generation = request.capacity.generation;
  intent.policy = request.policy.id;
  intent.policy_generation = request.policy.generation;
  intent.qos = request.qos.id;
  intent.qos_generation = request.qos.generation;
  intent.evidence = prepared.evidence_trusted ? request.evidence.id : EvidenceId{};
  intent.evidence_generation =
      prepared.evidence_trusted ? request.evidence.generation : EvidenceGeneration{};
  intent.fabric_epoch = expected.fabric_epoch;
  intent.attempt = request.attempt;
  intent.revalidate_after_nanos = policy.revalidate_after_nanos;
  if (policy.reservation_affinity.required || !prepared.affinity_refs.empty()) {
    for (const ReservationRef& ref : chosen.path->attributes.reservations) {
      if (std::binary_search(prepared.affinity_refs.begin(), prepared.affinity_refs.end(), ref)) {
        intent.reservation = ref;
        break;
      }
    }
  }
  if (!intent.reservation.valid() && prepared.service.reservation_required &&
      !chosen.path->attributes.reservations.empty()) {
    // The reservation list is a set: select its canonical (lowest) element so
    // that the supplied order cannot change the intent.
    intent.reservation = *std::min_element(chosen.path->attributes.reservations.begin(),
                                           chosen.path->attributes.reservations.end());
  }
  intent.digest = IntentDigest(intent);

  // ---- self-verification --------------------------------------------------
  std::string invariant_reason;
  if (!VerifyIntent(request, intent, &invariant_reason)) {
    PlacementDecision rejected = Reject(StatusCode::kInternalInvariantViolation,
                                        "placement self-verification failed: " + invariant_reason);
    rejected.authority = decision.authority;
    rejected.explanation.candidate_count = decision.explanation.candidate_count;
    rejected.explanation.legal_candidate_count = decision.explanation.legal_candidate_count;
    rejected.explanation.excluded_candidate_count = decision.explanation.excluded_candidate_count;
    rejected.explanation.exclusions = decision.explanation.exclusions;
    rejected.digest = DecisionDigest(rejected);
    return rejected;
  }

  AddBinding(&decision.explanation, ExclusionReason::kCapacityInsufficient, chosen.path->id,
             prepared.required_headroom_bytes, residual, "chosen path headroom");

  bool degrading = false;
  for (const DegradationFlag flag : decision.explanation.degradations) {
    if (IsDegradingFlag(flag)) degrading = true;
  }
  decision.outcome = degrading ? Outcome::kPlacedDegraded : Outcome::kPlaced;
  decision.code = StatusCode::kOk;
  decision.intent = intent;
  decision.digest = DecisionDigest(decision);
  return decision;
}

bool PlacementEngine::VerifyIntent(const PlacementRequest& request, const PlacementIntent& intent,
                                   std::string* reason) const {
  const auto fail = [reason](const std::string& message) {
    if (reason != nullptr) *reason = message;
    return false;
  };
  if (!ValidateRequest(request, limits_).ok()) {
    return fail("request is not valid");
  }
  if (intent.flow != request.flow || intent.flow_generation != request.flow_generation) {
    return fail("intent flow does not match the request");
  }
  if (intent.path_authority != request.expected.path_authority) {
    return fail("intent path authority generation is not the expected authority generation");
  }
  if (intent.candidate_set != request.candidates.id ||
      intent.candidate_set_generation != request.candidates.generation) {
    return fail("intent is not bound to the request candidate set generation");
  }
  if (intent.capacity != request.capacity.id ||
      intent.capacity_generation != request.capacity.generation) {
    return fail("intent is not bound to the request capacity snapshot generation");
  }
  if (intent.policy != request.policy.id || intent.policy_generation != request.policy.generation) {
    return fail("intent is not bound to the request policy generation");
  }
  if (intent.qos != request.qos.id || intent.qos_generation != request.qos.generation) {
    return fail("intent is not bound to the request qos generation");
  }
  if (intent.fabric_epoch != request.expected.fabric_epoch) {
    return fail("intent is not bound to the expected fabric epoch");
  }
  if (intent.evidence_generation.valid()) {
    if (intent.evidence_generation != request.evidence.generation) {
      return fail("intent references an evidence generation that is not the supplied one");
    }
    if (intent.evidence != request.evidence.id) {
      return fail("intent references an evidence bundle identity that is not the supplied one");
    }
    if (!request.expected.evidence.valid() ||
        intent.evidence_generation != request.expected.evidence) {
      return fail("intent is bound to an evidence generation the request does not trust");
    }
  } else if (intent.evidence.valid()) {
    return fail("intent carries an evidence identity without an evidence generation");
  }

  const PreparedRequest prepared = Prepare(request, limits_);
  const CandidatePath* path = LookupById(
      prepared.candidates_by_id, [](const CandidatePath& p) { return p.id; }, intent.path);
  if (path == nullptr) {
    return fail("chosen path " + intent.path.ToString() +
                " is not a member of the authorized candidate set");
  }
  if (path->authority != intent.path_authority) {
    return fail("chosen path authority generation does not match the intent");
  }
  const CandidateVerdict verdict = Evaluate(prepared, *path);
  if (!verdict.legal) {
    return fail(std::string("chosen path fails a hard constraint: ") +
                std::string(ExclusionReasonName(verdict.reason)));
  }
  if (intent.reservation.valid()) {
    bool found = false;
    for (const ReservationRef& ref : path->attributes.reservations) {
      if (ref == intent.reservation) {
        found = true;
        break;
      }
    }
    if (!found) return fail("intent references a reservation the chosen path does not carry");
  }
  return true;
}

}  // namespace flowplace

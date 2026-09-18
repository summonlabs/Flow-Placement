// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Structural validation. Everything that can be decided without ranking is
// decided here: identity presence, bounds, duplicates, unrepresentable values,
// and constraints that contradict each other by construction. A request that
// fails validation never reaches ranking.

#include <algorithm>
#include <string>
#include <vector>

#include "flowplace/checked.hpp"
#include "flowplace/engine.hpp"

namespace flowplace {
namespace {

Status Reject(StatusCode code, std::string message) {
  return Status(code, std::move(message));
}

std::string CountMessage(std::string_view what, std::uint64_t count, std::uint64_t limit) {
  std::string out(what);
  out += " count ";
  out += std::to_string(count);
  out += " exceeds limit ";
  out += std::to_string(limit);
  return out;
}

template <class IdT>
std::vector<IdT> SortedUniqueCopy(const std::vector<IdT>& input, bool* had_duplicates) {
  std::vector<IdT> copy = input;
  std::sort(copy.begin(), copy.end());
  const auto duplicate = std::adjacent_find(copy.begin(), copy.end());
  *had_duplicates = duplicate != copy.end();
  copy.erase(std::unique(copy.begin(), copy.end()), copy.end());
  return copy;
}

template <class IdT>
bool Contains(const std::vector<IdT>& sorted, IdT value) {
  return std::binary_search(sorted.begin(), sorted.end(), value);
}

template <class IdT>
bool Intersects(const std::vector<IdT>& a, const std::vector<IdT>& b) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] == b[j]) return true;
    if (a[i] < b[j]) {
      ++i;
    } else {
      ++j;
    }
  }
  return false;
}

std::string IdListToString(const std::vector<PolicyLabelId>& ids) {
  std::string out;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i != 0) out += ",";
    out += ids[i].ToString();
  }
  return out;
}

// Enum-typed fields arrive from callers and from the wire; an out-of-range value
// would otherwise select an unspecified behaviour in comparisons.
Status ValidateEnumerations(const PlacementRequest& request) {
  const auto in_range = [](std::uint32_t value, std::uint32_t max_value) {
    return value <= max_value;
  };
  if (!in_range(static_cast<std::uint32_t>(request.qos.service_class), 3)) {
    return Reject(StatusCode::kInvalidFieldValue, "qos service class is out of range");
  }
  if (!in_range(static_cast<std::uint32_t>(request.qos.priority), 3)) {
    return Reject(StatusCode::kInvalidFieldValue, "qos priority class is out of range");
  }
  if (!in_range(static_cast<std::uint32_t>(request.policy.service.max_relaxation_steps), 2)) {
    return Reject(StatusCode::kInvalidFieldValue, "policy service relaxation is out of range");
  }
  if (!in_range(static_cast<std::uint32_t>(request.policy.admission.on_below), 1) ||
      !in_range(static_cast<std::uint32_t>(request.policy.churn.on_move), 1) ||
      !in_range(static_cast<std::uint32_t>(request.policy.evidence.on_missing), 1) ||
      !in_range(static_cast<std::uint32_t>(request.policy.evidence.on_stale), 1)) {
    return Reject(StatusCode::kInvalidFieldValue, "policy mode field is out of range");
  }
  for (const Objective& objective : request.policy.objectives) {
    if (!in_range(static_cast<std::uint32_t>(objective.kind), 8) ||
        !in_range(static_cast<std::uint32_t>(objective.direction), 1)) {
      return Reject(StatusCode::kInvalidFieldValue, "policy objective is out of range");
    }
  }
  for (const PathTier tier : request.policy.allowed_tiers) {
    if (!in_range(static_cast<std::uint32_t>(tier), 2)) {
      return Reject(StatusCode::kInvalidFieldValue, "policy allowed tier is out of range");
    }
  }
  for (const CandidatePath& path : request.candidates.paths) {
    if (!in_range(static_cast<std::uint32_t>(path.attributes.tier), 2) ||
        !in_range(static_cast<std::uint32_t>(path.attributes.locality_scope), 4)) {
      return Reject(StatusCode::kInvalidFieldValue,
                    "candidate path " + path.id.ToString() + " has an out-of-range attribute");
    }
  }
  return Status::Ok();
}

Status ValidateGenerations(const PlacementRequest& request) {
  if (!request.flow.valid()) return Reject(StatusCode::kInvalidId, "flow id must be non-zero");
  if (!request.flow_generation.valid()) {
    return Reject(StatusCode::kInvalidFieldValue, "flow generation must be non-zero");
  }
  if (!request.candidates.id.valid()) {
    return Reject(StatusCode::kInvalidId, "candidate set id must be non-zero");
  }
  if (!request.candidates.generation.valid()) {
    return Reject(StatusCode::kInvalidFieldValue, "candidate set generation must be non-zero");
  }
  if (!request.capacity.id.valid()) {
    return Reject(StatusCode::kInvalidId, "capacity snapshot id must be non-zero");
  }
  if (!request.capacity.generation.valid()) {
    return Reject(StatusCode::kInvalidFieldValue, "capacity snapshot generation must be non-zero");
  }
  if (!request.qos.id.valid()) return Reject(StatusCode::kInvalidId, "qos profile id must be non-zero");
  if (!request.qos.generation.valid()) {
    return Reject(StatusCode::kInvalidFieldValue, "qos generation must be non-zero");
  }
  if (!request.policy.id.valid()) return Reject(StatusCode::kInvalidId, "policy id must be non-zero");
  if (!request.policy.generation.valid()) {
    return Reject(StatusCode::kInvalidFieldValue, "policy generation must be non-zero");
  }
  if (!request.expected.path_authority.valid()) {
    return Reject(StatusCode::kMissingField,
                  "expected path authority generation must be declared (non-zero)");
  }
  if (!request.expected.candidate_set.valid() || !request.expected.capacity.valid() ||
      !request.expected.policy.valid() || !request.expected.qos.valid() ||
      !request.expected.fabric_epoch.valid()) {
    return Reject(StatusCode::kMissingField,
                  "expected candidate set, capacity, policy, qos, and fabric epoch generations "
                  "must all be declared (non-zero)");
  }
  if (request.evidence.id.valid() != request.evidence.generation.valid()) {
    return Reject(StatusCode::kInconsistentAuthorityGeneration,
                  "evidence bundle must carry both an id and a generation, or neither");
  }
  if (request.incumbent) {
    const IncumbentPlacement& incumbent = *request.incumbent;
    if (!incumbent.id.valid() || !incumbent.generation.valid() || !incumbent.path.valid() ||
        !incumbent.path_authority.valid() || !incumbent.candidate_set_generation.valid() ||
        !incumbent.capacity_generation.valid() || !incumbent.policy_generation.valid() ||
        !incumbent.qos_generation.valid() || !incumbent.fabric_epoch.valid()) {
      return Reject(StatusCode::kInvalidFieldValue,
                    "incumbent placement is missing a required identity or generation");
    }
    if (incumbent.reservation.id.valid() != incumbent.reservation.generation.valid()) {
      return Reject(StatusCode::kInconsistentAuthorityGeneration,
                    "incumbent reservation must carry both an id and a generation, or neither");
    }
  }
  return Status::Ok();
}

Status ValidateBounds(const PlacementRequest& request, const Limits& limits) {
  if (request.candidates.paths.size() > limits.max_candidate_paths) {
    return Reject(StatusCode::kOversizeRequest,
                  CountMessage("candidate path", request.candidates.paths.size(),
                               limits.max_candidate_paths));
  }
  if (request.capacity.entries.size() > limits.max_capacity_entries) {
    return Reject(StatusCode::kOversizeRequest,
                  CountMessage("capacity entry", request.capacity.entries.size(),
                               limits.max_capacity_entries));
  }
  if (request.evidence.entries.size() > limits.max_evidence_entries) {
    return Reject(StatusCode::kOversizeRequest,
                  CountMessage("evidence entry", request.evidence.entries.size(),
                               limits.max_evidence_entries));
  }
  if (request.occupied_failure_domains.size() > limits.max_occupied_failure_domains) {
    return Reject(StatusCode::kOversizeRequest,
                  CountMessage("occupied failure domain", request.occupied_failure_domains.size(),
                               limits.max_occupied_failure_domains));
  }
  if (request.policy.objectives.empty()) {
    return Reject(StatusCode::kMissingField,
                  "policy must declare at least one ranking objective; the engine applies no "
                  "implicit objective order");
  }
  if (request.policy.objectives.size() > limits.max_objectives) {
    return Reject(StatusCode::kOversizeRequest,
                  CountMessage("objective", request.policy.objectives.size(), limits.max_objectives));
  }
  if (request.policy.allowed_tiers.size() > 3) {
    return Reject(StatusCode::kInvalidFieldValue, "policy lists more tiers than exist");
  }
  if (request.policy.required_labels.size() > limits.max_policy_id_lists ||
      request.policy.forbidden_labels.size() > limits.max_policy_id_lists ||
      request.policy.locality.forbidden_localities.size() > limits.max_policy_id_lists ||
      request.policy.failure_domains.forbidden_domains.size() > limits.max_policy_id_lists) {
    return Reject(StatusCode::kOversizeRequest,
                  "policy id list exceeds the configured limit (see Limits::max_policy_id_lists)");
  }
  if (request.policy.reservation_affinity.refs.size() > limits.max_affinity_refs) {
    return Reject(StatusCode::kOversizeRequest,
                  CountMessage("reservation affinity ref",
                               request.policy.reservation_affinity.refs.size(),
                               limits.max_affinity_refs));
  }
  if (request.provenance.producer.size() > kMaxProvenanceFieldLength ||
      request.provenance.producer_version.size() > kMaxProvenanceFieldLength) {
    return Reject(StatusCode::kOversizeRequest,
                  "provenance field exceeds kMaxProvenanceFieldLength");
  }
  return Status::Ok();
}

Status ValidateCandidates(const PlacementRequest& request, const Limits& limits) {
  for (const CandidatePath& path : request.candidates.paths) {
    if (!path.id.valid()) return Reject(StatusCode::kInvalidId, "candidate path id must be non-zero");
    if (!path.authority.valid()) {
      return Reject(StatusCode::kInconsistentAuthorityGeneration,
                    "candidate path " + path.id.ToString() +
                        " has an unset path authority generation");
    }
    if (path.attributes.labels.size() > limits.max_path_labels) {
      return Reject(StatusCode::kOversizeRequest,
                    CountMessage("path label", path.attributes.labels.size(),
                                 limits.max_path_labels));
    }
    if (path.attributes.reservations.size() > limits.max_path_reservations) {
      return Reject(StatusCode::kOversizeRequest,
                    CountMessage("path reservation", path.attributes.reservations.size(),
                                 limits.max_path_reservations));
    }
    bool duplicate = false;
    const std::vector<PolicyLabelId> labels = SortedUniqueCopy(path.attributes.labels, &duplicate);
    if (duplicate) {
      return Reject(StatusCode::kInvalidFieldValue,
                    "candidate path " + path.id.ToString() + " lists a duplicate label");
    }
    std::vector<ReservationRef> reservations = path.attributes.reservations;
    std::sort(reservations.begin(), reservations.end());
    if (std::adjacent_find(reservations.begin(), reservations.end()) != reservations.end()) {
      return Reject(StatusCode::kInvalidFieldValue,
                    "candidate path " + path.id.ToString() +
                        " lists a duplicate reservation reference");
    }
    for (const ReservationRef& ref : reservations) {
      if (!ref.valid()) {
        return Reject(StatusCode::kInconsistentAuthorityGeneration,
                      "candidate path " + path.id.ToString() +
                          " carries a reservation reference without an id or generation");
      }
    }
  }

  std::vector<CandidatePath> sorted = request.candidates.paths;
  std::sort(sorted.begin(), sorted.end(),
            [](const CandidatePath& a, const CandidatePath& b) { return a.id < b.id; });
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    if (sorted[i].id == sorted[i - 1].id) {
      if (sorted[i].authority != sorted[i - 1].authority) {
        return Reject(StatusCode::kInconsistentAuthorityGeneration,
                      "candidate path " + sorted[i].id.ToString() +
                          " appears with two different path authority generations");
      }
      return Reject(StatusCode::kDuplicatePathId,
                    "candidate path " + sorted[i].id.ToString() + " appears more than once");
    }
  }
  return Status::Ok();
}

Status ValidateCapacityAndEvidence(const PlacementRequest& request) {
  std::vector<PathCapacity> capacity = request.capacity.entries;
  std::sort(capacity.begin(), capacity.end(),
            [](const PathCapacity& a, const PathCapacity& b) { return a.path < b.path; });
  for (std::size_t i = 0; i < capacity.size(); ++i) {
    if (!capacity[i].path.valid()) {
      return Reject(StatusCode::kInvalidId, "capacity entry path id must be non-zero");
    }
    if (capacity[i].residual_bytes > capacity[i].capacity_bytes) {
      return Reject(StatusCode::kInvalidFieldValue,
                    "capacity entry for path " + capacity[i].path.ToString() +
                        " reports residual capacity above total capacity");
    }
    if (i > 0 && capacity[i].path == capacity[i - 1].path) {
      return Reject(StatusCode::kDuplicatePathId,
                    "capacity snapshot lists path " + capacity[i].path.ToString() +
                        " more than once");
    }
  }

  std::vector<CongestionEvidenceEntry> evidence = request.evidence.entries;
  std::sort(evidence.begin(), evidence.end(),
            [](const CongestionEvidenceEntry& a, const CongestionEvidenceEntry& b) {
              return a.path < b.path;
            });
  for (std::size_t i = 0; i < evidence.size(); ++i) {
    if (!evidence[i].path.valid()) {
      return Reject(StatusCode::kInvalidId, "evidence entry path id must be non-zero");
    }
    if (evidence[i].utilization_ppb > 1000000000u) {
      return Reject(StatusCode::kInvalidFieldValue,
                    "evidence entry for path " + evidence[i].path.ToString() +
                        " reports utilization above 1e9 ppb");
    }
    if (i > 0 && evidence[i].path == evidence[i - 1].path) {
      return Reject(StatusCode::kDuplicatePathId,
                    "evidence bundle lists path " + evidence[i].path.ToString() +
                        " more than once");
    }
  }
  return Status::Ok();
}

Status ValidatePolicy(const PlacementRequest& request) {
  const PlacementPolicy& policy = request.policy;

  std::vector<ObjectiveKind> kinds;
  kinds.reserve(policy.objectives.size());
  for (const Objective& objective : policy.objectives) {
    kinds.push_back(objective.kind);
  }
  std::sort(kinds.begin(), kinds.end());
  if (std::adjacent_find(kinds.begin(), kinds.end()) != kinds.end()) {
    return Reject(StatusCode::kInvalidFieldValue,
                  "policy lists the same objective kind more than once");
  }

  bool duplicate = false;
  const std::vector<PolicyLabelId> required =
      SortedUniqueCopy(policy.required_labels, &duplicate);
  if (duplicate) {
    return Reject(StatusCode::kInvalidFieldValue, "policy repeats a required label");
  }
  const std::vector<PolicyLabelId> forbidden =
      SortedUniqueCopy(policy.forbidden_labels, &duplicate);
  if (duplicate) {
    return Reject(StatusCode::kInvalidFieldValue, "policy repeats a forbidden label");
  }
  if (Intersects(required, forbidden)) {
    return Reject(StatusCode::kContradictoryConstraints,
                  "policy requires and forbids the same label(s): " + IdListToString(required));
  }

  const std::vector<LocalityId> forbidden_localities =
      SortedUniqueCopy(policy.locality.forbidden_localities, &duplicate);
  if (duplicate) return Reject(StatusCode::kInvalidFieldValue, "policy repeats a forbidden locality");
  if (policy.locality.required_locality &&
      Contains(forbidden_localities, *policy.locality.required_locality)) {
    return Reject(StatusCode::kContradictoryConstraints,
                  "policy requires locality " + policy.locality.required_locality->ToString() +
                      " and also forbids it");
  }

  const std::vector<FailureDomainId> forbidden_domains =
      SortedUniqueCopy(policy.failure_domains.forbidden_domains, &duplicate);
  if (duplicate) {
    return Reject(StatusCode::kInvalidFieldValue, "policy repeats a forbidden failure domain");
  }
  if (policy.failure_domains.required_domain &&
      Contains(forbidden_domains, *policy.failure_domains.required_domain)) {
    return Reject(StatusCode::kContradictoryConstraints,
                  "policy requires failure domain " +
                      policy.failure_domains.required_domain->ToString() + " and also forbids it");
  }

  if (policy.reservation_affinity.required && policy.reservation_affinity.refs.empty()) {
    return Reject(StatusCode::kContradictoryConstraints,
                  "policy requires reservation affinity but lists no reservation reference");
  }
  for (const ReservationRef& ref : policy.reservation_affinity.refs) {
    if (!ref.valid()) {
      return Reject(StatusCode::kInconsistentAuthorityGeneration,
                    "policy reservation affinity reference is missing an id or generation");
    }
  }

  if (policy.churn.threshold_objective_index >= policy.objectives.size()) {
    return Reject(StatusCode::kInvalidFieldValue,
                  "policy churn threshold objective index is outside the objective list");
  }
  if (policy.service.max_relaxation_steps > 2) {
    return Reject(StatusCode::kInvalidFieldValue,
                  "policy service relaxation allows at most 2 steps");
  }
  if (!policy.service.allow_degraded && policy.service.max_relaxation_steps != 0) {
    return Reject(StatusCode::kInvalidFieldValue,
                  "policy allows relaxation steps without allowing degraded service");
  }
  if (policy.churn.on_move == ChurnAction::kDefer && !policy.churn.prefer_incumbent) {
    return Reject(StatusCode::kInvalidFieldValue,
                  "policy defers moves without preferring the incumbent; the rule would be inert");
  }
  if (policy.allowed_tiers.size() > 1) {
    std::vector<PathTier> tiers = policy.allowed_tiers;
    std::sort(tiers.begin(), tiers.end());
    if (std::adjacent_find(tiers.begin(), tiers.end()) != tiers.end()) {
      return Reject(StatusCode::kInvalidFieldValue, "policy repeats an allowed tier");
    }
  }

  // The service-class headroom requirement must be representable.
  const std::uint64_t multiplier = ServiceHeadroomMultiplier(request.qos.service_class);
  if (!CheckedMul(request.qos.required_residual_bytes, multiplier).has_value()) {
    return Reject(StatusCode::kArithmeticOverflow,
                  "service-class headroom requirement overflows 64-bit capacity arithmetic");
  }

  if (policy.failure_domains.min_distinct_domains != 0) {
    std::vector<FailureDomainId> domains;
    domains.reserve(request.candidates.paths.size());
    for (const CandidatePath& path : request.candidates.paths) {
      if (path.attributes.failure_domain.valid()) domains.push_back(path.attributes.failure_domain);
    }
    std::sort(domains.begin(), domains.end());
    domains.erase(std::unique(domains.begin(), domains.end()), domains.end());
    if (static_cast<std::uint64_t>(policy.failure_domains.min_distinct_domains) > domains.size()) {
      return Reject(StatusCode::kContradictoryConstraints,
                    "policy requires at least " +
                        std::to_string(policy.failure_domains.min_distinct_domains) +
                        " distinct failure domains but the candidate set spans only " +
                        std::to_string(domains.size()));
    }
  }
  return Status::Ok();
}

}  // namespace

Status ValidateRequest(const PlacementRequest& request, const Limits& limits) {
  Status status = ValidateEnumerations(request);
  if (!status.ok()) return status;
  status = ValidateGenerations(request);
  if (!status.ok()) return status;
  status = ValidateBounds(request, limits);
  if (!status.ok()) return status;
  status = ValidateCandidates(request, limits);
  if (!status.ok()) return status;
  status = ValidateCapacityAndEvidence(request);
  if (!status.ok()) return status;
  return ValidatePolicy(request);
}

}  // namespace flowplace

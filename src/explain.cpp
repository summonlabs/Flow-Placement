// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/explain.hpp"

#include <algorithm>

#include "flowplace/hash.hpp"

namespace flowplace {
namespace {

void HashObjectiveValue(CanonicalHasher& h, const ObjectiveValue& v) {
  h.AddU8(static_cast<std::uint8_t>(v.kind));
  h.AddU8(static_cast<std::uint8_t>(v.direction));
  h.AddBool(v.known);
  h.AddU64(v.value);
}

}  // namespace

std::string_view OutcomeName(Outcome v) noexcept {
  switch (v) {
    case Outcome::kPlaced: return "PLACED";
    case Outcome::kPlacedDegraded: return "PLACED_DEGRADED";
    case Outcome::kDeferred: return "DEFERRED";
    case Outcome::kNoLegalPath: return "NO_LEGAL_PATH";
    case Outcome::kInsufficientCapacity: return "INSUFFICIENT_CAPACITY";
    case Outcome::kPolicyRejected: return "POLICY_REJECTED";
    case Outcome::kStaleInput: return "STALE_INPUT";
    case Outcome::kConflictingInput: return "CONFLICTING_INPUT";
  }
  return "UNKNOWN_OUTCOME";
}

std::string_view ExclusionReasonName(ExclusionReason v) noexcept {
  switch (v) {
    case ExclusionReason::kPathAuthorityStale: return "path_authority_stale";
    case ExclusionReason::kTierNotAllowed: return "tier_not_allowed";
    case ExclusionReason::kForbiddenLabel: return "forbidden_label";
    case ExclusionReason::kMissingRequiredLabel: return "missing_required_label";
    case ExclusionReason::kForbiddenLocality: return "forbidden_locality";
    case ExclusionReason::kLocalityMismatch: return "locality_mismatch";
    case ExclusionReason::kForbiddenFailureDomain: return "forbidden_failure_domain";
    case ExclusionReason::kRequiredFailureDomainMismatch: return "required_failure_domain_mismatch";
    case ExclusionReason::kFailureDomainOccupied: return "failure_domain_occupied";
    case ExclusionReason::kCapacityUnknown: return "capacity_unknown";
    case ExclusionReason::kCapacityInsufficient: return "capacity_insufficient";
    case ExclusionReason::kServiceHeadroomUnmet: return "service_headroom_unmet";
    case ExclusionReason::kServiceReservationUnmet: return "service_reservation_unmet";
    case ExclusionReason::kReservationAffinityUnsatisfied: return "reservation_affinity_unsatisfied";
    case ExclusionReason::kLatencyBudgetExceeded: return "latency_budget_exceeded";
    case ExclusionReason::kEvidenceMissing: return "evidence_missing";
    case ExclusionReason::kEvidenceStale: return "evidence_stale";
    case ExclusionReason::kDuplicatePathEntry: return "duplicate_path_entry";
  }
  return "unknown_exclusion";
}

bool IsCapacityReason(ExclusionReason v) noexcept {
  switch (v) {
    case ExclusionReason::kCapacityUnknown:
    case ExclusionReason::kCapacityInsufficient:
      return true;
    default:
      return false;
  }
}

bool IsServiceReason(ExclusionReason v) noexcept {
  switch (v) {
    case ExclusionReason::kServiceHeadroomUnmet:
    case ExclusionReason::kServiceReservationUnmet:
      return true;
    default:
      return false;
  }
}

bool IsStalenessReason(ExclusionReason v) noexcept {
  switch (v) {
    case ExclusionReason::kPathAuthorityStale:
    case ExclusionReason::kEvidenceStale:
      return true;
    default:
      return false;
  }
}

bool IsPolicyReason(ExclusionReason v) noexcept {
  switch (v) {
    case ExclusionReason::kTierNotAllowed:
    case ExclusionReason::kForbiddenLabel:
    case ExclusionReason::kMissingRequiredLabel:
    case ExclusionReason::kForbiddenLocality:
    case ExclusionReason::kLocalityMismatch:
    case ExclusionReason::kForbiddenFailureDomain:
    case ExclusionReason::kRequiredFailureDomainMismatch:
    case ExclusionReason::kFailureDomainOccupied:
    case ExclusionReason::kReservationAffinityUnsatisfied:
    case ExclusionReason::kEvidenceMissing:
      return true;
    default:
      return false;
  }
}

bool IsQosReason(ExclusionReason v) noexcept {
  return v == ExclusionReason::kLatencyBudgetExceeded;
}

std::string_view DegradationFlagName(DegradationFlag v) noexcept {
  switch (v) {
    case DegradationFlag::kIncumbentSuperseded: return "incumbent_superseded";
    case DegradationFlag::kIncumbentStale: return "incumbent_stale";
    case DegradationFlag::kServiceRelaxed: return "service_relaxed";
    case DegradationFlag::kHeadroomBelowComfort: return "headroom_below_comfort";
    case DegradationFlag::kLocalityPreferenceUnmet: return "locality_preference_unmet";
    case DegradationFlag::kReservationPreferenceUnmet: return "reservation_preference_unmet";
    case DegradationFlag::kEvidenceUnknown: return "evidence_unknown";
    case DegradationFlag::kChurnSuppressed: return "churn_suppressed";
    case DegradationFlag::kNoIncumbent: return "no_incumbent";
  }
  return "unknown_degradation";
}

bool IsDegradingFlag(DegradationFlag v) noexcept {
  switch (v) {
    case DegradationFlag::kServiceRelaxed:
    case DegradationFlag::kHeadroomBelowComfort:
    case DegradationFlag::kLocalityPreferenceUnmet:
    case DegradationFlag::kReservationPreferenceUnmet:
    case DegradationFlag::kEvidenceUnknown:
      return true;
    case DegradationFlag::kIncumbentSuperseded:
    case DegradationFlag::kIncumbentStale:
    case DegradationFlag::kChurnSuppressed:
    case DegradationFlag::kNoIncumbent:
      return false;
  }
  return false;
}

std::string_view IncumbentDeltaName(IncumbentDelta v) noexcept {
  switch (v) {
    case IncumbentDelta::kNoIncumbent: return "no_incumbent";
    case IncumbentDelta::kKept: return "kept";
    case IncumbentDelta::kMoved: return "moved";
    case IncumbentDelta::kReplacedStale: return "replaced_stale";
  }
  return "unknown_delta";
}

bool operator==(const PlacementIntent& a, const PlacementIntent& b) noexcept {
  return a.flow == b.flow && a.flow_generation == b.flow_generation && a.path == b.path &&
         a.path_authority == b.path_authority && a.candidate_set == b.candidate_set &&
         a.candidate_set_generation == b.candidate_set_generation && a.capacity == b.capacity &&
         a.capacity_generation == b.capacity_generation && a.policy == b.policy &&
         a.policy_generation == b.policy_generation && a.qos == b.qos &&
         a.qos_generation == b.qos_generation && a.evidence == b.evidence &&
         a.evidence_generation == b.evidence_generation && a.fabric_epoch == b.fabric_epoch &&
         a.reservation == b.reservation && a.attempt == b.attempt &&
         a.revalidate_after_nanos == b.revalidate_after_nanos;
}

Digest IntentDigest(const PlacementIntent& intent) {
  CanonicalHasher h;
  h.AddTag("intent");
  h.AddU64(intent.flow.value());
  h.AddU64(intent.flow_generation.value());
  h.AddU64(intent.path.value());
  h.AddU64(intent.path_authority.value());
  h.AddU64(intent.candidate_set.value());
  h.AddU64(intent.candidate_set_generation.value());
  h.AddU64(intent.capacity.value());
  h.AddU64(intent.capacity_generation.value());
  h.AddU64(intent.policy.value());
  h.AddU64(intent.policy_generation.value());
  h.AddU64(intent.qos.value());
  h.AddU64(intent.qos_generation.value());
  h.AddU64(intent.evidence.value());
  h.AddU64(intent.evidence_generation.value());
  h.AddU64(intent.fabric_epoch.value());
  h.AddU64(intent.reservation.id.value());
  h.AddU64(intent.reservation.generation.value());
  h.AddU64(intent.attempt.value());
  h.AddU64(intent.revalidate_after_nanos);
  return Digest::OfCanonical(h);
}

Digest DecisionDigest(const PlacementDecision& decision) {
  CanonicalHasher h;
  h.AddTag("decision");
  h.AddU8(static_cast<std::uint8_t>(decision.outcome));
  h.AddU16(static_cast<std::uint16_t>(decision.code));
  h.AddBool(decision.intent.has_value());
  if (decision.intent) {
    const Digest intent = IntentDigest(*decision.intent);
    h.AddU64(intent.lo);
    h.AddU64(intent.hi);
  }
  const Digest authority = AuthorityVectorDigest(decision.authority);
  h.AddU64(authority.lo);
  h.AddU64(authority.hi);
  h.AddU8(static_cast<std::uint8_t>(decision.delta));
  h.AddTag("explanation");
  h.AddU64(decision.explanation.candidate_count);
  h.AddU64(decision.explanation.legal_candidate_count);
  h.AddU64(decision.explanation.excluded_candidate_count);
  h.AddU64(decision.explanation.unknown_evidence_count);
  h.AddU64(decision.explanation.distinct_failure_domains);
  h.AddBool(decision.explanation.truncated);
  h.AddU32(static_cast<std::uint32_t>(decision.explanation.ranked.size()));
  for (const RankedCandidate& ranked : decision.explanation.ranked) {
    h.AddU32(ranked.rank);
    h.AddU64(ranked.path.value());
    h.AddU64(ranked.authority.value());
    h.AddBool(ranked.is_incumbent);
    h.AddU32(static_cast<std::uint32_t>(ranked.keys.size()));
    for (const ObjectiveValue& key : ranked.keys) HashObjectiveValue(h, key);
  }
  h.AddU32(static_cast<std::uint32_t>(decision.explanation.exclusions.size()));
  for (const ExclusionSummary& summary : decision.explanation.exclusions) {
    h.AddU8(static_cast<std::uint8_t>(summary.reason));
    h.AddU64(summary.count);
    h.AddU32(static_cast<std::uint32_t>(summary.sample.size()));
    for (const PathId id : summary.sample) h.AddU64(id.value());
  }
  h.AddU32(static_cast<std::uint32_t>(decision.explanation.degradations.size()));
  for (const DegradationFlag flag : decision.explanation.degradations) {
    h.AddU8(static_cast<std::uint8_t>(flag));
  }
  h.AddU32(static_cast<std::uint32_t>(decision.explanation.binding.size()));
  for (const BindingConstraint& binding : decision.explanation.binding) {
    h.AddU8(static_cast<std::uint8_t>(binding.kind));
    h.AddU64(binding.path.value());
    h.AddU64(binding.required);
    h.AddU64(binding.available);
    h.AddString(binding.label);
  }
  h.AddU32(static_cast<std::uint32_t>(decision.explanation.notes.size()));
  for (const std::string& note : decision.explanation.notes) h.AddString(note);
  h.AddU16(static_cast<std::uint16_t>(decision.validation.code()));
  return Digest::OfCanonical(h);
}

std::string RenderDecision(const PlacementDecision& decision) {
  std::string out;
  out.reserve(1024);
  out += "outcome=";
  out += OutcomeName(decision.outcome);
  out += " code=";
  out += StatusCodeName(decision.code);
  out += " delta=";
  out += IncumbentDeltaName(decision.delta);
  out += "\n";
  out += "authority: " + decision.authority.ToString() + "\n";
  out += "digest: " + decision.digest.ToHex() + "\n";
  if (!decision.validation.ok()) {
    out += "validation: " + decision.validation.ToString() + "\n";
  }
  if (decision.intent) {
    const PlacementIntent& intent = *decision.intent;
    out += "intent: flow=" + intent.flow.ToString() + "/" + intent.flow_generation.ToString();
    out += " path=" + intent.path.ToString() + "/" + intent.path_authority.ToString();
    out += " reservation=" + intent.reservation.ToString();
    out += " revalidate_after_nanos=" + std::to_string(intent.revalidate_after_nanos);
    out += "\n";
    out += "intent_digest: " + intent.digest.ToHex() + "\n";
  }
  out += "candidates: total=" + std::to_string(decision.explanation.candidate_count) +
         " legal=" + std::to_string(decision.explanation.legal_candidate_count) +
         " excluded=" + std::to_string(decision.explanation.excluded_candidate_count) +
         " unknown_evidence=" + std::to_string(decision.explanation.unknown_evidence_count) +
         " distinct_failure_domains=" +
         std::to_string(decision.explanation.distinct_failure_domains) + "\n";
  if (!decision.explanation.ranked.empty()) {
    out += "ranking:\n";
    for (const RankedCandidate& ranked : decision.explanation.ranked) {
      out += "  [" + std::to_string(ranked.rank) + "] path=" + ranked.path.ToString() +
             " auth=" + ranked.authority.ToString();
      if (ranked.is_incumbent) out += " incumbent";
      for (const ObjectiveValue& key : ranked.keys) {
        out += " ";
        out += ObjectiveKindName(key.kind);
        out += "=";
        out += key.known ? std::to_string(key.value) : std::string("UNKNOWN");
      }
      out += "\n";
    }
  }
  if (!decision.explanation.exclusions.empty()) {
    out += "exclusions:\n";
    for (const ExclusionSummary& summary : decision.explanation.exclusions) {
      out += "  " + std::string(ExclusionReasonName(summary.reason)) + "=" +
             std::to_string(summary.count);
      if (!summary.sample.empty()) {
        out += " sample=";
        for (std::size_t i = 0; i < summary.sample.size(); ++i) {
          if (i != 0) out += ",";
          out += summary.sample[i].ToString();
        }
      }
      out += "\n";
    }
  }
  if (!decision.explanation.degradations.empty()) {
    out += "degradations:";
    for (const DegradationFlag flag : decision.explanation.degradations) {
      out += " ";
      out += DegradationFlagName(flag);
    }
    out += "\n";
  }
  if (!decision.explanation.binding.empty()) {
    out += "binding:\n";
    for (const BindingConstraint& binding : decision.explanation.binding) {
      out += "  " + std::string(ExclusionReasonName(binding.kind)) +
             " path=" + binding.path.ToString() +
             " required=" + std::to_string(binding.required) +
             " available=" + std::to_string(binding.available);
      if (!binding.label.empty()) out += " (" + binding.label + ")";
      out += "\n";
    }
  }
  if (decision.explanation.truncated) {
    out += "explanation_truncated: true\n";
  }
  for (const std::string& note : decision.explanation.notes) {
    out += "note: " + note + "\n";
  }
  return out;
}

}  // namespace flowplace

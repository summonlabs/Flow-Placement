// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/codec.hpp"

#include "flowplace/checked.hpp"

namespace flowplace {
namespace {

constexpr std::uint32_t kMaxNotesOnDecode = 64;
constexpr std::uint32_t kMaxMessageLength = 1024;

bool ReadEnumU8(ByteReader& r, std::uint8_t max_value, std::uint8_t* out) {
  std::uint8_t raw = 0;
  if (!r.U8(&raw)) return false;
  if (raw > max_value) return false;
  *out = raw;
  return true;
}

void WriteReservation(const ReservationRef& ref, ByteWriter& w) {
  w.U64(ref.id.value());
  w.U64(ref.generation.value());
}

bool ReadReservation(ByteReader& r, ReservationRef* out) {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  if (!r.U64(&id) || !r.U64(&generation)) return false;
  out->id = ReservationId{id};
  out->generation = ReservationGeneration{generation};
  return true;
}

void WriteIdList(const std::vector<PolicyLabelId>& ids, ByteWriter& w) {
  w.U32(static_cast<std::uint32_t>(ids.size()));
  for (const PolicyLabelId id : ids) w.U64(id.value());
}

bool ReadIdList(ByteReader& r, std::uint32_t max_count, std::vector<PolicyLabelId>* out) {
  std::uint32_t count = 0;
  if (!r.U32(&count)) return false;
  if (count > max_count) return false;
  out->clear();
  out->reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint64_t value = 0;
    if (!r.U64(&value)) return false;
    out->push_back(PolicyLabelId{value});
  }
  return true;
}

template <class IdT>
void WriteIdVector(const std::vector<IdT>& ids, ByteWriter& w) {
  w.U32(static_cast<std::uint32_t>(ids.size()));
  for (const IdT id : ids) w.U64(id.value());
}

template <class IdT>
bool ReadIdVector(ByteReader& r, std::uint32_t max_count, std::vector<IdT>* out) {
  std::uint32_t count = 0;
  if (!r.U32(&count)) return false;
  if (count > max_count) return false;
  out->clear();
  out->reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint64_t value = 0;
    if (!r.U64(&value)) return false;
    out->push_back(IdT{value});
  }
  return true;
}

void WriteOptionalId(std::uint64_t value, ByteWriter& w) {
  w.U8(value != 0 ? 1u : 0u);
  if (value != 0) w.U64(value);
}

bool ReadOptionalId(ByteReader& r, std::uint64_t* out) {
  std::uint8_t present = 0;
  if (!r.U8(&present)) return false;
  if (present > 1) return false;
  if (present == 0) {
    *out = 0;
    return true;
  }
  return r.U64(out);
}

void WriteExpected(const AuthorityExpectation& v, ByteWriter& w) {
  w.U64(v.path_authority.value());
  w.U64(v.candidate_set.value());
  w.U64(v.capacity.value());
  w.U64(v.policy.value());
  w.U64(v.qos.value());
  w.U64(v.fabric_epoch.value());
  w.U64(v.evidence.value());
}

bool ReadExpected(ByteReader& r, AuthorityExpectation* out) {
  std::uint64_t values[7] = {0, 0, 0, 0, 0, 0, 0};
  for (std::uint64_t& value : values) {
    if (!r.U64(&value)) return false;
  }
  out->path_authority = PathAuthorityGeneration{values[0]};
  out->candidate_set = CandidateSetGeneration{values[1]};
  out->capacity = CapacitySnapshotGeneration{values[2]};
  out->policy = PolicyGeneration{values[3]};
  out->qos = QosGeneration{values[4]};
  out->fabric_epoch = FabricEpoch{values[5]};
  out->evidence = EvidenceGeneration{values[6]};
  return true;
}

void WriteProvenance(const Provenance& v, ByteWriter& w) {
  w.String(v.producer);
  w.String(v.producer_version);
  w.U64(v.source_sequence);
  w.I64(v.observer_unix_nanos);
}

bool ReadProvenance(ByteReader& r, Provenance* out) {
  if (!r.String(&out->producer, static_cast<std::uint32_t>(kMaxProvenanceFieldLength))) return false;
  if (!r.String(&out->producer_version, static_cast<std::uint32_t>(kMaxProvenanceFieldLength))) {
    return false;
  }
  if (!r.U64(&out->source_sequence)) return false;
  return r.I64(&out->observer_unix_nanos);
}

void WriteAttributes(const PathAttributes& v, ByteWriter& w) {
  w.U8(static_cast<std::uint8_t>(v.tier));
  w.U64(v.locality.value());
  w.U8(static_cast<std::uint8_t>(v.locality_scope));
  w.U64(v.failure_domain.value());
  w.U64(v.cost_micro);
  w.U64(v.latency_nanos);
  w.U16(v.hop_count);
  WriteIdList(v.labels, w);
  w.U32(static_cast<std::uint32_t>(v.reservations.size()));
  for (const ReservationRef& ref : v.reservations) WriteReservation(ref, w);
}

void WritePolicy(const PlacementPolicy& v, ByteWriter& w) {
  w.U64(v.id.value());
  w.U64(v.generation.value());
  w.U32(static_cast<std::uint32_t>(v.objectives.size()));
  for (const Objective& objective : v.objectives) {
    w.U8(static_cast<std::uint8_t>(objective.kind));
    w.U8(static_cast<std::uint8_t>(objective.direction));
  }
  w.U32(static_cast<std::uint32_t>(v.allowed_tiers.size()));
  for (const PathTier tier : v.allowed_tiers) w.U8(static_cast<std::uint8_t>(tier));
  WriteIdList(v.required_labels, w);
  WriteIdList(v.forbidden_labels, w);
  WriteOptionalId(v.locality.required_locality ? v.locality.required_locality->value() : 0, w);
  WriteOptionalId(v.locality.preferred_locality ? v.locality.preferred_locality->value() : 0, w);
  WriteIdVector(v.locality.forbidden_localities, w);
  WriteOptionalId(
      v.failure_domains.required_domain ? v.failure_domains.required_domain->value() : 0, w);
  WriteIdVector(v.failure_domains.forbidden_domains, w);
  w.U8(v.failure_domains.avoid_occupied_domains ? 1u : 0u);
  w.U32(v.failure_domains.min_distinct_domains);
  w.U8(v.reservation_affinity.required ? 1u : 0u);
  w.U32(static_cast<std::uint32_t>(v.reservation_affinity.refs.size()));
  for (const ReservationRef& ref : v.reservation_affinity.refs) WriteReservation(ref, w);
  w.U64(v.admission.min_residual_headroom_bytes);
  w.U8(static_cast<std::uint8_t>(v.admission.on_below));
  w.U8(v.service.allow_degraded ? 1u : 0u);
  w.U8(v.service.max_relaxation_steps);
  w.U8(static_cast<std::uint8_t>(v.churn.on_move));
  w.U8(v.churn.prefer_incumbent ? 1u : 0u);
  w.U64(v.churn.move_improvement_threshold);
  w.U32(v.churn.threshold_objective_index);
  w.U8(static_cast<std::uint8_t>(v.evidence.on_missing));
  w.U8(static_cast<std::uint8_t>(v.evidence.on_stale));
  w.U64(v.revalidate_after_nanos);
}

bool ReadBool(ByteReader& r, bool* out) {
  std::uint8_t raw = 0;
  if (!r.U8(&raw)) return false;
  if (raw > 1) return false;
  *out = raw != 0;
  return true;
}

bool ReadPolicy(ByteReader& r, const Limits& limits, PlacementPolicy* out) {
  std::uint64_t policy_id = 0;
  if (!r.U64(&policy_id)) return false;
  out->id = PolicyId{policy_id};
  std::uint64_t generation = 0;
  if (!r.U64(&generation)) return false;
  out->generation = PolicyGeneration{generation};
  std::uint32_t objective_count = 0;
  if (!r.U32(&objective_count)) return false;
  if (objective_count == 0 || objective_count > limits.max_objectives) return false;
  out->objectives.clear();
  for (std::uint32_t i = 0; i < objective_count; ++i) {
    Objective objective;
    std::uint8_t kind = 0;
    std::uint8_t direction = 0;
    if (!ReadEnumU8(r, 8, &kind) || !ReadEnumU8(r, 1, &direction)) return false;
    objective.kind = static_cast<ObjectiveKind>(kind);
    objective.direction = static_cast<Direction>(direction);
    out->objectives.push_back(objective);
  }
  std::uint32_t tier_count = 0;
  if (!r.U32(&tier_count)) return false;
  if (tier_count > 3) return false;
  out->allowed_tiers.clear();
  for (std::uint32_t i = 0; i < tier_count; ++i) {
    std::uint8_t tier = 0;
    if (!ReadEnumU8(r, 2, &tier)) return false;
    out->allowed_tiers.push_back(static_cast<PathTier>(tier));
  }
  const auto max_ids = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(limits.max_policy_id_lists, 0xFFFFFFFFull));
  if (!ReadIdList(r, max_ids, &out->required_labels)) return false;
  if (!ReadIdList(r, max_ids, &out->forbidden_labels)) return false;
  std::uint64_t required_locality = 0;
  std::uint64_t preferred_locality = 0;
  if (!ReadOptionalId(r, &required_locality) || !ReadOptionalId(r, &preferred_locality)) return false;
  out->locality.required_locality =
      required_locality != 0 ? std::optional<LocalityId>(LocalityId{required_locality}) : std::nullopt;
  out->locality.preferred_locality = preferred_locality != 0
                                         ? std::optional<LocalityId>(LocalityId{preferred_locality})
                                         : std::nullopt;
  if (!ReadIdVector(r, max_ids, &out->locality.forbidden_localities)) return false;
  std::uint64_t required_domain = 0;
  if (!ReadOptionalId(r, &required_domain)) return false;
  out->failure_domains.required_domain =
      required_domain != 0 ? std::optional<FailureDomainId>(FailureDomainId{required_domain})
                           : std::nullopt;
  if (!ReadIdVector(r, max_ids, &out->failure_domains.forbidden_domains)) return false;
  if (!ReadBool(r, &out->failure_domains.avoid_occupied_domains)) return false;
  if (!r.U32(&out->failure_domains.min_distinct_domains)) return false;
  if (!ReadBool(r, &out->reservation_affinity.required)) return false;
  std::uint32_t ref_count = 0;
  if (!r.U32(&ref_count)) return false;
  if (ref_count > limits.max_affinity_refs) return false;
  out->reservation_affinity.refs.clear();
  for (std::uint32_t i = 0; i < ref_count; ++i) {
    ReservationRef ref;
    if (!ReadReservation(r, &ref)) return false;
    out->reservation_affinity.refs.push_back(ref);
  }
  if (!r.U64(&out->admission.min_residual_headroom_bytes)) return false;
  std::uint8_t gate = 0;
  if (!ReadEnumU8(r, 1, &gate)) return false;
  out->admission.on_below = static_cast<GateAction>(gate);
  if (!ReadBool(r, &out->service.allow_degraded)) return false;
  if (!r.U8(&out->service.max_relaxation_steps)) return false;
  if (out->service.max_relaxation_steps > 2) return false;
  std::uint8_t churn = 0;
  if (!ReadEnumU8(r, 1, &churn)) return false;
  out->churn.on_move = static_cast<ChurnAction>(churn);
  if (!ReadBool(r, &out->churn.prefer_incumbent)) return false;
  if (!r.U64(&out->churn.move_improvement_threshold)) return false;
  if (!r.U32(&out->churn.threshold_objective_index)) return false;
  std::uint8_t on_missing = 0;
  std::uint8_t on_stale = 0;
  if (!ReadEnumU8(r, 1, &on_missing) || !ReadEnumU8(r, 1, &on_stale)) return false;
  out->evidence.on_missing = static_cast<EvidencePolicyMode>(on_missing);
  out->evidence.on_stale = static_cast<EvidencePolicyMode>(on_stale);
  return r.U64(&out->revalidate_after_nanos);
}

void WriteIncumbent(const IncumbentPlacement& v, ByteWriter& w) {
  w.U64(v.id.value());
  w.U64(v.generation.value());
  w.U64(v.path.value());
  w.U64(v.path_authority.value());
  w.U64(v.candidate_set_generation.value());
  w.U64(v.capacity_generation.value());
  w.U64(v.policy_generation.value());
  w.U64(v.qos_generation.value());
  w.U64(v.fabric_epoch.value());
  WriteReservation(v.reservation, w);
}

bool ReadIncumbent(ByteReader& r, IncumbentPlacement* out) {
  std::uint64_t values[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  for (std::uint64_t& value : values) {
    if (!r.U64(&value)) return false;
  }
  out->id = PlacementId{values[0]};
  out->generation = PlacementGeneration{values[1]};
  out->path = PathId{values[2]};
  out->path_authority = PathAuthorityGeneration{values[3]};
  out->candidate_set_generation = CandidateSetGeneration{values[4]};
  out->capacity_generation = CapacitySnapshotGeneration{values[5]};
  out->policy_generation = PolicyGeneration{values[6]};
  out->qos_generation = QosGeneration{values[7]};
  out->fabric_epoch = FabricEpoch{values[8]};
  return ReadReservation(r, &out->reservation);
}

void WriteIntent(const PlacementIntent& v, ByteWriter& w) {
  w.U64(v.flow.value());
  w.U64(v.flow_generation.value());
  w.U64(v.path.value());
  w.U64(v.path_authority.value());
  w.U64(v.candidate_set.value());
  w.U64(v.candidate_set_generation.value());
  w.U64(v.capacity.value());
  w.U64(v.capacity_generation.value());
  w.U64(v.policy.value());
  w.U64(v.policy_generation.value());
  w.U64(v.qos.value());
  w.U64(v.qos_generation.value());
  w.U64(v.evidence.value());
  w.U64(v.evidence_generation.value());
  w.U64(v.fabric_epoch.value());
  WriteReservation(v.reservation, w);
  w.U64(v.attempt.value());
  w.U64(v.revalidate_after_nanos);
  w.U64(v.digest.hi);
  w.U64(v.digest.lo);
}

bool ReadIntent(ByteReader& r, PlacementIntent* out) {
  std::uint64_t values[15] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  for (std::uint64_t& value : values) {
    if (!r.U64(&value)) return false;
  }
  out->flow = FlowId{values[0]};
  out->flow_generation = FlowGeneration{values[1]};
  out->path = PathId{values[2]};
  out->path_authority = PathAuthorityGeneration{values[3]};
  out->candidate_set = CandidateSetId{values[4]};
  out->candidate_set_generation = CandidateSetGeneration{values[5]};
  out->capacity = CapacitySnapshotId{values[6]};
  out->capacity_generation = CapacitySnapshotGeneration{values[7]};
  out->policy = PolicyId{values[8]};
  out->policy_generation = PolicyGeneration{values[9]};
  out->qos = QosProfileId{values[10]};
  out->qos_generation = QosGeneration{values[11]};
  out->evidence = EvidenceId{values[12]};
  out->evidence_generation = EvidenceGeneration{values[13]};
  out->fabric_epoch = FabricEpoch{values[14]};
  if (!ReadReservation(r, &out->reservation)) return false;
  std::uint64_t attempt = 0;
  if (!r.U64(&attempt)) return false;
  out->attempt = AttemptId{attempt};
  if (!r.U64(&out->revalidate_after_nanos)) return false;
  return r.U64(&out->digest.hi) && r.U64(&out->digest.lo);
}

void WriteAuthorityVector(const AuthorityVector& v, ByteWriter& w) {
  w.U64(v.path_authority.value());
  w.U64(v.candidate_set.value());
  w.U64(v.candidate_set_generation.value());
  w.U64(v.capacity.value());
  w.U64(v.capacity_generation.value());
  w.U64(v.policy.value());
  w.U64(v.policy_generation.value());
  w.U64(v.qos.value());
  w.U64(v.qos_generation.value());
  w.U64(v.evidence.value());
  w.U64(v.evidence_generation.value());
  w.U64(v.fabric_epoch.value());
}

bool ReadAuthorityVector(ByteReader& r, AuthorityVector* out) {
  std::uint64_t values[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  for (std::uint64_t& value : values) {
    if (!r.U64(&value)) return false;
  }
  out->path_authority = PathAuthorityGeneration{values[0]};
  out->candidate_set = CandidateSetId{values[1]};
  out->candidate_set_generation = CandidateSetGeneration{values[2]};
  out->capacity = CapacitySnapshotId{values[3]};
  out->capacity_generation = CapacitySnapshotGeneration{values[4]};
  out->policy = PolicyId{values[5]};
  out->policy_generation = PolicyGeneration{values[6]};
  out->qos = QosProfileId{values[7]};
  out->qos_generation = QosGeneration{values[8]};
  out->evidence = EvidenceId{values[9]};
  out->evidence_generation = EvidenceGeneration{values[10]};
  out->fabric_epoch = FabricEpoch{values[11]};
  return true;
}

void WriteExplanation(const Explanation& v, ByteWriter& w) {
  w.U8(v.truncated ? 1u : 0u);
  w.U64(v.candidate_count);
  w.U64(v.legal_candidate_count);
  w.U64(v.excluded_candidate_count);
  w.U64(v.unknown_evidence_count);
  w.U64(v.distinct_failure_domains);
  w.U32(static_cast<std::uint32_t>(v.ranked.size()));
  for (const RankedCandidate& ranked : v.ranked) {
    w.U64(ranked.path.value());
    w.U64(ranked.authority.value());
    w.U32(ranked.rank);
    w.U8(ranked.is_incumbent ? 1u : 0u);
    w.U32(static_cast<std::uint32_t>(ranked.keys.size()));
    for (const ObjectiveValue& key : ranked.keys) {
      w.U8(static_cast<std::uint8_t>(key.kind));
      w.U8(static_cast<std::uint8_t>(key.direction));
      w.U8(key.known ? 1u : 0u);
      w.U64(key.value);
    }
  }
  w.U32(static_cast<std::uint32_t>(v.exclusions.size()));
  for (const ExclusionSummary& summary : v.exclusions) {
    w.U8(static_cast<std::uint8_t>(summary.reason));
    w.U64(summary.count);
    w.U32(static_cast<std::uint32_t>(summary.sample.size()));
    for (const PathId id : summary.sample) w.U64(id.value());
  }
  w.U32(static_cast<std::uint32_t>(v.degradations.size()));
  for (const DegradationFlag flag : v.degradations) w.U8(static_cast<std::uint8_t>(flag));
  w.U32(static_cast<std::uint32_t>(v.binding.size()));
  for (const BindingConstraint& binding : v.binding) {
    w.U8(static_cast<std::uint8_t>(binding.kind));
    w.U64(binding.path.value());
    w.U64(binding.required);
    w.U64(binding.available);
    w.String(binding.label);
  }
  w.U32(static_cast<std::uint32_t>(v.notes.size()));
  for (const std::string& note : v.notes) w.String(note);
}

bool ReadExplanation(ByteReader& r, const Limits& limits, Explanation* out) {
  if (!ReadBool(r, &out->truncated)) return false;
  if (!r.U64(&out->candidate_count) || !r.U64(&out->legal_candidate_count) ||
      !r.U64(&out->excluded_candidate_count) || !r.U64(&out->unknown_evidence_count) ||
      !r.U64(&out->distinct_failure_domains)) {
    return false;
  }
  std::uint32_t ranked_count = 0;
  if (!r.U32(&ranked_count)) return false;
  if (ranked_count > limits.max_ranked_explained) return false;
  out->ranked.clear();
  for (std::uint32_t i = 0; i < ranked_count; ++i) {
    RankedCandidate ranked;
    std::uint64_t path_id = 0;
    std::uint64_t authority = 0;
    if (!r.U64(&path_id) || !r.U64(&authority)) return false;
    ranked.path = PathId{path_id};
    ranked.authority = PathAuthorityGeneration{authority};
    if (!r.U32(&ranked.rank)) return false;
    if (!ReadBool(r, &ranked.is_incumbent)) return false;
    std::uint32_t key_count = 0;
    if (!r.U32(&key_count)) return false;
    if (key_count > limits.max_objectives) return false;
    for (std::uint32_t k = 0; k < key_count; ++k) {
      ObjectiveValue key;
      std::uint8_t kind = 0;
      std::uint8_t direction = 0;
      if (!ReadEnumU8(r, 8, &kind) || !ReadEnumU8(r, 1, &direction)) return false;
      key.kind = static_cast<ObjectiveKind>(kind);
      key.direction = static_cast<Direction>(direction);
      if (!ReadBool(r, &key.known)) return false;
      if (!r.U64(&key.value)) return false;
      ranked.keys.push_back(key);
    }
    out->ranked.push_back(std::move(ranked));
  }
  std::uint32_t exclusion_count = 0;
  if (!r.U32(&exclusion_count)) return false;
  if (exclusion_count > limits.max_exclusion_reasons) return false;
  out->exclusions.clear();
  for (std::uint32_t i = 0; i < exclusion_count; ++i) {
    ExclusionSummary summary;
    std::uint8_t reason = 0;
    if (!ReadEnumU8(r, 17, &reason)) return false;
    summary.reason = static_cast<ExclusionReason>(reason);
    if (!r.U64(&summary.count)) return false;
    std::uint32_t sample_count = 0;
    if (!r.U32(&sample_count)) return false;
    if (sample_count > limits.max_exclusion_samples) return false;
    for (std::uint32_t s = 0; s < sample_count; ++s) {
      std::uint64_t path = 0;
      if (!r.U64(&path)) return false;
      summary.sample.push_back(PathId{path});
    }
    out->exclusions.push_back(std::move(summary));
  }
  std::uint32_t degradation_count = 0;
  if (!r.U32(&degradation_count)) return false;
  if (degradation_count > 9) return false;
  out->degradations.clear();
  for (std::uint32_t i = 0; i < degradation_count; ++i) {
    std::uint8_t flag = 0;
    if (!ReadEnumU8(r, 8, &flag)) return false;
    out->degradations.push_back(static_cast<DegradationFlag>(flag));
  }
  std::uint32_t binding_count = 0;
  if (!r.U32(&binding_count)) return false;
  if (binding_count > 32) return false;
  out->binding.clear();
  for (std::uint32_t i = 0; i < binding_count; ++i) {
    BindingConstraint binding;
    std::uint8_t kind = 0;
    if (!ReadEnumU8(r, 17, &kind)) return false;
    binding.kind = static_cast<ExclusionReason>(kind);
    std::uint64_t path = 0;
    if (!r.U64(&path)) return false;
    binding.path = PathId{path};
    if (!r.U64(&binding.required) || !r.U64(&binding.available)) return false;
    if (!r.String(&binding.label,
                  static_cast<std::uint32_t>(
                      std::min<std::uint64_t>(limits.max_string_length, 0xFFFFFFFFull)))) {
      return false;
    }
    out->binding.push_back(std::move(binding));
  }
  std::uint32_t note_count = 0;
  if (!r.U32(&note_count)) return false;
  if (note_count > kMaxNotesOnDecode) return false;
  out->notes.clear();
  for (std::uint32_t i = 0; i < note_count; ++i) {
    std::string note;
    if (!r.String(&note, static_cast<std::uint32_t>(limits.max_string_length))) return false;
    out->notes.push_back(std::move(note));
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public codecs
// ---------------------------------------------------------------------------

void EncodeAuthorityExpectation(const AuthorityExpectation& v, ByteWriter& w) {
  WriteExpected(v, w);
}

Status DecodeAuthorityExpectation(ByteReader& r, AuthorityExpectation* out) {
  AuthorityExpectation value;
  if (!ReadExpected(r, &value)) return Status(StatusCode::kWireMalformed, "truncated expectation");
  if (!r.AtEnd()) {
    return Status(StatusCode::kWireMalformed, "trailing bytes after the expectation");
  }
  *out = value;
  return Status::Ok();
}

void EncodeAuthorityVector(const AuthorityVector& v, ByteWriter& w) {
  WriteAuthorityVector(v, w);
}

Status DecodeAuthorityVector(ByteReader& r, AuthorityVector* out) {
  AuthorityVector value;
  if (!ReadAuthorityVector(r, &value)) {
    return Status(StatusCode::kWireMalformed, "truncated authority vector");
  }
  if (!r.AtEnd()) {
    return Status(StatusCode::kWireMalformed, "trailing bytes after the authority vector");
  }
  *out = value;
  return Status::Ok();
}

void EncodeRequest(const PlacementRequest& v, ByteWriter& w) {
  w.U64(v.flow.value());
  w.U64(v.flow_generation.value());
  w.U64(v.candidates.id.value());
  w.U64(v.candidates.generation.value());
  w.U32(static_cast<std::uint32_t>(v.candidates.paths.size()));
  for (const CandidatePath& path : v.candidates.paths) {
    w.U64(path.id.value());
    w.U64(path.authority.value());
    WriteAttributes(path.attributes, w);
  }
  w.U64(v.capacity.id.value());
  w.U64(v.capacity.generation.value());
  w.U32(static_cast<std::uint32_t>(v.capacity.entries.size()));
  for (const PathCapacity& entry : v.capacity.entries) {
    w.U64(entry.path.value());
    w.U64(entry.capacity_bytes);
    w.U64(entry.residual_bytes);
  }
  w.U64(v.qos.id.value());
  w.U64(v.qos.generation.value());
  w.U8(static_cast<std::uint8_t>(v.qos.service_class));
  w.U8(static_cast<std::uint8_t>(v.qos.priority));
  w.U64(v.qos.required_residual_bytes);
  w.U64(v.qos.max_latency_nanos);
  w.U64(v.evidence.id.value());
  w.U64(v.evidence.generation.value());
  w.U32(static_cast<std::uint32_t>(v.evidence.entries.size()));
  for (const CongestionEvidenceEntry& entry : v.evidence.entries) {
    w.U64(entry.path.value());
    w.U32(entry.utilization_ppb);
  }
  WritePolicy(v.policy, w);
  w.U8(v.incumbent.has_value() ? 1u : 0u);
  if (v.incumbent) WriteIncumbent(*v.incumbent, w);
  WriteIdVector(v.occupied_failure_domains, w);
  WriteExpected(v.expected, w);
  WriteProvenance(v.provenance, w);
  w.U64(v.attempt.value());
}

Status DecodeRequest(ByteReader& r, const Limits& limits, PlacementRequest* out) {
  PlacementRequest value;
  std::uint64_t raw = 0;
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated request header");
  value.flow = FlowId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated flow generation");
  value.flow_generation = FlowGeneration{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated candidate set id");
  value.candidates.id = CandidateSetId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated candidate set generation");
  value.candidates.generation = CandidateSetGeneration{raw};
  std::uint32_t path_count = 0;
  if (!r.U32(&path_count)) return Status(StatusCode::kWireMalformed, "truncated path count");
  if (path_count > limits.max_candidate_paths) {
    return Status(StatusCode::kOversizeRequest, "encoded candidate path count exceeds the limit");
  }
  value.candidates.paths.clear();
  value.candidates.paths.reserve(path_count);
  for (std::uint32_t i = 0; i < path_count; ++i) {
    CandidatePath path;
    if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated path id");
    path.id = PathId{raw};
    if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated path authority");
    path.authority = PathAuthorityGeneration{raw};
    std::uint8_t tier = 0;
    std::uint8_t scope = 0;
    if (!ReadEnumU8(r, 2, &tier) || !r.U64(&raw)) {
      return Status(StatusCode::kWireMalformed, "malformed path attributes");
    }
    path.attributes.tier = static_cast<PathTier>(tier);
    path.attributes.locality = LocalityId{raw};
    if (!ReadEnumU8(r, 4, &scope)) return Status(StatusCode::kWireMalformed, "malformed locality scope");
    path.attributes.locality_scope = static_cast<LocalityScope>(scope);
    if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated failure domain");
    path.attributes.failure_domain = FailureDomainId{raw};
    if (!r.U64(&path.attributes.cost_micro) || !r.U64(&path.attributes.latency_nanos) ||
        !r.U16(&path.attributes.hop_count)) {
      return Status(StatusCode::kWireMalformed, "truncated path metrics");
    }
    if (!ReadIdList(r,
                    static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(limits.max_path_labels, 0xFFFFFFFFull)),
                    &path.attributes.labels)) {
      return Status(StatusCode::kOversizeRequest, "encoded path label count exceeds the limit");
    }
    std::uint32_t reservation_count = 0;
    if (!r.U32(&reservation_count)) return Status(StatusCode::kWireMalformed, "truncated reservation count");
    if (reservation_count > limits.max_path_reservations) {
      return Status(StatusCode::kOversizeRequest, "encoded reservation count exceeds the limit");
    }
    for (std::uint32_t k = 0; k < reservation_count; ++k) {
      ReservationRef ref;
      if (!ReadReservation(r, &ref)) return Status(StatusCode::kWireMalformed, "truncated reservation");
      path.attributes.reservations.push_back(ref);
    }
    value.candidates.paths.push_back(std::move(path));
  }

  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated capacity id");
  value.capacity.id = CapacitySnapshotId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated capacity generation");
  value.capacity.generation = CapacitySnapshotGeneration{raw};
  std::uint32_t capacity_count = 0;
  if (!r.U32(&capacity_count)) return Status(StatusCode::kWireMalformed, "truncated capacity count");
  if (capacity_count > limits.max_capacity_entries) {
    return Status(StatusCode::kOversizeRequest, "encoded capacity entry count exceeds the limit");
  }
  value.capacity.entries.clear();
  value.capacity.entries.reserve(capacity_count);
  for (std::uint32_t i = 0; i < capacity_count; ++i) {
    PathCapacity entry;
    if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated capacity path");
    entry.path = PathId{raw};
    if (!r.U64(&entry.capacity_bytes) || !r.U64(&entry.residual_bytes)) {
      return Status(StatusCode::kWireMalformed, "truncated capacity entry");
    }
    value.capacity.entries.push_back(entry);
  }

  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated qos id");
  value.qos.id = QosProfileId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated qos generation");
  value.qos.generation = QosGeneration{raw};
  std::uint8_t service_class = 0;
  std::uint8_t priority = 0;
  if (!ReadEnumU8(r, 3, &service_class) || !ReadEnumU8(r, 3, &priority)) {
    return Status(StatusCode::kWireMalformed, "malformed qos class");
  }
  value.qos.service_class = static_cast<ServiceClass>(service_class);
  value.qos.priority = static_cast<PriorityClass>(priority);
  if (!r.U64(&value.qos.required_residual_bytes) || !r.U64(&value.qos.max_latency_nanos)) {
    return Status(StatusCode::kWireMalformed, "truncated qos requirement");
  }

  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated evidence id");
  value.evidence.id = EvidenceId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated evidence generation");
  value.evidence.generation = EvidenceGeneration{raw};
  std::uint32_t evidence_count = 0;
  if (!r.U32(&evidence_count)) return Status(StatusCode::kWireMalformed, "truncated evidence count");
  if (evidence_count > limits.max_evidence_entries) {
    return Status(StatusCode::kOversizeRequest, "encoded evidence entry count exceeds the limit");
  }
  value.evidence.entries.clear();
  value.evidence.entries.reserve(evidence_count);
  for (std::uint32_t i = 0; i < evidence_count; ++i) {
    CongestionEvidenceEntry entry;
    if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated evidence path");
    entry.path = PathId{raw};
    if (!r.U32(&entry.utilization_ppb)) {
      return Status(StatusCode::kWireMalformed, "truncated evidence utilization");
    }
    if (entry.utilization_ppb > 1000000000u) {
      return Status(StatusCode::kInvalidFieldValue, "evidence utilization above 1e9 ppb");
    }
    value.evidence.entries.push_back(entry);
  }

  if (!ReadPolicy(r, limits, &value.policy)) {
    return Status(StatusCode::kWireMalformed, "malformed policy");
  }

  bool has_incumbent = false;
  if (!ReadBool(r, &has_incumbent)) return Status(StatusCode::kWireMalformed, "truncated incumbent flag");
  if (has_incumbent) {
    IncumbentPlacement incumbent;
    if (!ReadIncumbent(r, &incumbent)) {
      return Status(StatusCode::kWireMalformed, "truncated incumbent");
    }
    value.incumbent = incumbent;
  }

  if (!ReadIdVector(r, static_cast<std::uint32_t>(
                             std::min<std::uint64_t>(limits.max_occupied_failure_domains,
                                                     0xFFFFFFFFull)),
                    &value.occupied_failure_domains)) {
    return Status(StatusCode::kOversizeRequest, "encoded occupied domain count exceeds the limit");
  }

  if (!ReadExpected(r, &value.expected)) {
    return Status(StatusCode::kWireMalformed, "truncated expectation");
  }
  if (!ReadProvenance(r, &value.provenance)) {
    return Status(StatusCode::kWireMalformed, "truncated provenance");
  }
  if (!r.U64(&raw)) return Status(StatusCode::kWireMalformed, "truncated attempt id");
  value.attempt = AttemptId{raw};
  if (!r.AtEnd()) return Status(StatusCode::kWireMalformed, "trailing bytes after request payload");
  *out = std::move(value);
  return Status::Ok();
}

void EncodeIntent(const PlacementIntent& v, ByteWriter& w) { WriteIntent(v, w); }

Status DecodeIntent(ByteReader& r, PlacementIntent* out) {
  PlacementIntent value;
  if (!ReadIntent(r, &value)) return Status(StatusCode::kWireMalformed, "truncated intent");
  if (!r.AtEnd()) return Status(StatusCode::kWireMalformed, "trailing bytes after the intent");
  *out = value;
  return Status::Ok();
}

void EncodeDecision(const PlacementDecision& v, ByteWriter& w) {
  w.U8(static_cast<std::uint8_t>(v.outcome));
  w.U16(static_cast<std::uint16_t>(v.code));
  w.U8(v.intent.has_value() ? 1u : 0u);
  if (v.intent) WriteIntent(*v.intent, w);
  WriteAuthorityVector(v.authority, w);
  w.U8(static_cast<std::uint8_t>(v.delta));
  WriteExplanation(v.explanation, w);
  w.U64(v.digest.hi);
  w.U64(v.digest.lo);
  w.U16(static_cast<std::uint16_t>(v.validation.code()));
  w.String(v.validation.message());
}

Status DecodeDecision(ByteReader& r, const Limits& limits, PlacementDecision* out) {
  PlacementDecision value;
  std::uint8_t outcome = 0;
  if (!ReadEnumU8(r, 7, &outcome)) return Status(StatusCode::kWireMalformed, "malformed outcome");
  value.outcome = static_cast<Outcome>(outcome);
  std::uint16_t code = 0;
  if (!r.U16(&code)) return Status(StatusCode::kWireMalformed, "truncated decision code");
  if (!IsKnownStatusCode(code)) {
    return Status(StatusCode::kWireMalformed, "unknown decision status code");
  }
  value.code = static_cast<StatusCode>(code);
  bool has_intent = false;
  if (!ReadBool(r, &has_intent)) return Status(StatusCode::kWireMalformed, "truncated intent flag");
  if (has_intent) {
    PlacementIntent intent;
    if (!ReadIntent(r, &intent)) return Status(StatusCode::kWireMalformed, "truncated intent");
    value.intent = intent;
  }
  if (!ReadAuthorityVector(r, &value.authority)) {
    return Status(StatusCode::kWireMalformed, "truncated authority vector");
  }
  std::uint8_t delta = 0;
  if (!ReadEnumU8(r, 3, &delta)) return Status(StatusCode::kWireMalformed, "malformed delta");
  value.delta = static_cast<IncumbentDelta>(delta);
  if (!ReadExplanation(r, limits, &value.explanation)) {
    return Status(StatusCode::kWireMalformed, "malformed explanation");
  }
  if (!r.U64(&value.digest.hi) || !r.U64(&value.digest.lo)) {
    return Status(StatusCode::kWireMalformed, "truncated decision digest");
  }
  if (!r.U16(&code)) return Status(StatusCode::kWireMalformed, "truncated validation code");
  if (!IsKnownStatusCode(code)) {
    return Status(StatusCode::kWireMalformed, "unknown validation status code");
  }
  std::string message;
  if (!r.String(&message, kMaxMessageLength)) {
    return Status(StatusCode::kWireMalformed, "truncated validation message");
  }
  value.validation = Status(static_cast<StatusCode>(code), std::move(message));
  if (!r.AtEnd()) return Status(StatusCode::kWireMalformed, "trailing bytes after decision payload");
  *out = std::move(value);
  return Status::Ok();
}

void EncodeAttemptRecord(const AttemptRecord& v, ByteWriter& w) {
  w.U64(v.attempt.value());
  w.U64(v.incarnation.value());
  w.U64(v.epoch.value());
  w.U64(v.flow.value());
  w.U8(static_cast<std::uint8_t>(v.phase));
  w.U16(static_cast<std::uint16_t>(v.code));
  w.U64(v.request_digest.hi);
  w.U64(v.request_digest.lo);
  w.U64(v.sequence);
  w.I64(v.observed_unix_nanos);
}

Status DecodeAttemptRecord(ByteReader& r, AttemptRecord* out) {
  AttemptRecord value;
  std::uint64_t raw = 0;
  if (!r.U64(&raw)) return Status(StatusCode::kStoreCorrupt, "truncated attempt record");
  value.attempt = AttemptId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kStoreCorrupt, "truncated attempt incarnation");
  value.incarnation = IncarnationId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kStoreCorrupt, "truncated attempt epoch");
  value.epoch = FabricEpoch{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kStoreCorrupt, "truncated attempt flow");
  value.flow = FlowId{raw};
  std::uint8_t phase = 0;
  if (!ReadEnumU8(r, 4, &phase)) return Status(StatusCode::kStoreCorrupt, "malformed attempt phase");
  value.phase = static_cast<AttemptPhase>(phase);
  std::uint16_t code = 0;
  if (!r.U16(&code)) return Status(StatusCode::kStoreCorrupt, "truncated attempt code");
  if (!IsKnownStatusCode(code)) {
    return Status(StatusCode::kStoreCorrupt, "unknown attempt status code");
  }
  value.code = static_cast<StatusCode>(code);
  if (!r.U64(&value.request_digest.hi) || !r.U64(&value.request_digest.lo)) {
    return Status(StatusCode::kStoreCorrupt, "truncated attempt digest");
  }
  if (!r.U64(&value.sequence) || !r.I64(&value.observed_unix_nanos)) {
    return Status(StatusCode::kStoreCorrupt, "truncated attempt trailer");
  }
  // Nested decoders consume exactly one record and leave the reader positioned
  // for the next one; the store checks for trailing bytes at its own boundary.
  *out = value;
  return Status::Ok();
}

void EncodePlacementRecord(const PlacementRecord& v, ByteWriter& w) {
  w.U64(v.id.value());
  w.U64(v.generation.value());
  WriteIntent(v.intent, w);
  w.U8(static_cast<std::uint8_t>(v.delta));
  w.U64(v.decision_digest.hi);
  w.U64(v.decision_digest.lo);
  w.U64(v.committed_by.value());
  w.U64(v.commit_epoch.value());
  w.U64(v.sequence);
  w.U64(v.supersedes_generation);
  w.I64(v.commit_unix_nanos);
}

Status DecodePlacementRecord(ByteReader& r, PlacementRecord* out) {
  PlacementRecord value;
  std::uint64_t raw = 0;
  if (!r.U64(&raw)) return Status(StatusCode::kStoreCorrupt, "truncated placement record");
  value.id = PlacementId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kStoreCorrupt, "truncated placement generation");
  value.generation = PlacementGeneration{raw};
  if (!ReadIntent(r, &value.intent)) {
    return Status(StatusCode::kStoreCorrupt, "truncated placement intent");
  }
  std::uint8_t delta = 0;
  if (!ReadEnumU8(r, 3, &delta)) return Status(StatusCode::kStoreCorrupt, "malformed placement delta");
  value.delta = static_cast<IncumbentDelta>(delta);
  if (!r.U64(&value.decision_digest.hi) || !r.U64(&value.decision_digest.lo)) {
    return Status(StatusCode::kStoreCorrupt, "truncated placement digest");
  }
  if (!r.U64(&raw)) return Status(StatusCode::kStoreCorrupt, "truncated placement incarnation");
  value.committed_by = IncarnationId{raw};
  if (!r.U64(&raw)) return Status(StatusCode::kStoreCorrupt, "truncated placement epoch");
  value.commit_epoch = FabricEpoch{raw};
  if (!r.U64(&value.sequence) || !r.U64(&value.supersedes_generation) ||
      !r.I64(&value.commit_unix_nanos)) {
    return Status(StatusCode::kStoreCorrupt, "truncated placement trailer");
  }
  value.requires_revalidation = true;
  *out = value;
  return Status::Ok();
}

void EncodeRecoveryReport(const RecoveryReport& v, ByteWriter& w) {
  w.U8(v.liveness_restored ? 1u : 0u);
  w.U8(v.truncated_tail ? 1u : 0u);
  w.U8(v.snapshot_loaded ? 1u : 0u);
  w.U64(v.bytes_discarded);
  w.U64(v.records_valid);
  w.U64(v.placements_total);
  w.U64(v.attempts_total);
  w.U64(v.committed_attempts);
  w.U64(v.rejected_attempts);
  w.U64(v.cancelled_attempts);
  w.U64(v.fenced_attempts);
  w.U64(v.unfinished_attempts);
  w.U64(v.superseded_placements);
  w.U64(v.placements_requiring_revalidation);
  w.U64(v.retired_attempts);
  w.U64(v.max_sequence);
  w.U64(v.last_epoch.value());
  w.U64(v.last_incarnation.value());
  w.U32(static_cast<std::uint32_t>(v.latest_by_flow.size()));
  for (const PlacementRecord& record : v.latest_by_flow) EncodePlacementRecord(record, w);
  w.U32(static_cast<std::uint32_t>(v.unfinished.size()));
  for (const AttemptRecord& record : v.unfinished) EncodeAttemptRecord(record, w);
  w.String(v.detail);
}

Status DecodeRecoveryReport(ByteReader& r, RecoveryReport* out) {
  RecoveryReport value;
  bool flag = false;
  if (!ReadBool(r, &flag)) return Status(StatusCode::kWireMalformed, "truncated recovery flag");
  value.liveness_restored = flag;
  if (!ReadBool(r, &value.truncated_tail) || !ReadBool(r, &value.snapshot_loaded)) {
    return Status(StatusCode::kWireMalformed, "truncated recovery flags");
  }
  std::uint64_t* const counters[] = {
      &value.bytes_discarded,   &value.records_valid,        &value.placements_total,
      &value.attempts_total,    &value.committed_attempts,   &value.rejected_attempts,
      &value.cancelled_attempts, &value.fenced_attempts,     &value.unfinished_attempts,
      &value.superseded_placements, &value.placements_requiring_revalidation,
      &value.retired_attempts, &value.max_sequence};
  for (std::uint64_t* counter : counters) {
    if (!r.U64(counter)) return Status(StatusCode::kWireMalformed, "truncated recovery counter");
  }
  std::uint64_t epoch = 0;
  std::uint64_t incarnation = 0;
  if (!r.U64(&epoch) || !r.U64(&incarnation)) {
    return Status(StatusCode::kWireMalformed, "truncated recovery authority");
  }
  value.last_epoch = FabricEpoch{epoch};
  value.last_incarnation = IncarnationId{incarnation};
  std::uint32_t placement_count = 0;
  if (!r.U32(&placement_count)) return Status(StatusCode::kWireMalformed, "truncated placement list");
  if (placement_count > 1000000u) return Status(StatusCode::kOversizeRequest, "placement list too long");
  for (std::uint32_t i = 0; i < placement_count; ++i) {
    PlacementRecord record;
    const Status status = DecodePlacementRecord(r, &record);
    if (!status.ok()) return status;
    value.latest_by_flow.push_back(std::move(record));
  }
  std::uint32_t attempt_count = 0;
  if (!r.U32(&attempt_count)) return Status(StatusCode::kWireMalformed, "truncated attempt list");
  if (attempt_count > 1000000u) return Status(StatusCode::kOversizeRequest, "attempt list too long");
  for (std::uint32_t i = 0; i < attempt_count; ++i) {
    AttemptRecord record;
    const Status status = DecodeAttemptRecord(r, &record);
    if (!status.ok()) return status;
    value.unfinished.push_back(std::move(record));
  }
  if (!r.String(&value.detail, kMaxMessageLength)) {
    return Status(StatusCode::kWireMalformed, "truncated recovery detail");
  }
  if (!r.AtEnd()) return Status(StatusCode::kWireMalformed, "trailing bytes after recovery report");
  *out = std::move(value);
  return Status::Ok();
}

}  // namespace flowplace

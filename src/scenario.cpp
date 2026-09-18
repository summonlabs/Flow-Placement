// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/scenario.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flowplace/checked.hpp"
#include "flowplace/version.hpp"

namespace flowplace {
namespace {

Status Reject(StatusCode code, std::string message) {
  return Status(code, std::move(message));
}

std::string LineTag(std::uint64_t line) { return "line " + std::to_string(line) + ": "; }

std::vector<std::string> SplitWhitespace(const std::string& text) {
  std::vector<std::string> tokens;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t' ||
                                   text[index] == '\r' || text[index] == '\n')) {
      ++index;
    }
    const std::size_t start = index;
    while (index < text.size() && text[index] != ' ' && text[index] != '\t' &&
           text[index] != '\r' && text[index] != '\n') {
      ++index;
    }
    if (index > start) tokens.push_back(text.substr(start, index - start));
  }
  return tokens;
}

bool ParseU64(const std::string& text, std::uint64_t* out) {
  if (text.empty()) return false;
  const char* first = text.data();
  const char* last = first + text.size();
  const std::from_chars_result result = std::from_chars(first, last, *out);
  return result.ec == std::errc() && result.ptr == last;
}

// Splits a comma-separated value. The element count is bounded before the
// strings are materialised, so an oversized list cannot allocate first and be
// rejected afterwards.
bool SplitCommas(const std::string& text, std::uint64_t max_count,
                 std::vector<std::string>* out) {
  out->clear();
  std::uint64_t elements = 1;
  for (const char character : text) {
    if (character == ',') {
      ++elements;
      if (elements > max_count) return false;
    }
  }
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    if (comma == std::string::npos) {
      out->push_back(text.substr(start));
      break;
    }
    out->push_back(text.substr(start, comma - start));
    start = comma + 1;
  }
  return true;
}

// Strict key/value argument parser. Every key must be consumed exactly once;
// an unknown or repeated key is a parse error.
class ArgParser {
 public:
  // A directive accepts at most kMaxKeysPerDirective key/value pairs. The bound
  // keeps the duplicate-key check linear in practice even for a hostile line.
  static constexpr std::size_t kMaxKeysPerDirective = 64;

  ArgParser(std::string_view directive, std::vector<std::string> tokens, std::uint64_t line)
      : directive_(directive), line_(line) {
    if (tokens.size() % 2 != 0) {
      malformed_ = true;
      return;
    }
    if (tokens.size() / 2 > kMaxKeysPerDirective) {
      oversized_ = true;
      return;
    }
    for (std::size_t i = 0; i + 1 < tokens.size(); i += 2) {
      for (const auto& existing : args_) {
        if (existing.key == tokens[i]) {
          duplicate_ = tokens[i];
          return;
        }
      }
      args_.push_back(Argument{tokens[i], tokens[i + 1], false});
    }
    ok_ = true;
  }

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] bool malformed() const { return malformed_; }
  [[nodiscard]] const std::string& duplicate() const { return duplicate_; }

  Status Validate() const {
    if (oversized_) {
      return Reject(StatusCode::kOversizeRequest,
                    LineTag(line_) + directive_ + ": too many keys on one directive");
    }
    if (malformed_) return Reject(StatusCode::kMalformedRequest, LineTag(line_) + directive_ + ": expected key/value pairs");
    if (!duplicate_.empty()) {
      return Reject(StatusCode::kMalformedRequest,
                    LineTag(line_) + directive_ + ": duplicate key '" + duplicate_ + "'");
    }
    return Status::Ok();
  }

  Status Find(std::string_view key, bool required, const std::string** out) {
    for (auto& entry : args_) {
      if (entry.key == key) {
        entry.consumed = true;
        *out = &entry.value;
        return Status::Ok();
      }
    }
    if (required) {
      return Reject(StatusCode::kMissingField,
                    LineTag(line_) + directive_ + ": missing key '" + std::string(key) + "'");
    }
    *out = nullptr;
    return Status::Ok();
  }

  Status U64(std::string_view key, bool required, std::uint64_t* out) {
    const std::string* value = nullptr;
    Status status = Find(key, required, &value);
    if (!status.ok()) return status;
    if (value == nullptr) return Status::Ok();
    if (!ParseU64(*value, out)) {
      return Reject(StatusCode::kInvalidFieldValue,
                    LineTag(line_) + directive_ + ": key '" + std::string(key) +
                        "' is not an unsigned integer");
    }
    return Status::Ok();
  }

  Status U32(std::string_view key, bool required, std::uint32_t* out) {
    std::uint64_t raw = 0;
    const Status status = U64(key, required, &raw);
    if (!status.ok()) return status;
    const std::optional<std::uint32_t> narrowed = NarrowU32(raw);
    if (!narrowed.has_value()) {
      return Reject(StatusCode::kUnrepresentableValue,
                    LineTag(line_) + directive_ + ": key '" + std::string(key) + "' exceeds 32 bits");
    }
    *out = *narrowed;
    return Status::Ok();
  }

  Status Bool(std::string_view key, bool required, bool* out) {
    const std::string* value = nullptr;
    const Status status = Find(key, required, &value);
    if (!status.ok()) return status;
    if (value == nullptr) return Status::Ok();
    if (*value == "true") {
      *out = true;
    } else if (*value == "false") {
      *out = false;
    } else {
      return Reject(StatusCode::kInvalidFieldValue,
                    LineTag(line_) + directive_ + ": key '" + std::string(key) +
                        "' must be true or false");
    }
    return Status::Ok();
  }

  Status String(std::string_view key, bool required, std::string* out, std::uint64_t max_length) {
    const std::string* value = nullptr;
    const Status status = Find(key, required, &value);
    if (!status.ok()) return status;
    if (value == nullptr) return Status::Ok();
    if (value->size() > max_length) {
      return Reject(StatusCode::kOversizeRequest,
                    LineTag(line_) + directive_ + ": key '" + std::string(key) + "' is too long");
    }
    *out = *value;
    return Status::Ok();
  }

  template <class IdT>
  Status Id(std::string_view key, bool required, IdT* out) {
    std::uint64_t raw = 0;
    const Status status = U64(key, required, &raw);
    if (!status.ok()) return status;
    *out = IdT{raw};
    return Status::Ok();
  }

  template <class IdT>
  Status IdList(std::string_view key, bool required, std::vector<IdT>* out, std::uint64_t max_count) {
    const std::string* value = nullptr;
    const Status status = Find(key, required, &value);
    if (!status.ok()) return status;
    if (value == nullptr) return Status::Ok();
    std::vector<std::string> parts;
    if (!SplitCommas(*value, max_count, &parts)) {
      return Reject(StatusCode::kOversizeRequest,
                    LineTag(line_) + directive_ + ": key '" + std::string(key) +
                        "' has too many entries");
    }
    out->clear();
    for (const std::string& part : parts) {
      std::uint64_t raw = 0;
      if (!ParseU64(part, &raw)) {
        return Reject(StatusCode::kInvalidFieldValue,
                      LineTag(line_) + directive_ + ": key '" + std::string(key) +
                          "' contains a non-numeric id");
      }
      out->push_back(IdT{raw});
    }
    return Status::Ok();
  }

  Status RefList(std::string_view key, bool required, std::vector<ReservationRef>* out,
                 std::uint64_t max_count) {
    const std::string* value = nullptr;
    const Status status = Find(key, required, &value);
    if (!status.ok()) return status;
    if (value == nullptr) return Status::Ok();
    std::vector<std::string> parts;
    if (!SplitCommas(*value, max_count, &parts)) {
      return Reject(StatusCode::kOversizeRequest,
                    LineTag(line_) + directive_ + ": key '" + std::string(key) +
                        "' has too many entries");
    }
    out->clear();
    for (const std::string& part : parts) {
      const std::size_t colon = part.find(':');
      if (colon == std::string::npos) {
        return Reject(StatusCode::kInvalidFieldValue,
                      LineTag(line_) + directive_ + ": key '" + std::string(key) +
                          "' entries must be id:generation");
      }
      std::uint64_t id = 0;
      std::uint64_t generation = 0;
      if (!ParseU64(part.substr(0, colon), &id) || !ParseU64(part.substr(colon + 1), &generation)) {
        return Reject(StatusCode::kInvalidFieldValue,
                      LineTag(line_) + directive_ + ": key '" + std::string(key) +
                          "' entries must be id:generation");
      }
      ReservationRef ref;
      ref.id = ReservationId{id};
      ref.generation = ReservationGeneration{generation};
      out->push_back(ref);
    }
    return Status::Ok();
  }

  template <class T, class ParseFn>
  Status Enum(std::string_view key, bool required, ParseFn parse, T* out) {
    const std::string* value = nullptr;
    const Status status = Find(key, required, &value);
    if (!status.ok()) return status;
    if (value == nullptr) return Status::Ok();
    const std::optional<T> parsed = parse(*value);
    if (!parsed.has_value()) {
      return Reject(StatusCode::kInvalidFieldValue,
                    LineTag(line_) + directive_ + ": key '" + std::string(key) +
                        "' has an unknown value '" + *value + "'");
    }
    *out = *parsed;
    return Status::Ok();
  }

  Status Finish() const {
    for (const auto& entry : args_) {
      if (!entry.consumed) {
        return Reject(StatusCode::kMalformedRequest,
                      LineTag(line_) + directive_ + ": unknown key '" + entry.key + "'");
      }
    }
    return Status::Ok();
  }

 private:
  std::string directive_;
  std::uint64_t line_ = 0;
  struct Argument {
    std::string key;
    std::string value;
    bool consumed = false;
  };
  std::vector<Argument> args_;
  bool malformed_ = false;
  bool oversized_ = false;
  bool ok_ = false;
  std::string duplicate_;
};

Status ParseObjectives(const std::string& text, std::vector<Objective>* out,
                       std::uint64_t max_objectives, std::uint64_t line) {
  std::vector<std::string> parts;
  if (!SplitCommas(text, max_objectives, &parts)) {
    return Reject(StatusCode::kOversizeRequest, LineTag(line) + "policy: too many objectives");
  }
  if (parts.empty()) return Reject(StatusCode::kMissingField, LineTag(line) + "policy: no objectives");
  out->clear();
  for (const std::string& part : parts) {
    const std::size_t colon = part.find(':');
    if (colon == std::string::npos) {
      return Reject(StatusCode::kInvalidFieldValue,
                    LineTag(line) + "policy: objectives must be kind:direction");
    }
    const std::optional<ObjectiveKind> kind = ParseObjectiveKind(part.substr(0, colon));
    const std::optional<Direction> direction = ParseDirection(part.substr(colon + 1));
    if (!kind.has_value() || !direction.has_value()) {
      return Reject(StatusCode::kInvalidFieldValue,
                    LineTag(line) + "policy: unknown objective '" + part + "'");
    }
    Objective objective;
    objective.kind = *kind;
    objective.direction = *direction;
    out->push_back(objective);
  }
  return Status::Ok();
}

Status ParseTierList(const std::string& text, std::vector<PathTier>* out, std::uint64_t line) {
  std::vector<std::string> parts;
  if (!SplitCommas(text, 3, &parts)) {
    return Reject(StatusCode::kOversizeRequest, LineTag(line) + "policy: too many tiers");
  }
  out->clear();
  for (const std::string& part : parts) {
    const std::optional<PathTier> tier = ParsePathTier(part);
    if (!tier.has_value()) {
      return Reject(StatusCode::kInvalidFieldValue, LineTag(line) + "policy: unknown tier '" + part + "'");
    }
    out->push_back(*tier);
  }
  return Status::Ok();
}

void AppendRef(std::string* out, const ReservationRef& ref) {
  out->append(ref.id.ToString());
  out->push_back(':');
  out->append(ref.generation.ToString());
}

// Comma-separated id lists are written as a single token: no spaces, so that
// the document round-trips through the whitespace tokenizer.
template <class IdT>
void AppendIdList(std::string* out, const std::vector<IdT>& ids) {
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i != 0) out->push_back(',');
    out->append(ids[i].ToString());
  }
}

}  // namespace

Result<PlacementRequest> ParseScenario(std::string_view text, const Limits& limits) {
  if (text.size() > kMaxScenarioBytes) {
    return Reject(StatusCode::kOversizeRequest, "scenario exceeds kMaxScenarioBytes");
  }
  PlacementRequest request;
  bool have_flow = false;
  bool have_candidates = false;
  bool have_capacity = false;
  bool have_qos = false;
  bool have_policy = false;
  bool have_expected = false;
  bool have_provenance = false;
  bool have_evidence = false;
  bool have_incumbent = false;
  bool have_attempt = false;
  bool have_occupied = false;

  const std::string document(text);
  std::size_t position = 0;
  std::uint64_t line_number = 0;
  while (position <= document.size()) {
    const std::size_t newline = document.find('\n', position);
    const std::size_t end = newline == std::string::npos ? document.size() : newline;
    std::string line = document.substr(position, end - position);
    position = end + 1;
    ++line_number;

    if (line.size() > kMaxScenarioLineBytes) {
      return Reject(StatusCode::kOversizeRequest, LineTag(line_number) + "line is too long");
    }
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) line = line.substr(0, comment);
    std::vector<std::string> tokens = SplitWhitespace(line);
    if (tokens.empty()) {
      if (newline == std::string::npos) break;
      continue;
    }
    const std::string directive = tokens.front();
    tokens.erase(tokens.begin());

    if (directive == "version") {
      if (tokens.size() != 1) return Reject(StatusCode::kMalformedRequest, LineTag(line_number) + "version: expected one value");
      std::uint64_t value = 0;
      if (!ParseU64(tokens[0], &value)) return Reject(StatusCode::kInvalidFieldValue, LineTag(line_number) + "version: not a number");
      if (value != kScenarioFormatVersion) {
        return Reject(StatusCode::kUnsupported, LineTag(line_number) + "version: unsupported scenario format version");
      }
      continue;
    }
    ArgParser args(directive, tokens, line_number);
    Status status = args.Validate();
    if (!status.ok()) return status;

    if (directive == "flow") {
      if (have_flow) return Reject(StatusCode::kMalformedRequest, LineTag(line_number) + "flow: already declared");
      status = args.Id("id", true, &request.flow);
      if (status.ok()) status = args.Id("gen", true, &request.flow_generation);
      if (!status.ok()) return status;
      have_flow = true;
    } else if (directive == "candidateset") {
      if (have_candidates) return Reject(StatusCode::kMalformedRequest, LineTag(line_number) + "candidateset: already declared");
      status = args.Id("id", true, &request.candidates.id);
      if (status.ok()) status = args.Id("gen", true, &request.candidates.generation);
      if (!status.ok()) return status;
      have_candidates = true;
    } else if (directive == "path") {
      if (!have_candidates) return Reject(StatusCode::kMissingField, LineTag(line_number) + "path: candidateset must be declared first");
      if (request.candidates.paths.size() >= limits.max_candidate_paths) {
        return Reject(StatusCode::kOversizeRequest, LineTag(line_number) + "path: too many candidate paths");
      }
      CandidatePath path;
      status = args.Id("id", true, &path.id);
      if (status.ok()) status = args.Id("auth", true, &path.authority);
      if (status.ok()) status = args.Enum("tier", false, ParsePathTier, &path.attributes.tier);
      if (status.ok()) status = args.Id("locality", false, &path.attributes.locality);
      if (status.ok()) status = args.Enum("scope", false, ParseLocalityScope, &path.attributes.locality_scope);
      if (status.ok()) status = args.Id("domain", false, &path.attributes.failure_domain);
      if (status.ok()) status = args.U64("cost", false, &path.attributes.cost_micro);
      if (status.ok()) status = args.U64("latency", false, &path.attributes.latency_nanos);
      if (status.ok()) {
        std::uint32_t hops = path.attributes.hop_count;
        status = args.U32("hops", false, &hops);
        if (status.ok()) {
          const std::optional<std::uint16_t> narrowed = NarrowU16(hops);
          if (!narrowed.has_value()) {
            status = Reject(StatusCode::kUnrepresentableValue, LineTag(line_number) + "path: hops exceeds 16 bits");
          } else {
            path.attributes.hop_count = *narrowed;
          }
        }
      }
      if (status.ok()) status = args.IdList("labels", false, &path.attributes.labels, limits.max_path_labels);
      if (status.ok()) status = args.RefList("reservations", false, &path.attributes.reservations, limits.max_path_reservations);
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
      request.candidates.paths.push_back(std::move(path));
    } else if (directive == "capacity") {
      if (have_capacity) return Reject(StatusCode::kMalformedRequest, LineTag(line_number) + "capacity: already declared");
      status = args.Id("id", true, &request.capacity.id);
      if (status.ok()) status = args.Id("gen", true, &request.capacity.generation);
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
      have_capacity = true;
    } else if (directive == "entry") {
      if (!have_capacity) return Reject(StatusCode::kMissingField, LineTag(line_number) + "entry: capacity must be declared first");
      if (request.capacity.entries.size() >= limits.max_capacity_entries) {
        return Reject(StatusCode::kOversizeRequest, LineTag(line_number) + "entry: too many capacity entries");
      }
      PathCapacity entry;
      status = args.Id("path", true, &entry.path);
      if (status.ok()) status = args.U64("total", true, &entry.capacity_bytes);
      if (status.ok()) status = args.U64("residual", true, &entry.residual_bytes);
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
      request.capacity.entries.push_back(entry);
    } else if (directive == "qos") {
      if (have_qos) return Reject(StatusCode::kMalformedRequest, LineTag(line_number) + "qos: already declared");
      status = args.Id("id", true, &request.qos.id);
      if (status.ok()) status = args.Id("gen", true, &request.qos.generation);
      if (status.ok()) status = args.Enum("class", true, ParseServiceClass, &request.qos.service_class);
      if (status.ok()) status = args.Enum("priority", false, ParsePriorityClass, &request.qos.priority);
      if (status.ok()) status = args.U64("required", false, &request.qos.required_residual_bytes);
      if (status.ok()) status = args.U64("maxlatency", false, &request.qos.max_latency_nanos);
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
      have_qos = true;
    } else if (directive == "evidence") {
      if (have_evidence) {
        return Reject(StatusCode::kMalformedRequest,
                      LineTag(line_number) + "evidence: already declared");
      }
      have_evidence = true;
      status = args.Id("id", true, &request.evidence.id);
      if (status.ok()) status = args.Id("gen", true, &request.evidence.generation);
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
    } else if (directive == "util") {
      if (request.evidence.entries.size() >= limits.max_evidence_entries) {
        return Reject(StatusCode::kOversizeRequest, LineTag(line_number) + "util: too many evidence entries");
      }
      CongestionEvidenceEntry entry;
      status = args.Id("path", true, &entry.path);
      if (status.ok()) status = args.U32("ppb", true, &entry.utilization_ppb);
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
      request.evidence.entries.push_back(entry);
    } else if (directive == "policy") {
      if (have_policy) return Reject(StatusCode::kMalformedRequest, LineTag(line_number) + "policy: already declared");
      status = args.Id("id", true, &request.policy.id);
      if (status.ok()) status = args.Id("gen", true, &request.policy.generation);
      const std::string* objectives = nullptr;
      if (status.ok()) {
        status = args.Find("objectives", true, &objectives);
        if (status.ok() && objectives != nullptr) {
          status = ParseObjectives(*objectives, &request.policy.objectives, limits.max_objectives, line_number);
        }
      }
      const std::string* tiers = nullptr;
      if (status.ok()) {
        status = args.Find("tiers", false, &tiers);
        if (status.ok() && tiers != nullptr) status = ParseTierList(*tiers, &request.policy.allowed_tiers, line_number);
      }
      if (status.ok()) status = args.IdList("require-labels", false, &request.policy.required_labels, limits.max_policy_id_lists);
      if (status.ok()) status = args.IdList("forbid-labels", false, &request.policy.forbidden_labels, limits.max_policy_id_lists);
      if (status.ok()) {
        LocalityId value;
        status = args.Id("locality-required", false, &value);
        if (status.ok() && value.valid()) request.policy.locality.required_locality = value;
      }
      if (status.ok()) {
        LocalityId value;
        status = args.Id("locality-preferred", false, &value);
        if (status.ok() && value.valid()) request.policy.locality.preferred_locality = value;
      }
      if (status.ok()) status = args.IdList("locality-forbidden", false, &request.policy.locality.forbidden_localities, limits.max_policy_id_lists);
      if (status.ok()) {
        FailureDomainId value;
        status = args.Id("domain-required", false, &value);
        if (status.ok() && value.valid()) request.policy.failure_domains.required_domain = value;
      }
      if (status.ok()) status = args.IdList("domain-forbidden", false, &request.policy.failure_domains.forbidden_domains, limits.max_policy_id_lists);
      if (status.ok()) status = args.Bool("domain-avoid-occupied", false, &request.policy.failure_domains.avoid_occupied_domains);
      if (status.ok()) status = args.U32("domain-min-distinct", false, &request.policy.failure_domains.min_distinct_domains);
      if (status.ok()) status = args.Bool("affinity-required", false, &request.policy.reservation_affinity.required);
      if (status.ok()) status = args.RefList("affinity-refs", false, &request.policy.reservation_affinity.refs, limits.max_affinity_refs);
      if (status.ok()) status = args.U64("admission-headroom", false, &request.policy.admission.min_residual_headroom_bytes);
      if (status.ok()) status = args.Enum("admission-action", false, ParseGateAction, &request.policy.admission.on_below);
      if (status.ok()) status = args.Bool("service-degraded", false, &request.policy.service.allow_degraded);
      if (status.ok()) {
        std::uint32_t steps = request.policy.service.max_relaxation_steps;
        status = args.U32("service-relaxation", false, &steps);
        if (status.ok()) {
          const std::optional<std::uint8_t> narrowed = NarrowU8(steps);
          if (!narrowed.has_value()) {
            status = Reject(StatusCode::kUnrepresentableValue, LineTag(line_number) + "policy: service relaxation exceeds 8 bits");
          } else {
            request.policy.service.max_relaxation_steps = *narrowed;
          }
        }
      }
      if (status.ok()) status = args.Enum("churn-action", false, ParseChurnAction, &request.policy.churn.on_move);
      if (status.ok()) status = args.Bool("churn-prefer-incumbent", false, &request.policy.churn.prefer_incumbent);
      if (status.ok()) status = args.U64("churn-threshold", false, &request.policy.churn.move_improvement_threshold);
      if (status.ok()) status = args.U32("churn-threshold-objective", false, &request.policy.churn.threshold_objective_index);
      if (status.ok()) status = args.Enum("evidence-missing", false, ParseEvidencePolicyMode, &request.policy.evidence.on_missing);
      if (status.ok()) status = args.Enum("evidence-stale", false, ParseEvidencePolicyMode, &request.policy.evidence.on_stale);
      if (status.ok()) status = args.U64("revalidate", false, &request.policy.revalidate_after_nanos);
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
      have_policy = true;
    } else if (directive == "incumbent") {
      if (have_incumbent) {
        return Reject(StatusCode::kMalformedRequest,
                      LineTag(line_number) + "incumbent: already declared");
      }
      have_incumbent = true;
      IncumbentPlacement incumbent;
      status = args.Id("id", true, &incumbent.id);
      if (status.ok()) status = args.Id("gen", true, &incumbent.generation);
      if (status.ok()) status = args.Id("path", true, &incumbent.path);
      if (status.ok()) status = args.Id("pathauth", true, &incumbent.path_authority);
      if (status.ok()) status = args.Id("cset", true, &incumbent.candidate_set_generation);
      if (status.ok()) status = args.Id("cap", true, &incumbent.capacity_generation);
      if (status.ok()) status = args.Id("policy", true, &incumbent.policy_generation);
      if (status.ok()) status = args.Id("qos", true, &incumbent.qos_generation);
      if (status.ok()) status = args.Id("epoch", true, &incumbent.fabric_epoch);
      if (status.ok()) {
        std::vector<ReservationRef> refs;
        status = args.RefList("reservation", false, &refs, 1);
        if (status.ok() && !refs.empty()) incumbent.reservation = refs.front();
      }
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
      request.incumbent = incumbent;
    } else if (directive == "occupied") {
      if (have_occupied) {
        return Reject(StatusCode::kMalformedRequest,
                      LineTag(line_number) + "occupied: already declared");
      }
      have_occupied = true;
      status = args.IdList("domains", true, &request.occupied_failure_domains, limits.max_occupied_failure_domains);
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
    } else if (directive == "expected") {
      if (have_expected) return Reject(StatusCode::kMalformedRequest, LineTag(line_number) + "expected: already declared");
      status = args.Id("pathauth", true, &request.expected.path_authority);
      if (status.ok()) status = args.Id("cset", true, &request.expected.candidate_set);
      if (status.ok()) status = args.Id("cap", true, &request.expected.capacity);
      if (status.ok()) status = args.Id("policy", true, &request.expected.policy);
      if (status.ok()) status = args.Id("qos", true, &request.expected.qos);
      if (status.ok()) status = args.Id("epoch", true, &request.expected.fabric_epoch);
      if (status.ok()) status = args.Id("evidence", false, &request.expected.evidence);
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
      have_expected = true;
    } else if (directive == "provenance") {
      if (have_provenance) return Reject(StatusCode::kMalformedRequest, LineTag(line_number) + "provenance: already declared");
      status = args.String("producer", true, &request.provenance.producer, kMaxProvenanceFieldLength);
      if (status.ok()) status = args.String("version", true, &request.provenance.producer_version, kMaxProvenanceFieldLength);
      if (status.ok()) status = args.U64("sequence", true, &request.provenance.source_sequence);
      if (status.ok()) {
        std::uint64_t observed = 0;
        status = args.U64("observed", false, &observed);
        if (status.ok()) request.provenance.observer_unix_nanos = static_cast<std::int64_t>(observed);
      }
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
      have_provenance = true;
    } else if (directive == "attempt") {
      if (have_attempt) {
        return Reject(StatusCode::kMalformedRequest,
                      LineTag(line_number) + "attempt: already declared");
      }
      have_attempt = true;
      status = args.Id("id", true, &request.attempt);
      if (status.ok()) status = args.Finish();
      if (!status.ok()) return status;
    } else {
      return Reject(StatusCode::kMalformedRequest,
                    LineTag(line_number) + "unknown directive '" + directive + "'");
    }
    if (newline == std::string::npos) break;
  }

  if (!have_flow) return Reject(StatusCode::kMissingField, "scenario is missing the flow directive");
  if (!have_candidates) return Reject(StatusCode::kMissingField, "scenario is missing the candidateset directive");
  if (!have_capacity) return Reject(StatusCode::kMissingField, "scenario is missing the capacity directive");
  if (!have_qos) return Reject(StatusCode::kMissingField, "scenario is missing the qos directive");
  if (!have_policy) return Reject(StatusCode::kMissingField, "scenario is missing the policy directive");
  if (!have_expected) return Reject(StatusCode::kMissingField, "scenario is missing the expected directive");
  return request;
}

std::string WriteScenario(const PlacementRequest& request) {
  std::string out;
  out += "version " + std::to_string(kScenarioFormatVersion) + "\n";
  out += "flow id " + request.flow.ToString() + " gen " + request.flow_generation.ToString() + "\n";
  out += "candidateset id " + request.candidates.id.ToString() + " gen " +
         request.candidates.generation.ToString() + "\n";
  for (const CandidatePath& path : request.candidates.paths) {
    out += "path id " + path.id.ToString() + " auth " + path.authority.ToString();
    out += " tier " + std::string(PathTierName(path.attributes.tier));
    out += " locality " + path.attributes.locality.ToString();
    out += " scope " + std::string(LocalityScopeName(path.attributes.locality_scope));
    out += " domain " + path.attributes.failure_domain.ToString();
    out += " cost " + std::to_string(path.attributes.cost_micro);
    out += " latency " + std::to_string(path.attributes.latency_nanos);
    out += " hops " + std::to_string(path.attributes.hop_count);
    if (!path.attributes.labels.empty()) {
      out += " labels ";
      AppendIdList(&out, path.attributes.labels);
    }
    if (!path.attributes.reservations.empty()) {
      out += " reservations ";
      for (std::size_t i = 0; i < path.attributes.reservations.size(); ++i) {
        if (i != 0) out.push_back(',');
        AppendRef(&out, path.attributes.reservations[i]);
      }
    }
    out += "\n";
  }
  out += "capacity id " + request.capacity.id.ToString() + " gen " +
         request.capacity.generation.ToString() + "\n";
  for (const PathCapacity& entry : request.capacity.entries) {
    out += "entry path " + entry.path.ToString() + " total " + std::to_string(entry.capacity_bytes) +
           " residual " + std::to_string(entry.residual_bytes) + "\n";
  }
  out += "qos id " + request.qos.id.ToString() + " gen " + request.qos.generation.ToString() +
         " class " + std::string(ServiceClassName(request.qos.service_class)) + " priority " +
         std::string(PriorityClassName(request.qos.priority)) + " required " +
         std::to_string(request.qos.required_residual_bytes) + " maxlatency " +
         std::to_string(request.qos.max_latency_nanos) + "\n";
  out += "evidence id " + request.evidence.id.ToString() + " gen " +
         request.evidence.generation.ToString() + "\n";
  for (const CongestionEvidenceEntry& entry : request.evidence.entries) {
    out += "util path " + entry.path.ToString() + " ppb " + std::to_string(entry.utilization_ppb) + "\n";
  }
  const PlacementPolicy& policy = request.policy;
  out += "policy id " + policy.id.ToString() + " gen " + policy.generation.ToString() + " objectives ";
  for (std::size_t i = 0; i < policy.objectives.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += std::string(ObjectiveKindName(policy.objectives[i].kind)) + ":" +
           std::string(DirectionName(policy.objectives[i].direction));
  }
  if (!policy.allowed_tiers.empty()) {
    out += " tiers ";
    for (std::size_t i = 0; i < policy.allowed_tiers.size(); ++i) {
      if (i != 0) out.push_back(',');
      out += std::string(PathTierName(policy.allowed_tiers[i]));
    }
  }
  if (!policy.required_labels.empty()) {
    out += " require-labels ";
    AppendIdList(&out, policy.required_labels);
  }
  if (!policy.forbidden_labels.empty()) {
    out += " forbid-labels ";
    AppendIdList(&out, policy.forbidden_labels);
  }
  if (policy.locality.required_locality) {
    out += " locality-required " + policy.locality.required_locality->ToString();
  }
  if (policy.locality.preferred_locality) {
    out += " locality-preferred " + policy.locality.preferred_locality->ToString();
  }
  if (!policy.locality.forbidden_localities.empty()) {
    out += " locality-forbidden ";
    AppendIdList(&out, policy.locality.forbidden_localities);
  }
  if (policy.failure_domains.required_domain) {
    out += " domain-required " + policy.failure_domains.required_domain->ToString();
  }
  if (!policy.failure_domains.forbidden_domains.empty()) {
    out += " domain-forbidden ";
    AppendIdList(&out, policy.failure_domains.forbidden_domains);
  }
  out += std::string(" domain-avoid-occupied ") +
         (policy.failure_domains.avoid_occupied_domains ? "true" : "false");
  out += " domain-min-distinct " + std::to_string(policy.failure_domains.min_distinct_domains);
  out += std::string(" affinity-required ") + (policy.reservation_affinity.required ? "true" : "false");
  if (!policy.reservation_affinity.refs.empty()) {
    out += " affinity-refs ";
    for (std::size_t i = 0; i < policy.reservation_affinity.refs.size(); ++i) {
      if (i != 0) out.push_back(',');
      AppendRef(&out, policy.reservation_affinity.refs[i]);
    }
  }
  out += " admission-headroom " + std::to_string(policy.admission.min_residual_headroom_bytes);
  out += " admission-action " + std::string(GateActionName(policy.admission.on_below));
  out += std::string(" service-degraded ") + (policy.service.allow_degraded ? "true" : "false");
  out += " service-relaxation " + std::to_string(policy.service.max_relaxation_steps);
  out += " churn-action " + std::string(ChurnActionName(policy.churn.on_move));
  out += std::string(" churn-prefer-incumbent ") + (policy.churn.prefer_incumbent ? "true" : "false");
  out += " churn-threshold " + std::to_string(policy.churn.move_improvement_threshold);
  out += " churn-threshold-objective " + std::to_string(policy.churn.threshold_objective_index);
  out += " evidence-missing " + std::string(EvidencePolicyModeName(policy.evidence.on_missing));
  out += " evidence-stale " + std::string(EvidencePolicyModeName(policy.evidence.on_stale));
  out += " revalidate " + std::to_string(policy.revalidate_after_nanos);
  out += "\n";
  if (request.incumbent) {
    const IncumbentPlacement& incumbent = *request.incumbent;
    out += "incumbent id " + incumbent.id.ToString() + " gen " + incumbent.generation.ToString() +
           " path " + incumbent.path.ToString() + " pathauth " + incumbent.path_authority.ToString() +
           " cset " + incumbent.candidate_set_generation.ToString() + " cap " +
           incumbent.capacity_generation.ToString() + " policy " +
           incumbent.policy_generation.ToString() + " qos " + incumbent.qos_generation.ToString() +
           " epoch " + std::to_string(incumbent.fabric_epoch.value());
    if (incumbent.reservation.valid()) {
      out += " reservation ";
      AppendRef(&out, incumbent.reservation);
    }
    out += "\n";
  }
  if (!request.occupied_failure_domains.empty()) {
    out += "occupied domains ";
    AppendIdList(&out, request.occupied_failure_domains);
    out += "\n";
  }
  out += "expected pathauth " + request.expected.path_authority.ToString() + " cset " +
         request.expected.candidate_set.ToString() + " cap " + request.expected.capacity.ToString() +
         " policy " + request.expected.policy.ToString() + " qos " + request.expected.qos.ToString() +
         " epoch " + std::to_string(request.expected.fabric_epoch.value()) + " evidence " +
         request.expected.evidence.ToString() + "\n";
  out += "provenance producer " + (request.provenance.producer.empty() ? std::string("-") : request.provenance.producer) +
         " version " + (request.provenance.producer_version.empty() ? std::string("-") : request.provenance.producer_version) +
         " sequence " + std::to_string(request.provenance.source_sequence) + " observed " +
         std::to_string(request.provenance.observer_unix_nanos) + "\n";
  out += "attempt id " + request.attempt.ToString() + "\n";
  return out;
}

}  // namespace flowplace

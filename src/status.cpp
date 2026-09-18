// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/status.hpp"

#include <string>

namespace flowplace {
namespace {

struct CodeName {
  StatusCode code;
  std::string_view name;
  std::string_view category;
};

constexpr CodeName kCodeNames[] = {
    {StatusCode::kOk, "OK", "ok"},
    {StatusCode::kMalformedRequest, "MALFORMED_REQUEST", "structural"},
    {StatusCode::kOversizeRequest, "OVERSIZE_REQUEST", "structural"},
    {StatusCode::kDuplicatePathId, "DUPLICATE_PATH_ID", "structural"},
    {StatusCode::kInvalidId, "INVALID_ID", "structural"},
    {StatusCode::kUnknownReference, "UNKNOWN_REFERENCE", "structural"},
    {StatusCode::kInconsistentAuthorityGeneration, "INCONSISTENT_AUTHORITY_GENERATION", "structural"},
    {StatusCode::kContradictoryConstraints, "CONTRADICTORY_CONSTRAINTS", "structural"},
    {StatusCode::kUnrepresentableValue, "UNREPRESENTABLE_VALUE", "structural"},
    {StatusCode::kMissingField, "MISSING_FIELD", "structural"},
    {StatusCode::kInvalidFieldValue, "INVALID_FIELD_VALUE", "structural"},
    {StatusCode::kStaleAuthorityGeneration, "STALE_AUTHORITY_GENERATION", "staleness"},
    {StatusCode::kStaleCandidateSet, "STALE_CANDIDATE_SET", "staleness"},
    {StatusCode::kStaleCapacitySnapshot, "STALE_CAPACITY_SNAPSHOT", "staleness"},
    {StatusCode::kStalePolicy, "STALE_POLICY", "staleness"},
    {StatusCode::kStaleQos, "STALE_QOS", "staleness"},
    {StatusCode::kStaleFabricEpoch, "STALE_FABRIC_EPOCH", "staleness"},
    {StatusCode::kStaleIncumbent, "STALE_INCUMBENT", "staleness"},
    {StatusCode::kStaleEvidence, "STALE_EVIDENCE", "staleness"},
    {StatusCode::kCapacityInsufficient, "CAPACITY_INSUFFICIENT", "feasibility"},
    {StatusCode::kServiceClassUnsatisfied, "SERVICE_CLASS_UNSATISFIED", "feasibility"},
    {StatusCode::kFailureDomainDiversityUnmet, "FAILURE_DOMAIN_DIVERSITY_UNMET", "feasibility"},
    {StatusCode::kLatencyBudgetUnmet, "LATENCY_BUDGET_UNMET", "feasibility"},
    {StatusCode::kPolicyRejectedFlow, "POLICY_REJECTED_FLOW", "policy"},
    {StatusCode::kPolicyRejectedAllPaths, "POLICY_REJECTED_ALL_PATHS", "policy"},
    {StatusCode::kPolicyDeferred, "POLICY_DEFERRED", "policy"},
    {StatusCode::kPolicyMoveSuppressed, "POLICY_MOVE_SUPPRESSED", "policy"},
    {StatusCode::kPolicyHeadroomGate, "POLICY_HEADROOM_GATE", "policy"},
    {StatusCode::kQueueFull, "QUEUE_FULL", "runtime"},
    {StatusCode::kShuttingDown, "SHUTTING_DOWN", "runtime"},
    {StatusCode::kFencedStaleEpoch, "FENCED_STALE_EPOCH", "runtime"},
    {StatusCode::kFencedStaleIncarnation, "FENCED_STALE_INCARNATION", "runtime"},
    {StatusCode::kDuplicateAttemptConflict, "DUPLICATE_ATTEMPT_CONFLICT", "runtime"},
    {StatusCode::kCancelled, "CANCELLED", "runtime"},
    {StatusCode::kAlreadyCommitted, "ALREADY_COMMITTED", "runtime"},
    {StatusCode::kUnknownAttempt, "UNKNOWN_ATTEMPT", "runtime"},
    {StatusCode::kWorkerLimitExceeded, "WORKER_LIMIT_EXCEEDED", "runtime"},
    {StatusCode::kNotRunning, "NOT_RUNNING", "runtime"},
    {StatusCode::kStoreNotFound, "STORE_NOT_FOUND", "store"},
    {StatusCode::kStoreCorrupt, "STORE_CORRUPT", "store"},
    {StatusCode::kStoreTruncatedTail, "STORE_TRUNCATED_TAIL", "store"},
    {StatusCode::kStoreVersionUnsupported, "STORE_VERSION_UNSUPPORTED", "store"},
    {StatusCode::kStoreIoError, "STORE_IO_ERROR", "store"},
    {StatusCode::kStoreGrowthExceeded, "STORE_GROWTH_EXCEEDED", "store"},
    {StatusCode::kStoreAmbiguousAttempt, "STORE_AMBIGUOUS_ATTEMPT", "store"},
    {StatusCode::kFrameTooLarge, "FRAME_TOO_LARGE", "wire"},
    {StatusCode::kFrameChecksumMismatch, "FRAME_CHECKSUM_MISMATCH", "wire"},
    {StatusCode::kProtocolMismatch, "PROTOCOL_MISMATCH", "wire"},
    {StatusCode::kWireMalformed, "WIRE_MALFORMED", "wire"},
    {StatusCode::kConnectionClosed, "CONNECTION_CLOSED", "wire"},
    {StatusCode::kIoError, "IO_ERROR", "generic"},
    {StatusCode::kSocketError, "SOCKET_ERROR", "generic"},
    {StatusCode::kInvalidArgument, "INVALID_ARGUMENT", "generic"},
    {StatusCode::kNotFound, "NOT_FOUND", "generic"},
    {StatusCode::kAlreadyExists, "ALREADY_EXISTS", "generic"},
    {StatusCode::kUnsupported, "UNSUPPORTED", "generic"},
    {StatusCode::kArithmeticOverflow, "ARITHMETIC_OVERFLOW", "generic"},
    {StatusCode::kInternalInvariantViolation, "INTERNAL_INVARIANT_VIOLATION", "generic"},
};

}  // namespace

bool IsKnownStatusCode(std::uint16_t value) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (static_cast<std::uint16_t>(entry.code) == value) return true;
  }
  return false;
}

std::string_view StatusCodeName(StatusCode code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code) return entry.name;
  }
  return "UNKNOWN_CODE";
}

std::string_view StatusCodeCategory(StatusCode code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code) return entry.category;
  }
  return "unknown";
}

std::string Status::ToString() const {
  std::string out(StatusCodeName(code_));
  if (!message_.empty()) {
    out.append(": ");
    out.append(message_);
  }
  return out;
}

}  // namespace flowplace

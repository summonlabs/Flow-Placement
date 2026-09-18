// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Status and Result: the library never throws across its public boundary. Every
// fallible operation reports a precise, enumerable StatusCode so that callers
// can distinguish malformed input, stale authority, fencing, and I/O failure
// without parsing strings.

#ifndef FLOWPLACE_STATUS_HPP
#define FLOWPLACE_STATUS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace flowplace {

enum class StatusCode : std::uint16_t {
  kOk = 0,

  // ---- Structural / malformed input -------------------------------------
  kMalformedRequest = 10,
  kOversizeRequest = 11,
  kDuplicatePathId = 12,
  kInvalidId = 13,
  kUnknownReference = 14,
  kInconsistentAuthorityGeneration = 15,
  kContradictoryConstraints = 16,
  kUnrepresentableValue = 17,
  kMissingField = 18,
  kInvalidFieldValue = 19,

  // ---- Staleness --------------------------------------------------------
  kStaleAuthorityGeneration = 30,
  kStaleCandidateSet = 31,
  kStaleCapacitySnapshot = 32,
  kStalePolicy = 33,
  kStaleQos = 34,
  kStaleFabricEpoch = 35,
  kStaleIncumbent = 36,
  kStaleEvidence = 37,

  // ---- Feasibility ------------------------------------------------------
  kCapacityInsufficient = 50,
  kServiceClassUnsatisfied = 51,
  kFailureDomainDiversityUnmet = 52,
  kLatencyBudgetUnmet = 53,

  // ---- Policy -----------------------------------------------------------
  kPolicyRejectedFlow = 70,
  kPolicyRejectedAllPaths = 71,
  kPolicyDeferred = 72,
  kPolicyMoveSuppressed = 73,
  kPolicyHeadroomGate = 74,

  // ---- Runtime / concurrency -------------------------------------------
  kQueueFull = 90,
  kShuttingDown = 91,
  kFencedStaleEpoch = 92,
  kFencedStaleIncarnation = 93,
  kDuplicateAttemptConflict = 94,
  kCancelled = 95,
  kAlreadyCommitted = 96,
  kUnknownAttempt = 97,
  kWorkerLimitExceeded = 98,
  kNotRunning = 99,

  // ---- Durable store ----------------------------------------------------
  kStoreNotFound = 110,
  kStoreCorrupt = 111,
  kStoreTruncatedTail = 112,
  kStoreVersionUnsupported = 113,
  kStoreIoError = 114,
  kStoreGrowthExceeded = 115,
  kStoreAmbiguousAttempt = 116,

  // ---- Wire -------------------------------------------------------------
  kFrameTooLarge = 130,
  kFrameChecksumMismatch = 131,
  kProtocolMismatch = 132,
  kWireMalformed = 133,
  kConnectionClosed = 134,

  // ---- Generic ----------------------------------------------------------
  kIoError = 150,
  kSocketError = 151,
  kInvalidArgument = 152,
  kNotFound = 153,
  kAlreadyExists = 154,
  kUnsupported = 155,
  kArithmeticOverflow = 156,
  kInternalInvariantViolation = 157,
};

std::string_view StatusCodeName(StatusCode code) noexcept;
std::string_view StatusCodeCategory(StatusCode code) noexcept;
// True when |value| is one of the defined status codes. Decoders use this to
// reject unrepresentable codes instead of carrying them as unknown values.
[[nodiscard]] bool IsKnownStatusCode(std::uint16_t value) noexcept;

class Status {
 public:
  Status() noexcept = default;
  Status(StatusCode code, std::string message) noexcept
      : code_(code), message_(std::move(message)) {}

  static Status Ok() noexcept { return Status{}; }

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::kOk; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] std::string_view message() const noexcept { return message_; }
  [[nodiscard]] std::string ToString() const;

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

// Minimal expected-like result type. This library targets C++20 and therefore
// cannot rely on std::expected (C++23).
template <class T>
class Result {
 public:
  Result(T value) noexcept : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) noexcept : storage_(std::move(status)) {}   // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept {
    return ok() ? kOkStatus() : std::get<1>(storage_);
  }
  [[nodiscard]] StatusCode code() const noexcept { return status().code(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] T value_or(T fallback) const {
    return ok() ? std::get<0>(storage_) : std::move(fallback);
  }

 private:
  static const Status& kOkStatus() noexcept {
    static const Status kOk{};
    return kOk;
  }
  std::variant<T, Status> storage_;
};

// Result<void> is not needed by this library's API; fallible void operations
// return Status directly.

}  // namespace flowplace

#endif  // FLOWPLACE_STATUS_HPP

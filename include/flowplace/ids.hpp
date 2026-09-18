// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Strongly typed identities, generations, epochs, and provenance.
//
// Every identity in this library is a distinct type: a FlowId can never be
// passed where a PathId is expected, and a generation can never be compared
// against an identity. Generations are monotone counters whose advancement is
// checked; a generation that cannot advance reports that fact instead of
// wrapping.

#ifndef FLOWPLACE_IDS_HPP
#define FLOWPLACE_IDS_HPP

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

namespace flowplace {

// ---------------------------------------------------------------------------
// Identity tags. These are incomplete types used only as type-level markers.
// ---------------------------------------------------------------------------
struct FlowTag;
struct PlacementTag;
struct CandidateSetTag;
struct PathTag;
struct CapacitySnapshotTag;
struct PolicyTag;
struct QosProfileTag;
struct ReservationTag;
struct EvidenceTag;
struct FailureDomainTag;
struct LocalityTag;
struct PolicyLabelTag;
struct WorkerTag;
struct PublisherTag;
struct AttemptTag;
struct IncarnationTag;
struct NodeTag;

template <class Tag>
class Id {
 public:
  using tag = Tag;
  using value_type = std::uint64_t;

  constexpr Id() noexcept = default;
  constexpr explicit Id(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  // Identity 0 is reserved for "absent"/"unset". A valid identity is non-zero.
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(Id a, Id b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Id a, Id b) noexcept { return a.value_ != b.value_; }
  friend constexpr bool operator<(Id a, Id b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator>(Id a, Id b) noexcept { return b.value_ < a.value_; }
  friend constexpr bool operator<=(Id a, Id b) noexcept { return !(b.value_ < a.value_); }
  friend constexpr bool operator>=(Id a, Id b) noexcept { return !(a.value_ < b.value_); }

  [[nodiscard]] std::string ToString() const { return std::to_string(value_); }

 private:
  std::uint64_t value_ = 0;
};

using FlowId = Id<FlowTag>;
using PlacementId = Id<PlacementTag>;
using CandidateSetId = Id<CandidateSetTag>;
using PathId = Id<PathTag>;
using CapacitySnapshotId = Id<CapacitySnapshotTag>;
using PolicyId = Id<PolicyTag>;
using QosProfileId = Id<QosProfileTag>;
using ReservationId = Id<ReservationTag>;
using EvidenceId = Id<EvidenceTag>;
using FailureDomainId = Id<FailureDomainTag>;
using LocalityId = Id<LocalityTag>;
using PolicyLabelId = Id<PolicyLabelTag>;
using WorkerId = Id<WorkerTag>;
using PublisherId = Id<PublisherTag>;
using AttemptId = Id<AttemptTag>;
using IncarnationId = Id<IncarnationTag>;
using NodeId = Id<NodeTag>;

// ---------------------------------------------------------------------------
// Generations. A generation orders successive revisions of one authority
// domain. Generation 0 means "unset"; generation 1 is the first revision.
// ---------------------------------------------------------------------------
template <class Tag>
class Generation {
 public:
  using tag = Tag;

  constexpr Generation() noexcept = default;
  constexpr explicit Generation(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  // True when this generation is strictly newer than |other|.
  [[nodiscard]] constexpr bool IsNewerThan(Generation other) const noexcept {
    return value_ > other.value_;
  }
  // Returns false when the counter cannot advance (it would wrap).
  [[nodiscard]] constexpr bool Advance() noexcept {
    if (value_ == std::numeric_limits<std::uint64_t>::max()) return false;
    ++value_;
    return true;
  }
  // Returns the next generation, or the current one when it cannot advance.
  [[nodiscard]] constexpr Generation Next() const noexcept {
    Generation copy = *this;
    static_cast<void>(copy.Advance());
    return copy;
  }

  friend constexpr bool operator==(Generation a, Generation b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(Generation a, Generation b) noexcept {
    return a.value_ != b.value_;
  }
  friend constexpr bool operator<(Generation a, Generation b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator>(Generation a, Generation b) noexcept { return b < a; }
  friend constexpr bool operator<=(Generation a, Generation b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(Generation a, Generation b) noexcept { return !(a < b); }

  [[nodiscard]] std::string ToString() const { return std::to_string(value_); }

 private:
  std::uint64_t value_ = 0;
};

struct FlowGenerationTag;
struct PlacementGenerationTag;
struct CandidateSetGenerationTag;
struct PathAuthorityGenerationTag;
struct CapacitySnapshotGenerationTag;
struct PolicyGenerationTag;
struct QosGenerationTag;
struct ReservationGenerationTag;
struct EvidenceGenerationTag;

using FlowGeneration = Generation<FlowGenerationTag>;
using PlacementGeneration = Generation<PlacementGenerationTag>;
using CandidateSetGeneration = Generation<CandidateSetGenerationTag>;
using PathAuthorityGeneration = Generation<PathAuthorityGenerationTag>;
using CapacitySnapshotGeneration = Generation<CapacitySnapshotGenerationTag>;
using PolicyGeneration = Generation<PolicyGenerationTag>;
using QosGeneration = Generation<QosGenerationTag>;
using ReservationGeneration = Generation<ReservationGenerationTag>;
using EvidenceGeneration = Generation<EvidenceGenerationTag>;

// ---------------------------------------------------------------------------
// Fabric epoch. An epoch bounds the authority of a whole fabric view; work
// bound to a superseded epoch must never mutate authoritative state.
// ---------------------------------------------------------------------------
class FabricEpoch {
 public:
  constexpr FabricEpoch() noexcept = default;
  constexpr explicit FabricEpoch(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr bool IsNewerThan(FabricEpoch other) const noexcept {
    return value_ > other.value_;
  }
  [[nodiscard]] constexpr bool Advance() noexcept {
    if (value_ == std::numeric_limits<std::uint64_t>::max()) return false;
    ++value_;
    return true;
  }

  friend constexpr bool operator==(FabricEpoch a, FabricEpoch b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(FabricEpoch a, FabricEpoch b) noexcept { return !(a == b); }
  friend constexpr bool operator<(FabricEpoch a, FabricEpoch b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator>(FabricEpoch a, FabricEpoch b) noexcept { return b < a; }
  friend constexpr bool operator<=(FabricEpoch a, FabricEpoch b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(FabricEpoch a, FabricEpoch b) noexcept { return !(a < b); }

 private:
  std::uint64_t value_ = 0;
};

// A process incarnation is fresh on every process start and is never reused
// across restarts. Durable state must never resurrect a previous incarnation.
using Incarnation = Id<IncarnationTag>;

// ---------------------------------------------------------------------------
// Provenance. Provenance is descriptive metadata. Wall-clock fields are
// informational only and are never used to make or to justify a decision.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxProvenanceFieldLength = 128;

struct Provenance {
  std::string producer;                       // bounded by kMaxProvenanceFieldLength
  std::string producer_version;               // bounded by kMaxProvenanceFieldLength
  std::uint64_t source_sequence = 0;          // caller-supplied ordering hint
  std::int64_t observer_unix_nanos = 0;       // informational only, never authority

  [[nodiscard]] bool operator==(const Provenance& other) const noexcept {
    return producer == other.producer && producer_version == other.producer_version &&
           source_sequence == other.source_sequence &&
           observer_unix_nanos == other.observer_unix_nanos;
  }
};

}  // namespace flowplace

#endif  // FLOWPLACE_IDS_HPP

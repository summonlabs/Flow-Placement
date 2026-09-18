// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Durable placement store: a versioned, integrity-checked, crash-safe
// append-only journal with an atomically replaced snapshot for compaction.
//
// Record layout (all integers little-endian):
//   0   u32 magic 'FPRC'
//   4   u32 container format version
//   8   u16 record type (1 = placement, 2 = attempt)
//   10  u16 flags (must be zero)
//   12  u64 sequence
//   20  u32 payload length
//   24  u32 payload CRC-32C
//   28  payload
//   +   u32 record CRC-32C over header and payload
//   +   u32 end magic 'FEND'
//
// A record is durable before it is acknowledged. Recovery is fail-closed:
// damage in the middle of a journal is an error, while a torn tail is repaired
// by truncation and reported precisely.

#include "flowplace/store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "flowplace/codec.hpp"
#include "flowplace/hash.hpp"
#include "flowplace/process.hpp"
#include "flowplace/version.hpp"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace flowplace {

std::string_view AttemptPhaseName(AttemptPhase v) noexcept {
  switch (v) {
    case AttemptPhase::kStarted: return "STARTED";
    case AttemptPhase::kCommitted: return "COMMITTED";
    case AttemptPhase::kRejected: return "REJECTED";
    case AttemptPhase::kCancelled: return "CANCELLED";
    case AttemptPhase::kFenced: return "FENCED";
  }
  return "UNKNOWN_PHASE";
}

std::optional<AttemptPhase> ParseAttemptPhase(std::string_view v) noexcept {
  if (v == "STARTED") return AttemptPhase::kStarted;
  if (v == "COMMITTED") return AttemptPhase::kCommitted;
  if (v == "REJECTED") return AttemptPhase::kRejected;
  if (v == "CANCELLED") return AttemptPhase::kCancelled;
  if (v == "FENCED") return AttemptPhase::kFenced;
  return std::nullopt;
}

namespace {

constexpr std::uint32_t kRecordMagic = 0x43525046u;  // 'F','P','R','C'
constexpr std::uint32_t kRecordEndMagic = 0x444E4546u;  // 'F','E','N','D'
constexpr std::uint32_t kSnapshotMagic = 0x4E535046u;   // 'F','P','S','N'
constexpr std::uint32_t kSnapshotEndMagic = 0x45535046u;  // 'F','P','S','E'
constexpr std::uint32_t kHeaderBytes = 28;
constexpr std::uint32_t kTrailerBytes = 8;
constexpr std::uint16_t kRecordTypePlacement = 1;
constexpr std::uint16_t kRecordTypeAttempt = 2;
// Written as the first record of a journal that follows a compaction. Its
// payload carries the sequence the snapshot covers, so a journal that was
// replaced after a crash is recognised and the rebased sequence is accepted.
constexpr std::uint16_t kRecordTypeJournalReset = 3;

Status IoError(const std::string& what, const std::string& path) {
  return Status(StatusCode::kStoreIoError, what + ": " + path);
}

// 64-bit positioned read support; long is 32-bit on Windows.
Status SeekFile(std::FILE* file, std::uint64_t offset) {
#ifdef _WIN32
  if (_fseeki64(file, static_cast<long long>(offset), SEEK_SET) != 0) {
    return Status(StatusCode::kStoreIoError, "seek failed");
  }
#else
  if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {
    return Status(StatusCode::kStoreIoError, "seek failed");
  }
#endif
  return Status::Ok();
}

std::string SnapshotPathFor(const std::string& path) { return path + ".snap"; }
std::string TempPathFor(const std::string& path) { return path + ".tmp"; }

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}

std::uint32_t GetU32(const std::uint8_t* data) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[i]) << (8 * i);
  return v;
}

std::uint16_t GetU16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    (static_cast<std::uint16_t>(data[1]) << 8));
}

std::uint64_t GetU64(const std::uint8_t* data) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(data[i]) << (8 * i);
  return v;
}

Status SyncFile(std::FILE* file) {
  if (std::fflush(file) != 0) return Status(StatusCode::kStoreIoError, "fflush failed");
#ifdef _WIN32
  if (_commit(_fileno(file)) != 0) return Status(StatusCode::kStoreIoError, "_commit failed");
#else
  if (fsync(fileno(file)) != 0) return Status(StatusCode::kStoreIoError, "fsync failed");
#endif
  return Status::Ok();
}

Status ReplaceFileAtomically(const std::string& from, const std::string& to) {
#ifdef _WIN32
  if (MoveFileExA(from.c_str(), to.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Status(StatusCode::kStoreIoError, "MoveFileEx failed for " + to);
  }
  return Status::Ok();
#else
  if (std::rename(from.c_str(), to.c_str()) != 0) {
    return Status(StatusCode::kStoreIoError, "rename failed for " + to);
  }
  const std::string directory = to.substr(0, to.find_last_of('/') + 1);
  const int dir_fd = ::open(directory.empty() ? "." : directory.c_str(), O_RDONLY);
  if (dir_fd >= 0) {
    static_cast<void>(::fsync(dir_fd));
    static_cast<void>(::close(dir_fd));
  }
  return Status::Ok();
#endif
}

std::uint64_t FileSizeBytes(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return 0;
  static_cast<void>(std::fseek(file, 0, SEEK_END));
  const long size = std::ftell(file);
  std::fclose(file);
  return size > 0 ? static_cast<std::uint64_t>(size) : 0;
}

struct RecordHeader {
  std::uint16_t type = 0;
  std::uint16_t flags = 0;
  std::uint64_t sequence = 0;
  std::uint32_t payload_length = 0;
  std::uint32_t payload_crc = 0;
};

bool ParseHeader(const std::uint8_t* raw, std::uint32_t copy_limit, RecordHeader* out) {
  if (GetU32(raw) != kRecordMagic) return false;
  const std::uint32_t version = GetU32(raw + 4);
  if (version != kStoreFormatVersion) return false;
  out->type = GetU16(raw + 8);
  if (out->type != kRecordTypePlacement && out->type != kRecordTypeAttempt &&
      out->type != kRecordTypeJournalReset) {
    return false;
  }
  out->flags = GetU16(raw + 10);
  if (out->flags != 0) return false;
  out->sequence = GetU64(raw + 12);
  out->payload_length = GetU32(raw + 20);
  if (out->payload_length > copy_limit) return false;
  out->payload_crc = GetU32(raw + 24);
  return true;
}

}  // namespace

struct PlacementStore::Impl {
  std::vector<PlacementRecord> placements;                 // sequence order
  std::map<FlowId, std::size_t> latest_by_flow;            // flow -> index into placements
  std::map<AttemptId, AttemptRecord> attempts;             // attempt -> latest record
  std::map<AttemptId, std::size_t> commit_by_attempt;      // attempt -> index into placements
  std::uint64_t next_placement_id = 1;
  std::uint64_t max_sequence = 0;
  bool closed = false;
  bool compaction_in_progress = false;
  bool compacting = false;

  const PlacementRecord* Latest(FlowId flow) const {
    const auto it = latest_by_flow.find(flow);
    if (it == latest_by_flow.end()) return nullptr;
    return &placements[it->second];
  }
};

PlacementStore::~PlacementStore() {
  if (file_ != nullptr) {
    static_cast<void>(std::fclose(static_cast<std::FILE*>(file_)));
    file_ = nullptr;
  }
  delete impl_;
  impl_ = nullptr;
}

PlacementStore::PlacementStore(PlacementStore&& other) noexcept
    : path_(std::move(other.path_)),
      options_(other.options_),
      recovery_(std::move(other.recovery_)),
      stats_(other.stats_),
      file_(other.file_),
      impl_(other.impl_) {
  other.file_ = nullptr;
  other.impl_ = nullptr;
}

PlacementStore& PlacementStore::operator=(PlacementStore&& other) noexcept {
  if (this == &other) return *this;
  if (file_ != nullptr) static_cast<void>(std::fclose(static_cast<std::FILE*>(file_)));
  delete impl_;
  path_ = std::move(other.path_);
  options_ = other.options_;
  recovery_ = std::move(other.recovery_);
  stats_ = other.stats_;
  file_ = other.file_;
  impl_ = other.impl_;
  other.file_ = nullptr;
  other.impl_ = nullptr;
  return *this;
}

Result<PlacementStore> PlacementStore::Open(const std::string& path, StoreOptions options) {
  PlacementStore store;
  store.path_ = path;
  store.options_ = options;
  store.impl_ = new Impl();

  const std::string snapshot_path = SnapshotPathFor(path);
  std::FILE* snapshot = std::fopen(snapshot_path.c_str(), "rb");
  if (snapshot != nullptr) {
    static_cast<void>(std::fclose(snapshot));
    const Status status = store.LoadSnapshot();
    if (!status.ok()) return status;
    store.recovery_.snapshot_loaded = true;
  }

  const Status journal_status = store.LoadJournal();
  if (!journal_status.ok()) return journal_status;

  if (!options.read_only) {
    std::FILE* file = std::fopen(path.c_str(), "ab");
    if (file == nullptr) return IoError("cannot open store for append", path);
    store.file_ = file;
  }
  store.stats_.journal_bytes = FileSizeBytes(path);
  store.stats_.snapshot_bytes = FileSizeBytes(snapshot_path);

  // ---- recovery accounting ------------------------------------------------
  RecoveryReport& report = store.recovery_;
  Impl& impl = *store.impl_;
  report.placements_total = impl.placements.size();
  report.attempts_total = impl.attempts.size();
  report.max_sequence = impl.max_sequence;
  report.latest_by_flow.reserve(impl.latest_by_flow.size());
  for (const auto& pair : impl.latest_by_flow) {
    report.latest_by_flow.push_back(impl.placements[pair.second]);
  }
  report.superseded_placements = impl.placements.size() - impl.latest_by_flow.size();
  report.placements_requiring_revalidation = report.latest_by_flow.size();

  bool orphan_commit = false;
  for (auto& pair : impl.attempts) {
    AttemptRecord& attempt = pair.second;
    const auto committed = impl.commit_by_attempt.find(pair.first);
    switch (attempt.phase) {
      case AttemptPhase::kStarted:
        if (committed != impl.commit_by_attempt.end()) {
          attempt.phase = AttemptPhase::kCommitted;
          ++report.committed_attempts;
        } else {
          ++report.unfinished_attempts;
          if (report.unfinished.size() < 64) report.unfinished.push_back(attempt);
        }
        break;
      case AttemptPhase::kCommitted:
        if (committed == impl.commit_by_attempt.end()) {
          orphan_commit = true;
        } else {
          ++report.committed_attempts;
        }
        break;
      case AttemptPhase::kRejected: ++report.rejected_attempts; break;
      case AttemptPhase::kCancelled: ++report.cancelled_attempts; break;
      case AttemptPhase::kFenced: ++report.fenced_attempts; break;
    }
    if (attempt.epoch > report.last_epoch) report.last_epoch = attempt.epoch;
    if (attempt.incarnation.valid()) report.last_incarnation = attempt.incarnation;
  }
  for (const PlacementRecord& record : impl.placements) {
    if (record.commit_epoch > report.last_epoch) report.last_epoch = record.commit_epoch;
    if (record.committed_by.valid()) report.last_incarnation = record.committed_by;
  }
  if (orphan_commit) {
    return Status(StatusCode::kStoreCorrupt,
                  "store records an acknowledged attempt with no durable placement");
  }
  report.liveness_restored = false;  // durable state never restores liveness
  if (report.unfinished_attempts != 0) {
    report.detail += "unfinished attempts require re-driving; ";
  }
  if (report.placements_requiring_revalidation != 0) {
    report.detail += "recovered placements require revalidation against current evidence";
  }
  if (report.truncated_tail) {
    report.detail += "; torn journal tail was discarded";
  }
  return store;
}

Status PlacementStore::LoadSnapshot() {
  const std::string snapshot_path = SnapshotPathFor(path_);
  std::FILE* file = std::fopen(snapshot_path.c_str(), "rb");
  if (file == nullptr) return Status::Ok();
  struct Closer {
    std::FILE* file;
    ~Closer() { std::fclose(file); }
  } closer{file};

  std::uint8_t header[16];
  if (std::fread(header, 1, sizeof(header), file) != sizeof(header)) {
    return Status(StatusCode::kStoreCorrupt, "snapshot header is truncated");
  }
  if (GetU32(header) != kSnapshotMagic) {
    return Status(StatusCode::kStoreCorrupt, "snapshot magic mismatch");
  }
  if (GetU32(header + 4) != kStoreFormatVersion) {
    return Status(StatusCode::kStoreVersionUnsupported, "unsupported snapshot format version");
  }
  const std::uint32_t payload_length = GetU32(header + 8);
  const std::uint32_t payload_crc = GetU32(header + 12);
  if (payload_length > options_.max_record_bytes) {
    return Status(StatusCode::kStoreCorrupt, "snapshot payload exceeds the configured bound");
  }
  std::vector<std::uint8_t> payload(payload_length);
  if (payload_length != 0 && std::fread(payload.data(), 1, payload_length, file) != payload_length) {
    return Status(StatusCode::kStoreCorrupt, "snapshot payload is truncated");
  }
  std::uint8_t trailer[8];
  if (std::fread(trailer, 1, sizeof(trailer), file) != sizeof(trailer)) {
    return Status(StatusCode::kStoreCorrupt, "snapshot trailer is truncated");
  }
  if (GetU32(trailer + 4) != kSnapshotEndMagic) {
    return Status(StatusCode::kStoreCorrupt, "snapshot end magic mismatch");
  }
  if (Crc32C::Compute(payload.data(), payload.size()) != payload_crc) {
    return Status(StatusCode::kStoreCorrupt, "snapshot payload checksum mismatch");
  }
  // A trailing partial write must not exist in an atomically replaced snapshot.
  std::uint8_t extra = 0;
  if (std::fread(&extra, 1, 1, file) == 1) {
    return Status(StatusCode::kStoreCorrupt, "snapshot has trailing bytes");
  }

  ByteReader reader(payload);
  std::uint64_t next_id = 0;
  std::uint64_t max_sequence = 0;
  std::uint64_t retired = 0;
  std::uint32_t placement_count = 0;
  if (!reader.U64(&next_id) || !reader.U64(&max_sequence) || !reader.U64(&retired) ||
      !reader.U32(&placement_count)) {
    return Status(StatusCode::kStoreCorrupt, "snapshot payload header is truncated");
  }
  recovery_.retired_attempts = retired;
  if (placement_count > options_.max_total_records) {
    return Status(StatusCode::kStoreCorrupt, "snapshot lists too many placements");
  }
  impl_->placements.clear();
  impl_->latest_by_flow.clear();
  impl_->commit_by_attempt.clear();
  impl_->attempts.clear();
  for (std::uint32_t i = 0; i < placement_count; ++i) {
    PlacementRecord record;
    const Status status = DecodePlacementRecord(reader, &record);
    if (!status.ok()) return status;
    const std::size_t index = impl_->placements.size();
    impl_->placements.push_back(record);
    impl_->latest_by_flow[record.intent.flow] = index;
    if (record.intent.attempt.valid()) impl_->commit_by_attempt[record.intent.attempt] = index;
  }
  std::uint32_t attempt_count = 0;
  if (!reader.U32(&attempt_count)) {
    return Status(StatusCode::kStoreCorrupt, "snapshot attempt list is truncated");
  }
  if (attempt_count > options_.max_total_records) {
    return Status(StatusCode::kStoreCorrupt, "snapshot lists too many attempts");
  }
  for (std::uint32_t i = 0; i < attempt_count; ++i) {
    AttemptRecord record;
    const Status status = DecodeAttemptRecord(reader, &record);
    if (!status.ok()) return status;
    impl_->attempts[record.attempt] = record;
  }
  if (!reader.AtEnd()) return Status(StatusCode::kStoreCorrupt, "snapshot payload has trailing bytes");
  impl_->next_placement_id = next_id;
  impl_->max_sequence = max_sequence;
  return Status::Ok();
}

Status PlacementStore::LoadJournal() {
  std::FILE* file = std::fopen(path_.c_str(), "rb");
  if (file == nullptr) {
    recovery_.detail += "journal does not exist yet; starting empty";
    return Status::Ok();
  }
  struct Closer {
    std::FILE* file;
    ~Closer() { std::fclose(file); }
  } closer{file};

  const std::uint64_t total_bytes = FileSizeBytes(path_);
  const std::uint64_t snapshot_sequence = impl_->max_sequence;
  std::uint64_t offset = 0;
  std::uint64_t journal_records_read = 0;
  std::vector<std::uint8_t> header(kHeaderBytes);
  std::vector<std::uint8_t> payload;
  std::uint8_t trailer[kTrailerBytes];

  while (offset < total_bytes) {
    if (!SeekFile(file, offset).ok()) break;
    const std::size_t header_read = std::fread(header.data(), 1, kHeaderBytes, file);
    if (header_read == 0 && offset == total_bytes) break;
    if (header_read < kHeaderBytes) {
      const Status status = HandleDamage(offset, total_bytes);
      if (!status.ok()) return status;
      break;
    }
    RecordHeader parsed;
    if (!ParseHeader(header.data(), static_cast<std::uint32_t>(options_.max_record_bytes), &parsed)) {
      const Status status = HandleDamage(offset, total_bytes);
      if (!status.ok()) return status;
      break;
    }
    payload.resize(parsed.payload_length);
    if (parsed.payload_length != 0 &&
        std::fread(payload.data(), 1, parsed.payload_length, file) != parsed.payload_length) {
      const Status status = HandleDamage(offset, total_bytes);
      if (!status.ok()) return status;
      break;
    }
    if (std::fread(trailer, 1, kTrailerBytes, file) != kTrailerBytes) {
      const Status status = HandleDamage(offset, total_bytes);
      if (!status.ok()) return status;
      break;
    }
    const std::uint32_t record_crc = GetU32(trailer);
    if (GetU32(trailer + 4) != kRecordEndMagic ||
        Crc32C::Compute(payload.data(), payload.size()) != parsed.payload_crc) {
      const Status status = HandleDamage(offset, total_bytes);
      if (!status.ok()) return status;
      break;
    }
    Crc32C record_hasher;
    record_hasher.Update(header.data(), header.size());
    if (!payload.empty()) record_hasher.Update(payload.data(), payload.size());
    if (record_hasher.value() != record_crc) {
      const Status status = HandleDamage(offset, total_bytes);
      if (!status.ok()) return status;
      break;
    }

    const std::uint64_t next_offset = offset + kHeaderBytes + parsed.payload_length + kTrailerBytes;
    ++journal_records_read;
    if (parsed.type == kRecordTypeJournalReset) {
      if (parsed.payload_length != sizeof(std::uint64_t)) {
        return Status(StatusCode::kStoreCorrupt, "journal reset marker is malformed");
      }
      const std::uint64_t covered = GetU64(payload.data());
      if (covered != snapshot_sequence) {
        return Status(StatusCode::kStoreCorrupt,
                      "journal reset marker does not match the snapshot it follows");
      }
      // The journal restarted: sequences continue from the marker itself, and
      // every record after it belongs to this journal.
      impl_->max_sequence = parsed.sequence;
      offset = offset + kHeaderBytes + parsed.payload_length + kTrailerBytes;
      continue;
    }
    if (parsed.sequence <= impl_->max_sequence) {
      // Already covered by the snapshot; the record is a leftover of the
      // pre-compaction journal and is skipped deterministically.
      offset = next_offset;
      continue;
    }
    if (parsed.sequence != impl_->max_sequence + 1) {
      return Status(StatusCode::kStoreCorrupt,
                    "journal sequence is not contiguous at offset " + std::to_string(offset));
    }
    ByteReader reader(payload);
    if (parsed.type == kRecordTypePlacement) {
      PlacementRecord record;
      const Status status = DecodePlacementRecord(reader, &record);
      if (!status.ok()) return status;
      if (!reader.AtEnd()) {
        return Status(StatusCode::kStoreCorrupt, "trailing bytes after a placement record");
      }
      const std::size_t index = impl_->placements.size();
      impl_->placements.push_back(record);
      impl_->latest_by_flow[record.intent.flow] = index;
      if (record.intent.attempt.valid()) impl_->commit_by_attempt[record.intent.attempt] = index;
      if (record.id.value() >= impl_->next_placement_id) {
        impl_->next_placement_id = record.id.value() + 1;
      }
    } else {
      AttemptRecord record;
      const Status status = DecodeAttemptRecord(reader, &record);
      if (!status.ok()) return status;
      if (!reader.AtEnd()) {
        return Status(StatusCode::kStoreCorrupt, "trailing bytes after an attempt record");
      }
      const auto existing = impl_->attempts.find(record.attempt);
      if (existing != impl_->attempts.end() && existing->second.phase != AttemptPhase::kStarted &&
          record.phase == AttemptPhase::kStarted) {
        return Status(StatusCode::kStoreCorrupt,
                      "journal records an attempt start after its terminal record");
      }
      impl_->attempts[record.attempt] = record;
    }
    impl_->max_sequence = parsed.sequence;
    ++recovery_.records_valid;
    offset = next_offset;
  }
  stats_.journal_records = journal_records_read;
  return Status::Ok();
}

// Called when the journal cannot be parsed at |offset|. Distinguishes a torn
// tail (repairable, reported) from mid-file damage (fail closed) by scanning
// the remaining bytes for a later structurally valid, checksum-correct record.
Status PlacementStore::HandleDamage(std::uint64_t offset, std::uint64_t total_bytes) {
  const std::uint64_t remaining = total_bytes > offset ? total_bytes - offset : 0;
  constexpr std::uint64_t kRecordOverhead = kHeaderBytes + kTrailerBytes;
  bool valid_record_after = false;

  std::FILE* file = std::fopen(path_.c_str(), "rb");
  if (file == nullptr) return IoError("cannot reopen store for damage scan", path_);
  {
    std::vector<std::uint8_t> chunk(1u << 20);
    std::vector<std::uint8_t> header(kHeaderBytes);
    std::vector<std::uint8_t> payload;
    std::uint8_t trailer[kTrailerBytes];
    std::uint64_t scan = offset + 1;
    while (!valid_record_after && scan + kRecordOverhead <= total_bytes) {
      const std::uint64_t wanted = std::min<std::uint64_t>(chunk.size(), total_bytes - scan);
      if (!SeekFile(file, scan).ok()) break;
      const std::size_t got = std::fread(chunk.data(), 1, static_cast<std::size_t>(wanted), file);
      if (got < kHeaderBytes) break;
      for (std::size_t i = 0; i + 4 <= got; ++i) {
        if (chunk[i] != 0x46u || chunk[i + 1] != 0x50u || chunk[i + 2] != 0x52u ||
            chunk[i + 3] != 0x43u) {
          continue;
        }
        const std::uint64_t candidate = scan + i;
        if (candidate + kRecordOverhead > total_bytes) continue;
        if (!SeekFile(file, candidate).ok()) continue;
        if (std::fread(header.data(), 1, kHeaderBytes, file) != kHeaderBytes) continue;
        RecordHeader parsed;
        if (!ParseHeader(header.data(), static_cast<std::uint32_t>(options_.max_record_bytes),
                         &parsed)) {
          continue;
        }
        payload.resize(parsed.payload_length);
        if (parsed.payload_length != 0 &&
            std::fread(payload.data(), 1, parsed.payload_length, file) != parsed.payload_length) {
          continue;
        }
        if (std::fread(trailer, 1, kTrailerBytes, file) != kTrailerBytes) continue;
        if (GetU32(trailer + 4) != kRecordEndMagic) continue;
        if (Crc32C::Compute(payload.data(), payload.size()) != parsed.payload_crc) continue;
        Crc32C record_hasher;
        record_hasher.Update(header.data(), header.size());
        if (!payload.empty()) record_hasher.Update(payload.data(), payload.size());
        if (record_hasher.value() != GetU32(trailer)) continue;
        valid_record_after = true;
        break;
      }
      if (got < wanted) break;
      if (got <= 3) break;
      scan += static_cast<std::uint64_t>(got) - 3;  // overlap so a split magic is found
    }
  }
  static_cast<void>(std::fclose(file));

  if (valid_record_after) {
    return Status(StatusCode::kStoreCorrupt,
                  "journal damage at offset " + std::to_string(offset) +
                      " is followed by valid records; refusing to repair");
  }
  recovery_.truncated_tail = true;
  recovery_.bytes_discarded += remaining;
  if (options_.read_only) {
    recovery_.detail += "read-only open: torn tail reported but not repaired; ";
    return Status::Ok();
  }
  if (!options_.repair_torn_tail) {
    return Status(StatusCode::kStoreTruncatedTail,
                  "journal tail at offset " + std::to_string(offset) + " is torn");
  }
#ifdef _WIN32
  std::FILE* truncate_file = std::fopen(path_.c_str(), "r+b");
  if (truncate_file == nullptr) return IoError("cannot reopen store for truncation", path_);
  const int fd = _fileno(truncate_file);
  if (_chsize_s(fd, static_cast<long long>(offset)) != 0) {
    static_cast<void>(std::fclose(truncate_file));
    return IoError("cannot truncate torn journal tail", path_);
  }
  static_cast<void>(SyncFile(truncate_file));
  static_cast<void>(std::fclose(truncate_file));
#else
  if (truncate(path_.c_str(), static_cast<off_t>(offset)) != 0) {
    return IoError("cannot truncate torn journal tail", path_);
  }
#endif
  recovery_.detail += "torn journal tail of " + std::to_string(remaining) +
                      " bytes was discarded at offset " + std::to_string(offset) + "; ";
  return Status::Ok();
}

Status PlacementStore::AppendRecordRaw(std::uint16_t type, const std::vector<std::uint8_t>& payload) {
  if (impl_ == nullptr || impl_->closed) return Status(StatusCode::kNotRunning, "store is closed");
  if (file_ == nullptr) return Status(StatusCode::kNotRunning, "store is not open for append");
  if (payload.size() > options_.max_record_bytes) {
    return Status(StatusCode::kOversizeRequest, "record payload exceeds the configured bound");
  }
  if (impl_->max_sequence >= options_.max_total_records) {
    return Status(StatusCode::kStoreGrowthExceeded, "durable record budget is exhausted");
  }
  const std::uint64_t sequence = impl_->max_sequence + 1;

  std::vector<std::uint8_t> record;
  record.reserve(kHeaderBytes + payload.size() + kTrailerBytes);
  PutU32(&record, kRecordMagic);
  PutU32(&record, kStoreFormatVersion);
  record.push_back(static_cast<std::uint8_t>(type & 0xFFu));
  record.push_back(static_cast<std::uint8_t>((type >> 8) & 0xFFu));
  record.push_back(0);
  record.push_back(0);
  for (int i = 0; i < 8; ++i) {
    record.push_back(static_cast<std::uint8_t>((sequence >> (8 * i)) & 0xFFu));
  }
  PutU32(&record, static_cast<std::uint32_t>(payload.size()));
  PutU32(&record, Crc32C::Compute(payload.data(), payload.size()));
  record.insert(record.end(), payload.begin(), payload.end());
  PutU32(&record, Crc32C::Compute(record.data(), record.size()));
  PutU32(&record, kRecordEndMagic);

  std::FILE* file = static_cast<std::FILE*>(file_);
  if (std::fwrite(record.data(), 1, record.size(), file) != record.size()) {
    return IoError("short write to store", path_);
  }
  if (options_.fsync_on_append) {
    const Status status = SyncFile(file);
    if (!status.ok()) return status;
    ++stats_.syncs;
  } else if (std::fflush(file) != 0) {
    return IoError("fflush failed", path_);
  }
  impl_->max_sequence = sequence;
  ++stats_.appends;
  ++stats_.journal_records;
  stats_.journal_bytes += record.size();
  return Status::Ok();
}

// Bounded durable growth. Compaction rewrites the snapshot from the in-memory
// state and resets the journal, so it must only run once the caller has applied
// the record it just appended to that state.
Status PlacementStore::MaybeCompact() {
  if (impl_ == nullptr || impl_->closed || options_.read_only) return Status::Ok();
  if (impl_->compaction_in_progress) return Status::Ok();
  if (options_.max_journal_records == 0) return Status::Ok();
  if (stats_.journal_records < options_.max_journal_records) return Status::Ok();
  return Compact();
}

Status PlacementStore::AppendAttemptStart(const AttemptRecord& record) {
  if (impl_ != nullptr) {
    const auto it = impl_->attempts.find(record.attempt);
    if (it != impl_->attempts.end() && it->second.phase != AttemptPhase::kStarted) {
      // Writing a start record after a terminal record for the same attempt
      // would make the journal unreadable; it is an idempotent replay instead.
      return Status(StatusCode::kAlreadyExists,
                    "attempt already has a durable terminal record");
    }
    if (impl_->commit_by_attempt.find(record.attempt) != impl_->commit_by_attempt.end()) {
      // The attempt already committed a placement, so this is a replay of work
      // that reached its authoritative outcome.
      return Status(StatusCode::kAlreadyExists, "attempt already committed a placement");
    }
  }
  AttemptRecord copy = record;
  copy.phase = AttemptPhase::kStarted;
  // The durable sequence is assigned by the record writer; keep the field in
  // sync so that retention and ordering can rely on it.
  copy.sequence = impl_ != nullptr ? impl_->max_sequence + 1 : 0;
  ByteWriter writer;
  EncodeAttemptRecord(copy, writer);
  const Status status = AppendRecordRaw(kRecordTypeAttempt, writer.data());
  if (!status.ok()) return status;
  impl_->attempts[record.attempt] = copy;
  return MaybeCompact();
}

Status PlacementStore::AppendAttemptTerminal(const AttemptRecord& record) {
  if (record.phase == AttemptPhase::kStarted) {
    return Status(StatusCode::kInvalidArgument, "terminal attempt record must carry a final phase");
  }
  AttemptRecord existing;
  if (impl_ != nullptr) {
    const auto it = impl_->attempts.find(record.attempt);
    if (it != impl_->attempts.end() && it->second.phase != AttemptPhase::kStarted) {
      return Status(StatusCode::kAlreadyExists, "attempt already has a terminal record");
    }
  }
  AttemptRecord copy = record;
  copy.sequence = impl_ != nullptr ? impl_->max_sequence + 1 : 0;
  ByteWriter writer;
  EncodeAttemptRecord(copy, writer);
  const Status status = AppendRecordRaw(kRecordTypeAttempt, writer.data());
  if (!status.ok()) return status;
  impl_->attempts[record.attempt] = copy;
  return MaybeCompact();
}

Result<PlacementRecord> PlacementStore::CommitPlacement(const PlacementIntent& intent,
                                                       IncumbentDelta delta,
                                                       Digest decision_digest,
                                                       IncarnationId committed_by,
                                                       FabricEpoch commit_epoch) {
  if (impl_ == nullptr || impl_->closed) {
    return Status(StatusCode::kNotRunning, "store is closed");
  }
  if (!intent.flow.valid() || !intent.path.valid() || !intent.path_authority.valid()) {
    return Status(StatusCode::kInvalidArgument, "intent is missing a required identity");
  }
  if (intent.digest != IntentDigest(intent)) {
    return Status(StatusCode::kInconsistentAuthorityGeneration,
                  "intent digest does not match its contents");
  }
  // Idempotent replay: an identical attempt returns the existing record.
  if (intent.attempt.valid()) {
    const auto existing = impl_->commit_by_attempt.find(intent.attempt);
    if (existing != impl_->commit_by_attempt.end()) {
      const PlacementRecord& record = impl_->placements[existing->second];
      if (record.intent == intent) return record;
      return Status(StatusCode::kDuplicateAttemptConflict,
                    "attempt already committed a different placement intent");
    }
  }
  if (impl_->max_sequence >= options_.max_total_records) {
    return Status(StatusCode::kStoreGrowthExceeded, "durable record budget is exhausted");
  }

  PlacementRecord record;
  record.id = PlacementId{impl_->next_placement_id};
  record.intent = intent;
  record.delta = delta;
  record.decision_digest = decision_digest;
  record.committed_by = committed_by;
  record.commit_epoch = commit_epoch;
  record.sequence = impl_->max_sequence + 1;
  record.supersedes_generation = 0;
  record.commit_unix_nanos = CurrentUnixNanos();
  record.requires_revalidation = false;
  if (const PlacementRecord* previous = impl_->Latest(intent.flow)) {
    if (previous->generation.value() == std::numeric_limits<std::uint64_t>::max()) {
      return Status(StatusCode::kArithmeticOverflow,
                    "placement generation is exhausted for this flow");
    }
    record.supersedes_generation = previous->generation.value();
    record.generation = PlacementGeneration{previous->generation.value() + 1};
  } else {
    record.generation = PlacementGeneration{1};
  }

  ByteWriter writer;
  EncodePlacementRecord(record, writer);
  // Verify the encoding before it becomes durable.
  PlacementRecord decoded;
  ByteReader verify(writer.data());
  const Status verify_status = DecodePlacementRecord(verify, &decoded);
  if (!verify_status.ok() || !(decoded.intent == record.intent) ||
      decoded.id != record.id || decoded.generation != record.generation) {
    return Status(StatusCode::kInternalInvariantViolation,
                  "placement record failed encode/decode verification");
  }
  const Status status = AppendRecordRaw(kRecordTypePlacement, writer.data());
  if (!status.ok()) return status;

  const std::size_t index = impl_->placements.size();
  impl_->placements.push_back(record);
  impl_->latest_by_flow[record.intent.flow] = index;
  if (record.intent.attempt.valid()) impl_->commit_by_attempt[record.intent.attempt] = index;
  impl_->next_placement_id = record.id.value() + 1;
  const Status compacted = MaybeCompact();
  if (!compacted.ok()) return compacted;
  return record;
}

Result<std::optional<PlacementRecord>> PlacementStore::FindLatest(FlowId flow) const {
  if (impl_ == nullptr) return Status(StatusCode::kNotRunning, "store is not open");
  const PlacementRecord* record = impl_->Latest(flow);
  if (record == nullptr) return std::optional<PlacementRecord>{};
  return std::optional<PlacementRecord>{*record};
}

Result<std::vector<PlacementRecord>> PlacementStore::History(FlowId flow) const {
  if (impl_ == nullptr) return Status(StatusCode::kNotRunning, "store is not open");
  std::vector<PlacementRecord> out;
  for (const PlacementRecord& record : impl_->placements) {
    if (record.intent.flow == flow) out.push_back(record);
  }
  return out;
}

Result<std::vector<PlacementRecord>> PlacementStore::LatestPlacements() const {
  if (impl_ == nullptr) return Status(StatusCode::kNotRunning, "store is not open");
  std::vector<PlacementRecord> out;
  out.reserve(impl_->latest_by_flow.size());
  for (const auto& pair : impl_->latest_by_flow) out.push_back(impl_->placements[pair.second]);
  return out;
}

Result<std::vector<AttemptRecord>> PlacementStore::Attempts() const {
  if (impl_ == nullptr) return Status(StatusCode::kNotRunning, "store is not open");
  std::vector<AttemptRecord> out;
  out.reserve(impl_->attempts.size());
  for (const auto& pair : impl_->attempts) out.push_back(pair.second);
  std::sort(out.begin(), out.end(), [](const AttemptRecord& a, const AttemptRecord& b) {
    return a.sequence < b.sequence;
  });
  return out;
}

Result<std::optional<AttemptRecord>> PlacementStore::FindAttempt(AttemptId attempt) const {
  if (impl_ == nullptr) return Status(StatusCode::kNotRunning, "store is not open");
  const auto it = impl_->attempts.find(attempt);
  if (it == impl_->attempts.end()) return std::optional<AttemptRecord>{};
  return std::optional<AttemptRecord>{it->second};
}

Result<std::optional<PlacementRecord>> PlacementStore::FindByAttempt(AttemptId attempt) const {
  if (impl_ == nullptr) return Status(StatusCode::kNotRunning, "store is not open");
  const auto it = impl_->commit_by_attempt.find(attempt);
  if (it == impl_->commit_by_attempt.end()) return std::optional<PlacementRecord>{};
  return std::optional<PlacementRecord>{impl_->placements[it->second]};
}

Status PlacementStore::Compact() {
  if (impl_ == nullptr || impl_->closed) return Status(StatusCode::kNotRunning, "store is closed");
  if (options_.read_only) return Status(StatusCode::kNotRunning, "store is read-only");
  if (impl_->compaction_in_progress) {
    return Status(StatusCode::kAlreadyExists, "compaction is already in progress");
  }
  impl_->compaction_in_progress = true;
  struct CompactionGuard {
    Impl* impl;
    ~CompactionGuard() { impl->compaction_in_progress = false; }
  } guard{impl_};

  // The snapshot keeps the latest placement of every flow and a bounded window
  // of the most recent attempt records: attempt history that has aged out is
  // retired and reported, so durable metadata growth stays bounded.
  const std::uint64_t snapshot_covered = impl_->max_sequence;
  const std::uint64_t retention =
      options_.max_journal_records == 0 ? impl_->max_sequence : options_.max_journal_records;
  std::vector<const AttemptRecord*> retained;
  std::uint64_t retired = recovery_.retired_attempts;
  for (const auto& pair : impl_->attempts) {
    const AttemptRecord& attempt = pair.second;
    if (impl_->max_sequence > retention && attempt.sequence + retention < impl_->max_sequence) {
      ++retired;
      continue;
    }
    retained.push_back(&attempt);
  }
  std::sort(retained.begin(), retained.end(),
            [](const AttemptRecord* a, const AttemptRecord* b) { return a->attempt < b->attempt; });

  ByteWriter writer;
  writer.U64(impl_->next_placement_id);
  writer.U64(impl_->max_sequence);
  writer.U64(retired);
  writer.U32(static_cast<std::uint32_t>(impl_->latest_by_flow.size()));
  for (const auto& pair : impl_->latest_by_flow) {
    EncodePlacementRecord(impl_->placements[pair.second], writer);
  }
  writer.U32(static_cast<std::uint32_t>(retained.size()));
  for (const AttemptRecord* attempt : retained) EncodeAttemptRecord(*attempt, writer);

  std::vector<std::uint8_t> container;
  container.reserve(writer.size() + 24);
  PutU32(&container, kSnapshotMagic);
  PutU32(&container, kStoreFormatVersion);
  PutU32(&container, static_cast<std::uint32_t>(writer.size()));
  PutU32(&container, Crc32C::Compute(writer.data().data(), writer.size()));
  container.insert(container.end(), writer.data().begin(), writer.data().end());
  PutU32(&container, Crc32C::Compute(container.data(), container.size()));
  PutU32(&container, kSnapshotEndMagic);

  const std::string snapshot_path = SnapshotPathFor(path_);
  const std::string temp_path = TempPathFor(snapshot_path);
  std::FILE* temp = std::fopen(temp_path.c_str(), "wb");
  if (temp == nullptr) return IoError("cannot create snapshot temporary file", temp_path);
  if (std::fwrite(container.data(), 1, container.size(), temp) != container.size()) {
    std::fclose(temp);
    static_cast<void>(std::remove(temp_path.c_str()));
    return IoError("short write to snapshot temporary file", temp_path);
  }
  const Status sync_status = SyncFile(temp);
  std::fclose(temp);
  if (!sync_status.ok()) {
    static_cast<void>(std::remove(temp_path.c_str()));
    return sync_status;
  }
  // The snapshot is durable before the journal is replaced.
  const Status replace_status = ReplaceFileAtomically(temp_path, snapshot_path);
  if (!replace_status.ok()) {
    static_cast<void>(std::remove(temp_path.c_str()));
    return replace_status;
  }

  // Reset the journal: durable empty replacement, then reopen for append.
  if (file_ != nullptr) {
    static_cast<void>(std::fclose(static_cast<std::FILE*>(file_)));
    file_ = nullptr;
  }
  const std::string journal_temp = TempPathFor(path_);
  std::FILE* empty = std::fopen(journal_temp.c_str(), "wb");
  if (empty == nullptr) return IoError("cannot create journal temporary file", journal_temp);
  const Status empty_sync = SyncFile(empty);
  std::fclose(empty);
  if (!empty_sync.ok()) return empty_sync;
  const Status journal_replace = ReplaceFileAtomically(journal_temp, path_);
  if (!journal_replace.ok()) return journal_replace;
  std::FILE* reopened = std::fopen(path_.c_str(), "ab");
  if (reopened == nullptr) return IoError("cannot reopen journal after compaction", path_);
  file_ = reopened;
  // The snapshot is durable and the journal has been replaced. The marker makes
  // the sequence rebase explicit and crash-safe: without it a journal that was
  // replaced early would look like a stale pre-compaction journal.
  {
    std::vector<std::uint8_t> marker;
    for (int i = 0; i < 8; ++i) {
      marker.push_back(static_cast<std::uint8_t>((snapshot_covered >> (8 * i)) & 0xFFu));
    }
    impl_->max_sequence = 0;
    stats_.journal_records = 0;
    const Status marker_status = AppendRecordRaw(kRecordTypeJournalReset, marker);
    if (!marker_status.ok()) return marker_status;
    impl_->max_sequence = 1;  // the marker owns sequence 1 of the new journal
  }
  ++stats_.compactions;
  stats_.journal_records = 0;
  stats_.journal_bytes = 0;
  stats_.snapshot_bytes = container.size();
  return Status::Ok();
}

Status PlacementStore::Sync() {
  if (impl_ == nullptr || impl_->closed) return Status(StatusCode::kNotRunning, "store is closed");
  if (file_ == nullptr) return Status(StatusCode::kNotRunning, "store is not open for append");
  const Status status = SyncFile(static_cast<std::FILE*>(file_));
  if (status.ok()) ++stats_.syncs;
  return status;
}

Status PlacementStore::Close() {
  if (impl_ != nullptr) impl_->closed = true;
  if (file_ == nullptr) return Status::Ok();
  std::FILE* file = static_cast<std::FILE*>(file_);
  file_ = nullptr;
  const Status sync_status = options_.read_only ? Status::Ok() : SyncFile(file);
  const int close_result = std::fclose(file);
  if (!sync_status.ok()) return sync_status;
  if (close_result != 0) return IoError("fclose failed", path_);
  return Status::Ok();
}

}  // namespace flowplace

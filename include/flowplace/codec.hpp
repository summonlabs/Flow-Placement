// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Canonical binary codecs shared by the durable store and the wire protocol.
// Decoding is bounded and strict: unknown enum values, truncated payloads,
// oversized collections, and trailing bytes are all rejected.

#ifndef FLOWPLACE_CODEC_HPP
#define FLOWPLACE_CODEC_HPP

#include "flowplace/bytes.hpp"
#include "flowplace/explain.hpp"
#include "flowplace/model.hpp"
#include "flowplace/store.hpp"

namespace flowplace {

// ---- model ---------------------------------------------------------------
void EncodeAuthorityExpectation(const AuthorityExpectation& v, ByteWriter& w);
Status DecodeAuthorityExpectation(ByteReader& r, AuthorityExpectation* out);

void EncodeAuthorityVector(const AuthorityVector& v, ByteWriter& w);
Status DecodeAuthorityVector(ByteReader& r, AuthorityVector* out);

void EncodeRequest(const PlacementRequest& v, ByteWriter& w);
Status DecodeRequest(ByteReader& r, const Limits& limits, PlacementRequest* out);

void EncodeIntent(const PlacementIntent& v, ByteWriter& w);
Status DecodeIntent(ByteReader& r, PlacementIntent* out);

// ---- decisions -----------------------------------------------------------
void EncodeDecision(const PlacementDecision& v, ByteWriter& w);
Status DecodeDecision(ByteReader& r, const Limits& limits, PlacementDecision* out);

// ---- durable records -----------------------------------------------------
void EncodeAttemptRecord(const AttemptRecord& v, ByteWriter& w);
Status DecodeAttemptRecord(ByteReader& r, AttemptRecord* out);

void EncodePlacementRecord(const PlacementRecord& v, ByteWriter& w);
Status DecodePlacementRecord(ByteReader& r, PlacementRecord* out);

// ---- runtime -------------------------------------------------------------
void EncodeRecoveryReport(const RecoveryReport& v, ByteWriter& w);
Status DecodeRecoveryReport(ByteReader& r, RecoveryReport* out);

}  // namespace flowplace

#endif  // FLOWPLACE_CODEC_HPP

// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "fsl/bytes.hpp"
#include "fsl/event.hpp"
#include "fsl/hash.hpp"
#include "fsl/status.hpp"
#include "fsl/subject.hpp"

/// \file codec.hpp
/// Canonical byte encoding of submissions, envelopes and the key material of
/// derived index postings.
///
/// One encoding function per concept lives here so that the bytes hashed for a
/// content digest, the bytes written into a segment frame, and the bytes used as
/// an index key can never drift apart.

namespace fsl::detail {

inline constexpr std::uint32_t kEnvelopeVersion = 1;
inline constexpr std::uint32_t kSubmissionVersion = 1;

/// Appends the canonical encoding of a subject reference.
void encode_subject_ref(ByteWriter& writer, const SubjectRef& reference);
/// Decodes a subject reference. Rejects unknown kinds and invalid keys.
[[nodiscard]] bool decode_subject_ref(ByteReader& reader, std::optional<SubjectRef>& out);

/// Appends the canonical encoding of a provenance record.
void encode_provenance(ByteWriter& writer, const ProvenanceRecord& provenance);
[[nodiscard]] Result<ProvenanceRecord> decode_provenance(ByteReader& reader);

/// Canonical bytes of the caller-controlled portion of a submission, excluding
/// the retry token: two submissions that differ only in their idempotency token
/// are the same content.
[[nodiscard]] std::vector<std::uint8_t> encode_submission(const SubmittedObservation& observation);

/// Body of a committed event frame.
[[nodiscard]] std::vector<std::uint8_t> encode_envelope(const EventEnvelope& envelope);
[[nodiscard]] Result<EventEnvelope> decode_envelope(std::span<const std::uint8_t> body);

/// Index key for a subject posting.
[[nodiscard]] std::vector<std::uint8_t> subject_key(const SubjectRef& reference);
/// Index key for a relationship posting: subject, kind, related subject.
[[nodiscard]] std::vector<std::uint8_t> relationship_key(const SubjectRef& subject,
                                                        RelationshipKind kind,
                                                        const SubjectRef& related);
/// Decodes a relationship key back into its three components.
[[nodiscard]] bool decode_relationship_key(std::span<const std::uint8_t> key,
                                          std::optional<SubjectRef>& subject,
                                          RelationshipKind& kind,
                                          std::optional<SubjectRef>& related);

}  // namespace fsl::detail

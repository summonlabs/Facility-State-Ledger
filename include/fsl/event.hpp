// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "fsl/counter.hpp"
#include "fsl/hash.hpp"
#include "fsl/ids.hpp"
#include "fsl/provenance.hpp"
#include "fsl/status.hpp"
#include "fsl/subject.hpp"

/// \file event.hpp
/// The distinction between what a producer submits and what the ledger accepts.
///
/// A SubmittedObservation is an untrusted request: it carries no ledger
/// sequence, no logical tick and no acceptance time, because those are assigned
/// by the ledger. An EventEnvelope is an immutable record of an event that
/// crossed the commit boundary; it is only ever produced by the ledger and can
/// never be constructed by a consumer.

namespace fsl {

/// Kind of authoritative facility-state transition recorded by the ledger.
///
/// The ledger enforces the lifecycle of these kinds against its own subject
/// registry. It does not judge whether the *content* of an upstream transition
/// is correct for the facility; that belongs to the producing component.
enum class EventKind : std::uint8_t {
  /// Records the creation of the ledger itself. Always sequence 1.
  kLedgerOpened = 1,
  /// Opens a facility epoch. No other kind is admitted until an epoch is open.
  kEpochOpened = 2,
  /// Closes the open epoch.
  kEpochClosed = 3,
  /// Advances the authoritative facility generation. Only while no epoch is open.
  kGenerationAdvanced = 4,
  /// Registers a subject in the facility model.
  kSubjectRegistered = 5,
  /// Records an observation from a producer that the ledger accepted.
  kObservationAccepted = 6,
  /// Records an authoritative mutation of a registered subject.
  kSubjectMutated = 7,
  /// Retires a registered subject. Retired subjects accept no further mutation.
  kSubjectRetired = 8,
  /// Asserts a structural relationship between two known subjects.
  kRelationshipAsserted = 9,
  /// Retracts a previously asserted relationship.
  kRelationshipRetracted = 10,
  /// Records that a producer reconciled divergent upstream views.
  kReconciliationRecorded = 11,
  /// Records a correction that references an earlier committed event. The
  /// referenced event is never modified; the correction is additive.
  kCorrectionRecorded = 12,
  /// Records an accepted change to the ledger's own admission policy.
  kPolicyAttested = 13,
};

[[nodiscard]] std::string_view to_string(EventKind kind) noexcept;
[[nodiscard]] std::optional<EventKind> event_kind_from_string(std::string_view name) noexcept;

/// True when the kind is admitted only while no facility epoch is open.
[[nodiscard]] bool event_kind_is_epoch_control(EventKind kind) noexcept;

/// Ledger-assigned acceptance position of a committed event.
///
/// `logical_tick` is a strict ledger-global ordinal: it increases by exactly one
/// per accepted event. `monotonic_nanoseconds` comes from a monotonic clock and
/// is meaningful only relative to other values from the same process
/// incarnation. Neither field is a wall-clock timestamp and neither is used for
/// ordering: the sole public ordering guarantee is ascending LedgerSequence.
struct AcceptedAt {
  LogicalTick logical_tick;
  std::uint64_t monotonic_nanoseconds = 0;

  [[nodiscard]] friend bool operator==(const AcceptedAt&, const AcceptedAt&) noexcept = default;
};

/// Immutable record of an event that crossed the ledger commit boundary.
class EventEnvelope {
 public:
  EventEnvelope(LedgerSequence sequence,
                EventId event_id,
                std::optional<IdempotencyToken> idempotency_token,
                FacilityGeneration facility_generation,
                std::optional<FacilityEpoch> epoch,
                EventKind kind,
                SubjectRef subject,
                std::optional<EventId> correction_target,
                SchemaId payload_schema,
                SchemaVersion payload_schema_version,
                std::vector<std::uint8_t> payload,
                ProvenanceRecord provenance,
                AcceptedAt accepted_at,
                Digest content_digest,
                Digest integrity)
      : sequence_(sequence),
        event_id_(event_id),
        idempotency_token_(std::move(idempotency_token)),
        facility_generation_(facility_generation),
        epoch_(epoch),
        kind_(kind),
        subject_(std::move(subject)),
        correction_target_(correction_target),
        payload_schema_(std::move(payload_schema)),
        payload_schema_version_(payload_schema_version),
        payload_(std::move(payload)),
        provenance_(std::move(provenance)),
        accepted_at_(accepted_at),
        content_digest_(content_digest),
        integrity_(integrity) {}

  [[nodiscard]] LedgerSequence sequence() const noexcept { return sequence_; }
  [[nodiscard]] const EventId& event_id() const noexcept { return event_id_; }
  /// Retry token supplied by the producer, when one was. Persisted with the
  /// event so that duplicate detection by token survives a rebuild of derived
  /// state.
  [[nodiscard]] const std::optional<IdempotencyToken>& idempotency_token() const noexcept {
    return idempotency_token_;
  }
  [[nodiscard]] const FacilityGeneration& facility_generation() const noexcept { return facility_generation_; }
  /// The epoch the event was admitted in. Absent only for kLedgerOpened, which by
  /// construction precedes the first epoch.
  [[nodiscard]] const std::optional<FacilityEpoch>& epoch() const noexcept { return epoch_; }
  [[nodiscard]] EventKind kind() const noexcept { return kind_; }
  [[nodiscard]] const SubjectRef& subject() const noexcept { return subject_; }
  [[nodiscard]] const std::optional<EventId>& correction_target() const noexcept { return correction_target_; }
  [[nodiscard]] const SchemaId& payload_schema() const noexcept { return payload_schema_; }
  [[nodiscard]] const SchemaVersion& payload_schema_version() const noexcept { return payload_schema_version_; }
  [[nodiscard]] const std::vector<std::uint8_t>& payload() const noexcept { return payload_; }
  [[nodiscard]] const ProvenanceRecord& provenance() const noexcept { return provenance_; }
  [[nodiscard]] const AcceptedAt& accepted_at() const noexcept { return accepted_at_; }
  /// SHA-256 over the canonical bytes of the caller-supplied submission. Stable
  /// for equal submissions regardless of when they were accepted.
  [[nodiscard]] const Digest& content_digest() const noexcept { return content_digest_; }
  /// SHA-256 over the canonical bytes of this envelope, ledger-assigned fields
  /// included. This is the per-event integrity checksum.
  [[nodiscard]] const Digest& integrity() const noexcept { return integrity_; }

 private:
  LedgerSequence sequence_;
  EventId event_id_;
  std::optional<IdempotencyToken> idempotency_token_;
  FacilityGeneration facility_generation_;
  std::optional<FacilityEpoch> epoch_;
  EventKind kind_;
  SubjectRef subject_;
  std::optional<EventId> correction_target_;
  SchemaId payload_schema_;
  SchemaVersion payload_schema_version_;
  std::vector<std::uint8_t> payload_;
  ProvenanceRecord provenance_;
  AcceptedAt accepted_at_;
  Digest content_digest_;
  Digest integrity_;
};

/// Untrusted, unvalidated request to append one event.
///
/// Every field is optional so that a caller (or a malformed imported document)
/// can present a partial submission; `SubmittedObservation::create` reports the
/// first missing or invalid field with a stable error code instead of guessing.
struct SubmittedObservationFields {
  std::optional<EventId> event_id;
  std::optional<IdempotencyToken> idempotency_token;
  std::optional<FacilityGeneration> facility_generation;
  std::optional<FacilityEpoch> epoch;
  std::optional<EventKind> kind;
  std::optional<SubjectRef> subject;
  std::optional<EventId> correction_target;
  std::optional<SchemaId> payload_schema;
  std::optional<SchemaVersion> payload_schema_version;
  std::vector<std::uint8_t> payload;
  std::optional<ProvenanceRecord> provenance;
};

/// A validated submission: every field the ledger needs is present and in range.
class SubmittedObservation {
 public:
  [[nodiscard]] static Result<SubmittedObservation> create(SubmittedObservationFields fields);

  [[nodiscard]] const EventId& event_id() const noexcept { return event_id_; }
  [[nodiscard]] const std::optional<IdempotencyToken>& idempotency_token() const noexcept {
    return idempotency_token_;
  }
  [[nodiscard]] const FacilityGeneration& facility_generation() const noexcept { return facility_generation_; }
  /// The epoch the submission targets. Required for every kind except
  /// kLedgerOpened, which is recorded before any epoch exists.
  [[nodiscard]] const std::optional<FacilityEpoch>& epoch() const noexcept { return epoch_; }
  [[nodiscard]] EventKind kind() const noexcept { return kind_; }
  [[nodiscard]] const SubjectRef& subject() const noexcept { return subject_; }
  [[nodiscard]] const std::optional<EventId>& correction_target() const noexcept { return correction_target_; }
  [[nodiscard]] const SchemaId& payload_schema() const noexcept { return payload_schema_; }
  [[nodiscard]] const SchemaVersion& payload_schema_version() const noexcept { return payload_schema_version_; }
  [[nodiscard]] const std::vector<std::uint8_t>& payload() const noexcept { return payload_; }
  [[nodiscard]] const ProvenanceRecord& provenance() const noexcept { return provenance_; }

  /// SHA-256 over the canonical submission bytes. Two submissions that agree on
  /// every caller-controlled field have equal content digests.
  [[nodiscard]] const Digest& content_digest() const noexcept { return content_digest_; }

  /// Canonical byte encoding of the caller-controlled portion of a submission.
  /// Deterministic: fixed field order, little-endian fixed-width integers,
  /// length-prefixed byte strings.
  [[nodiscard]] std::vector<std::uint8_t> canonical_submission_bytes() const;

 private:
  SubmittedObservation(EventId event_id,
                       std::optional<IdempotencyToken> idempotency_token,
                       FacilityGeneration facility_generation,
                       std::optional<FacilityEpoch> epoch,
                       EventKind kind,
                       SubjectRef subject,
                       std::optional<EventId> correction_target,
                       SchemaId payload_schema,
                       SchemaVersion payload_schema_version,
                       std::vector<std::uint8_t> payload,
                       ProvenanceRecord provenance,
                       Digest content_digest)
      : event_id_(event_id),
        idempotency_token_(std::move(idempotency_token)),
        facility_generation_(facility_generation),
        epoch_(epoch),
        kind_(kind),
        subject_(std::move(subject)),
        correction_target_(correction_target),
        payload_schema_(std::move(payload_schema)),
        payload_schema_version_(payload_schema_version),
        payload_(std::move(payload)),
        provenance_(std::move(provenance)),
        content_digest_(content_digest) {}

  EventId event_id_;
  std::optional<IdempotencyToken> idempotency_token_;
  FacilityGeneration facility_generation_;
  std::optional<FacilityEpoch> epoch_;
  EventKind kind_;
  SubjectRef subject_;
  std::optional<EventId> correction_target_;
  SchemaId payload_schema_;
  SchemaVersion payload_schema_version_;
  std::vector<std::uint8_t> payload_;
  ProvenanceRecord provenance_;
  Digest content_digest_;
};

}  // namespace fsl

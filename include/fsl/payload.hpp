// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fsl/counter.hpp"
#include "fsl/event.hpp"
#include "fsl/ids.hpp"
#include "fsl/status.hpp"
#include "fsl/subject.hpp"

/// \file payload.hpp
/// Canonical payload schemas for the event kinds this ledger admits.
///
/// A payload is a versioned, schema-identified byte string. The ledger decodes
/// and validates the payload of every event it records, because referential
/// integrity (does the referenced subject exist? does the relationship name two
/// known endpoints?) is part of admitting an event. It does not interpret the
/// operational meaning of the content: a rack's rated capacity or a link's
/// bandwidth belong to the components that own those semantics.
///
/// Encoders and decoders are exact inverses. Decoding rejects a schema version
/// this build does not implement, unknown enum values, out-of-range lengths, and
/// trailing bytes.

namespace fsl::payload {

/// Schema identifiers, one per event kind.
[[nodiscard]] SchemaId ledger_opened_schema() noexcept;
[[nodiscard]] SchemaId epoch_opened_schema() noexcept;
[[nodiscard]] SchemaId epoch_closed_schema() noexcept;
[[nodiscard]] SchemaId generation_advanced_schema() noexcept;
[[nodiscard]] SchemaId subject_registered_schema() noexcept;
[[nodiscard]] SchemaId observation_accepted_schema() noexcept;
[[nodiscard]] SchemaId subject_mutated_schema() noexcept;
[[nodiscard]] SchemaId subject_retired_schema() noexcept;
[[nodiscard]] SchemaId relationship_schema() noexcept;
[[nodiscard]] SchemaId reconciliation_recorded_schema() noexcept;
[[nodiscard]] SchemaId correction_recorded_schema() noexcept;
[[nodiscard]] SchemaId policy_attested_schema() noexcept;

/// The schema the ledger requires for `kind`.
[[nodiscard]] SchemaId required_schema(EventKind kind) noexcept;
/// Current schema version the ledger writes and accepts for `kind`.
[[nodiscard]] SchemaVersion required_schema_version(EventKind kind) noexcept;

/// Payload of kLedgerOpened. Always the first committed event of a ledger.
struct LedgerOpened {
  LedgerId ledger_id;
  std::uint64_t segment_format_version = 0;
  std::uint64_t manifest_generation = 0;
};
[[nodiscard]] std::vector<std::uint8_t> encode(const LedgerOpened& value);
[[nodiscard]] Result<LedgerOpened> decode_ledger_opened(std::span<const std::uint8_t> bytes,
                                                        SchemaVersion version);

/// Payload of kEpochOpened.
struct EpochOpened {
  FacilityEpoch epoch{1};
  FacilityGeneration generation{1};
};
[[nodiscard]] std::vector<std::uint8_t> encode(const EpochOpened& value);
[[nodiscard]] Result<EpochOpened> decode_epoch_opened(std::span<const std::uint8_t> bytes, SchemaVersion version);

/// Payload of kEpochClosed.
struct EpochClosed {
  FacilityEpoch epoch{1};
};
[[nodiscard]] std::vector<std::uint8_t> encode(const EpochClosed& value);
[[nodiscard]] Result<EpochClosed> decode_epoch_closed(std::span<const std::uint8_t> bytes, SchemaVersion version);

/// Payload of kGenerationAdvanced.
struct GenerationAdvanced {
  FacilityGeneration from{1};
  FacilityGeneration to{1};
};
[[nodiscard]] std::vector<std::uint8_t> encode(const GenerationAdvanced& value);
[[nodiscard]] Result<GenerationAdvanced> decode_generation_advanced(std::span<const std::uint8_t> bytes,
                                                                   SchemaVersion version);

/// Payload of kSubjectRegistered.
struct SubjectRegistered {
  SubjectRef subject;
  /// Required for physical subject kinds (see subject_kind_requires_location).
  std::optional<SubjectRef> location;
};
[[nodiscard]] std::vector<std::uint8_t> encode(const SubjectRegistered& value);
[[nodiscard]] Result<SubjectRegistered> decode_subject_registered(std::span<const std::uint8_t> bytes,
                                                                  SchemaVersion version);

/// Payload of kObservationAccepted.
struct ObservationAccepted {
  std::string observation_ref;
  std::vector<std::uint8_t> body;
};
[[nodiscard]] std::vector<std::uint8_t> encode(const ObservationAccepted& value);
[[nodiscard]] Result<ObservationAccepted> decode_observation_accepted(std::span<const std::uint8_t> bytes,
                                                                     SchemaVersion version);

/// Payload of kSubjectMutated.
struct SubjectMutated {
  std::string mutation_ref;
  std::vector<std::uint8_t> body;
};
[[nodiscard]] std::vector<std::uint8_t> encode(const SubjectMutated& value);
[[nodiscard]] Result<SubjectMutated> decode_subject_mutated(std::span<const std::uint8_t> bytes,
                                                            SchemaVersion version);

/// Payload of kSubjectRetired.
struct SubjectRetired {
  std::string reason;
};
[[nodiscard]] std::vector<std::uint8_t> encode(const SubjectRetired& value);
[[nodiscard]] Result<SubjectRetired> decode_subject_retired(std::span<const std::uint8_t> bytes,
                                                            SchemaVersion version);

/// Payload of kRelationshipAsserted and kRelationshipRetracted.
struct Relationship {
  SubjectRef related;
  RelationshipKind kind = RelationshipKind::kContainedIn;
};
[[nodiscard]] std::vector<std::uint8_t> encode(const Relationship& value);
[[nodiscard]] Result<Relationship> decode_relationship(std::span<const std::uint8_t> bytes, SchemaVersion version);

/// Payload of kReconciliationRecorded.
struct ReconciliationRecorded {
  std::string reconciliation_ref;
  std::vector<std::uint8_t> detail;
};
[[nodiscard]] std::vector<std::uint8_t> encode(const ReconciliationRecorded& value);
[[nodiscard]] Result<ReconciliationRecorded> decode_reconciliation_recorded(std::span<const std::uint8_t> bytes,
                                                                            SchemaVersion version);

/// Payload of kCorrectionRecorded.
struct CorrectionRecorded {
  std::string rationale;
};
[[nodiscard]] std::vector<std::uint8_t> encode(const CorrectionRecorded& value);
[[nodiscard]] Result<CorrectionRecorded> decode_correction_recorded(std::span<const std::uint8_t> bytes,
                                                                    SchemaVersion version);

/// Payload of kPolicyAttested.
struct PolicyAttested {
  std::string policy_ref;
  std::vector<std::uint8_t> detail;
};
[[nodiscard]] std::vector<std::uint8_t> encode(const PolicyAttested& value);
[[nodiscard]] Result<PolicyAttested> decode_policy_attested(std::span<const std::uint8_t> bytes,
                                                            SchemaVersion version);

/// Decodes and validates the payload of any admitted kind.
///
/// Fails with kUnsupportedPayloadSchema when `schema` is not the schema required
/// for `kind` or `version` is not the implemented version, and with
/// kMalformedInput for a payload that does not decode exactly.
[[nodiscard]] Status validate(EventKind kind,
                              const SchemaId& schema,
                              SchemaVersion version,
                              std::span<const std::uint8_t> bytes);

/// Bounds applied to every decoded payload field.
struct Limits {
  static constexpr std::uint32_t kMaxReferenceLength = 256;
  static constexpr std::uint32_t kMaxBodyBytes = 1u * 1024u * 1024u;
  static constexpr std::uint32_t kMaxReasonLength = 512;
};

}  // namespace fsl::payload

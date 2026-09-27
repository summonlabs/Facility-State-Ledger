// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fsl/payload.hpp"

#include "detail/codec.hpp"
#include "fsl/bytes.hpp"
#include "fsl/text.hpp"

namespace fsl::payload {
namespace {

constexpr std::uint32_t kCurrentSchemaVersion = 1;

[[nodiscard]] SchemaId schema(std::string_view text) { return SchemaId::parse(text).value(); }

[[nodiscard]] bool decode_subject(ByteReader& reader, std::optional<SubjectRef>& out) {
  return detail::decode_subject_ref(reader, out);
}

[[nodiscard]] Status require_version(SchemaVersion version, std::string_view what) {
  if (version.value() != kCurrentSchemaVersion) {
    return unsupported_version(ErrorCode::kUnsupportedPayloadSchema,
                               std::string(what) + " schema version " + std::to_string(version.value()) +
                                   " is not implemented; this build writes and accepts version " +
                                   std::to_string(kCurrentSchemaVersion));
  }
  return Status::ok();
}

[[nodiscard]] Status require_end(const ByteReader& reader, std::string_view what) {
  if (!reader.at_end()) {
    return malformed_input(ErrorCode::kUnexpectedTrailingBytes,
                           std::string(what) + " payload has trailing bytes");
  }
  return Status::ok();
}

[[nodiscard]] Status truncated(std::string_view what) {
  return malformed_input(ErrorCode::kTruncatedInput, std::string(what) + " payload is truncated");
}

[[nodiscard]] Status validate_reference_text(std::string_view text, std::string_view field) {
  if (text.empty()) {
    return invalid_argument(ErrorCode::kValueEmpty, std::string(field) + " is empty");
  }
  if (text.size() > Limits::kMaxReferenceLength) {
    return invalid_argument(ErrorCode::kValueTooLong,
                            std::string(field) + " is longer than " +
                                std::to_string(Limits::kMaxReferenceLength) + " characters");
  }
  if (!is_valid_identifier(text, Limits::kMaxReferenceLength)) {
    return invalid_argument(ErrorCode::kSyntaxInvalid,
                            std::string(field) + " must be lower-case [a-z0-9._-]: '" + std::string(text) +
                                "'");
  }
  return Status::ok();
}

[[nodiscard]] Status validate_body(std::span<const std::uint8_t> body, std::string_view field) {
  if (body.size() > Limits::kMaxBodyBytes) {
    return capacity_exceeded(ErrorCode::kSizeOutOfRange,
                             std::string(field) + " is larger than " +
                                 std::to_string(Limits::kMaxBodyBytes) + " bytes");
  }
  return Status::ok();
}

}  // namespace

SchemaId ledger_opened_schema() noexcept { return schema("fsl.ledger-opened.v1"); }
SchemaId epoch_opened_schema() noexcept { return schema("fsl.epoch-opened.v1"); }
SchemaId epoch_closed_schema() noexcept { return schema("fsl.epoch-closed.v1"); }
SchemaId generation_advanced_schema() noexcept { return schema("fsl.generation-advanced.v1"); }
SchemaId subject_registered_schema() noexcept { return schema("fsl.subject-registered.v1"); }
SchemaId observation_accepted_schema() noexcept { return schema("fsl.observation-accepted.v1"); }
SchemaId subject_mutated_schema() noexcept { return schema("fsl.subject-mutated.v1"); }
SchemaId subject_retired_schema() noexcept { return schema("fsl.subject-retired.v1"); }
SchemaId relationship_schema() noexcept { return schema("fsl.relationship.v1"); }
SchemaId reconciliation_recorded_schema() noexcept { return schema("fsl.reconciliation-recorded.v1"); }
SchemaId correction_recorded_schema() noexcept { return schema("fsl.correction-recorded.v1"); }
SchemaId policy_attested_schema() noexcept { return schema("fsl.policy-attested.v1"); }

SchemaId required_schema(EventKind kind) noexcept {
  switch (kind) {
    case EventKind::kLedgerOpened:
      return ledger_opened_schema();
    case EventKind::kEpochOpened:
      return epoch_opened_schema();
    case EventKind::kEpochClosed:
      return epoch_closed_schema();
    case EventKind::kGenerationAdvanced:
      return generation_advanced_schema();
    case EventKind::kSubjectRegistered:
      return subject_registered_schema();
    case EventKind::kObservationAccepted:
      return observation_accepted_schema();
    case EventKind::kSubjectMutated:
      return subject_mutated_schema();
    case EventKind::kSubjectRetired:
      return subject_retired_schema();
    case EventKind::kRelationshipAsserted:
    case EventKind::kRelationshipRetracted:
      return relationship_schema();
    case EventKind::kReconciliationRecorded:
      return reconciliation_recorded_schema();
    case EventKind::kCorrectionRecorded:
      return correction_recorded_schema();
    case EventKind::kPolicyAttested:
      return policy_attested_schema();
  }
  return ledger_opened_schema();
}

SchemaVersion required_schema_version(EventKind /*kind*/) noexcept {
  return SchemaVersion(kCurrentSchemaVersion);
}

// -- Ledger opened ------------------------------------------------------------

std::vector<std::uint8_t> encode(const LedgerOpened& value) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  writer.id128(value.ledger_id);
  writer.u64(value.segment_format_version);
  writer.u64(value.manifest_generation);
  return out;
}

Result<LedgerOpened> decode_ledger_opened(std::span<const std::uint8_t> bytes, SchemaVersion version) {
  if (Status status = require_version(version, "ledger-opened"); status.is_error()) {
    return status;
  }
  ByteReader reader(bytes);
  LedgerOpened value;
  if (!reader.id128(value.ledger_id) || !reader.u64(value.segment_format_version) ||
      !reader.u64(value.manifest_generation)) {
    return truncated("ledger-opened");
  }
  if (Status status = require_end(reader, "ledger-opened"); status.is_error()) {
    return status;
  }
  if (value.ledger_id.is_nil()) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "ledger-opened payload carries a nil ledger id");
  }
  return value;
}

// -- Epochs -------------------------------------------------------------------

std::vector<std::uint8_t> encode(const EpochOpened& value) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  writer.u64(value.epoch.value());
  writer.u64(value.generation.value());
  return out;
}

Result<EpochOpened> decode_epoch_opened(std::span<const std::uint8_t> bytes, SchemaVersion version) {
  if (Status status = require_version(version, "epoch-opened"); status.is_error()) {
    return status;
  }
  ByteReader reader(bytes);
  std::uint64_t epoch = 0;
  std::uint64_t generation = 0;
  if (!reader.u64(epoch) || !reader.u64(generation)) {
    return truncated("epoch-opened");
  }
  if (Status status = require_end(reader, "epoch-opened"); status.is_error()) {
    return status;
  }
  if (epoch == 0 || generation == 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid,
                           "epoch-opened payload carries a zero epoch or generation");
  }
  return EpochOpened{FacilityEpoch(epoch), FacilityGeneration(generation)};
}

std::vector<std::uint8_t> encode(const EpochClosed& value) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  writer.u64(value.epoch.value());
  return out;
}

Result<EpochClosed> decode_epoch_closed(std::span<const std::uint8_t> bytes, SchemaVersion version) {
  if (Status status = require_version(version, "epoch-closed"); status.is_error()) {
    return status;
  }
  ByteReader reader(bytes);
  std::uint64_t epoch = 0;
  if (!reader.u64(epoch)) {
    return truncated("epoch-closed");
  }
  if (Status status = require_end(reader, "epoch-closed"); status.is_error()) {
    return status;
  }
  if (epoch == 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "epoch-closed payload carries a zero epoch");
  }
  return EpochClosed{FacilityEpoch(epoch)};
}

std::vector<std::uint8_t> encode(const GenerationAdvanced& value) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  writer.u64(value.from.value());
  writer.u64(value.to.value());
  return out;
}

Result<GenerationAdvanced> decode_generation_advanced(std::span<const std::uint8_t> bytes,
                                                      SchemaVersion version) {
  if (Status status = require_version(version, "generation-advanced"); status.is_error()) {
    return status;
  }
  ByteReader reader(bytes);
  std::uint64_t from = 0;
  std::uint64_t to = 0;
  if (!reader.u64(from) || !reader.u64(to)) {
    return truncated("generation-advanced");
  }
  if (Status status = require_end(reader, "generation-advanced"); status.is_error()) {
    return status;
  }
  if (from == 0 || to == 0 || to != from + 1) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid,
                           "generation-advanced must advance by exactly one generation");
  }
  return GenerationAdvanced{FacilityGeneration(from), FacilityGeneration(to)};
}

// -- Subjects -----------------------------------------------------------------

std::vector<std::uint8_t> encode(const SubjectRegistered& value) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  detail::encode_subject_ref(writer, value.subject);
  writer.boolean(value.location.has_value());
  if (value.location.has_value()) {
    detail::encode_subject_ref(writer, *value.location);
  }
  return out;
}

Result<SubjectRegistered> decode_subject_registered(std::span<const std::uint8_t> bytes,
                                                    SchemaVersion version) {
  if (Status status = require_version(version, "subject-registered"); status.is_error()) {
    return status;
  }
  ByteReader reader(bytes);
  std::optional<SubjectRef> subject;
  bool has_location = false;
  if (!decode_subject(reader, subject) || !reader.boolean(has_location)) {
    return truncated("subject-registered");
  }
  std::optional<SubjectRef> location;
  if (has_location && !decode_subject(reader, location)) {
    return malformed_input(ErrorCode::kInvalidEnumValue,
                           "subject-registered payload carries an invalid location reference");
  }
  if (Status status = require_end(reader, "subject-registered"); status.is_error()) {
    return status;
  }
  if (location.has_value() && location->kind() != SubjectKind::kLocation) {
    return malformed_input(ErrorCode::kSubjectKindMismatch,
                           "subject-registered location must be a location subject reference");
  }
  return SubjectRegistered{std::move(subject).value(), std::move(location)};
}

// -- Observations and mutations -----------------------------------------------

namespace {

[[nodiscard]] std::vector<std::uint8_t> encode_reference_body(std::string_view reference,
                                                              std::span<const std::uint8_t> body) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  writer.sized_string(reference);
  writer.sized_bytes(body);
  return out;
}

[[nodiscard]] Result<std::pair<std::string, std::vector<std::uint8_t>>> decode_reference_body(
    std::span<const std::uint8_t> bytes,
    SchemaVersion version,
    std::string_view what,
    std::string_view field) {
  if (Status status = require_version(version, what); status.is_error()) {
    return status;
  }
  ByteReader reader(bytes);
  std::string reference;
  if (!reader.sized_string(reference, Limits::kMaxReferenceLength)) {
    return malformed_input(ErrorCode::kSizeOutOfRange, std::string(field) + " is malformed or too long");
  }
  std::span<const std::uint8_t> body;
  if (!reader.sized_bytes(body, Limits::kMaxBodyBytes)) {
    return malformed_input(ErrorCode::kSizeOutOfRange, std::string(what) + " body is malformed or too long");
  }
  if (Status status = require_end(reader, what); status.is_error()) {
    return status;
  }
  if (Status status = validate_reference_text(reference, field); status.is_error()) {
    return status;
  }
  if (Status status = validate_body(body, what); status.is_error()) {
    return status;
  }
  return std::make_pair(std::move(reference), std::vector<std::uint8_t>(body.begin(), body.end()));
}

}  // namespace

std::vector<std::uint8_t> encode(const ObservationAccepted& value) {
  return encode_reference_body(value.observation_ref, value.body);
}

Result<ObservationAccepted> decode_observation_accepted(std::span<const std::uint8_t> bytes,
                                                        SchemaVersion version) {
  auto decoded = decode_reference_body(bytes, version, "observation-accepted", "observation_ref");
  if (!decoded.has_value()) {
    return decoded.status();
  }
  ObservationAccepted value;
  value.observation_ref = std::move(decoded.value().first);
  value.body = std::move(decoded.value().second);
  return value;
}

std::vector<std::uint8_t> encode(const SubjectMutated& value) {
  return encode_reference_body(value.mutation_ref, value.body);
}

Result<SubjectMutated> decode_subject_mutated(std::span<const std::uint8_t> bytes, SchemaVersion version) {
  auto decoded = decode_reference_body(bytes, version, "subject-mutated", "mutation_ref");
  if (!decoded.has_value()) {
    return decoded.status();
  }
  SubjectMutated value;
  value.mutation_ref = std::move(decoded.value().first);
  value.body = std::move(decoded.value().second);
  return value;
}

std::vector<std::uint8_t> encode(const ReconciliationRecorded& value) {
  return encode_reference_body(value.reconciliation_ref, value.detail);
}

Result<ReconciliationRecorded> decode_reconciliation_recorded(std::span<const std::uint8_t> bytes,
                                                              SchemaVersion version) {
  auto decoded =
      decode_reference_body(bytes, version, "reconciliation-recorded", "reconciliation_ref");
  if (!decoded.has_value()) {
    return decoded.status();
  }
  ReconciliationRecorded value;
  value.reconciliation_ref = std::move(decoded.value().first);
  value.detail = std::move(decoded.value().second);
  return value;
}

std::vector<std::uint8_t> encode(const PolicyAttested& value) {
  return encode_reference_body(value.policy_ref, value.detail);
}

Result<PolicyAttested> decode_policy_attested(std::span<const std::uint8_t> bytes, SchemaVersion version) {
  auto decoded = decode_reference_body(bytes, version, "policy-attested", "policy_ref");
  if (!decoded.has_value()) {
    return decoded.status();
  }
  PolicyAttested value;
  value.policy_ref = std::move(decoded.value().first);
  value.detail = std::move(decoded.value().second);
  return value;
}

// -- Retirement and correction ------------------------------------------------

namespace {

[[nodiscard]] std::vector<std::uint8_t> encode_text(std::string_view text) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  writer.sized_string(text);
  return out;
}

[[nodiscard]] Result<std::string> decode_text(std::span<const std::uint8_t> bytes,
                                              SchemaVersion version,
                                              std::string_view what,
                                              std::string_view field) {
  if (Status status = require_version(version, what); status.is_error()) {
    return status;
  }
  ByteReader reader(bytes);
  std::string text;
  if (!reader.sized_string(text, Limits::kMaxReasonLength)) {
    return malformed_input(ErrorCode::kSizeOutOfRange, std::string(field) + " is malformed or too long");
  }
  if (Status status = require_end(reader, what); status.is_error()) {
    return status;
  }
  if (text.empty()) {
    return invalid_argument(ErrorCode::kValueEmpty, std::string(field) + " is empty");
  }
  if (!is_valid_utf8(text)) {
    return invalid_argument(ErrorCode::kInvalidUtf8, std::string(field) + " is not valid UTF-8");
  }
  if (!has_no_control_characters(text)) {
    return invalid_argument(ErrorCode::kSyntaxInvalid,
                            std::string(field) + " contains a control character");
  }
  return text;
}

}  // namespace

std::vector<std::uint8_t> encode(const SubjectRetired& value) { return encode_text(value.reason); }

Result<SubjectRetired> decode_subject_retired(std::span<const std::uint8_t> bytes, SchemaVersion version) {
  auto decoded = decode_text(bytes, version, "subject-retired", "reason");
  if (!decoded.has_value()) {
    return decoded.status();
  }
  return SubjectRetired{std::move(decoded).value()};
}

std::vector<std::uint8_t> encode(const CorrectionRecorded& value) { return encode_text(value.rationale); }

Result<CorrectionRecorded> decode_correction_recorded(std::span<const std::uint8_t> bytes,
                                                      SchemaVersion version) {
  auto decoded = decode_text(bytes, version, "correction-recorded", "rationale");
  if (!decoded.has_value()) {
    return decoded.status();
  }
  return CorrectionRecorded{std::move(decoded).value()};
}

// -- Relationships ------------------------------------------------------------

std::vector<std::uint8_t> encode(const Relationship& value) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  detail::encode_subject_ref(writer, value.related);
  writer.u8(static_cast<std::uint8_t>(value.kind));
  return out;
}

Result<Relationship> decode_relationship(std::span<const std::uint8_t> bytes, SchemaVersion version) {
  if (Status status = require_version(version, "relationship"); status.is_error()) {
    return status;
  }
  ByteReader reader(bytes);
  std::optional<SubjectRef> related;
  std::uint8_t kind_raw = 0;
  if (!decode_subject(reader, related) || !reader.u8(kind_raw)) {
    return truncated("relationship");
  }
  if (kind_raw < static_cast<std::uint8_t>(RelationshipKind::kContainedIn) ||
      kind_raw > static_cast<std::uint8_t>(RelationshipKind::kDependsOn)) {
    return malformed_input(ErrorCode::kInvalidEnumValue,
                           "relationship payload carries unknown relationship kind " +
                               std::to_string(kind_raw));
  }
  if (Status status = require_end(reader, "relationship"); status.is_error()) {
    return status;
  }
  return Relationship{std::move(related).value(), static_cast<RelationshipKind>(kind_raw)};
}

// -- Dispatch -----------------------------------------------------------------

Status validate(EventKind kind,
                const SchemaId& schema_id,
                SchemaVersion version,
                std::span<const std::uint8_t> bytes) {
  const SchemaId expected = required_schema(kind);
  if (schema_id != expected) {
    return unsupported_version(ErrorCode::kUnsupportedPayloadSchema,
                               "event kind " + std::string(to_string(kind)) + " requires payload schema '" +
                                   expected.str() + "' but '" + schema_id.str() + "' was supplied");
  }
  if (bytes.size() > ByteReader::kMaxSizedField) {
    return capacity_exceeded(ErrorCode::kSizeOutOfRange, "payload exceeds the structural size limit");
  }

  switch (kind) {
    case EventKind::kLedgerOpened:
      return decode_ledger_opened(bytes, version).has_value()
                 ? Status::ok()
                 : decode_ledger_opened(bytes, version).status();
    case EventKind::kEpochOpened: {
      auto decoded = decode_epoch_opened(bytes, version);
      return decoded.has_value() ? Status::ok() : decoded.status();
    }
    case EventKind::kEpochClosed: {
      auto decoded = decode_epoch_closed(bytes, version);
      return decoded.has_value() ? Status::ok() : decoded.status();
    }
    case EventKind::kGenerationAdvanced: {
      auto decoded = decode_generation_advanced(bytes, version);
      return decoded.has_value() ? Status::ok() : decoded.status();
    }
    case EventKind::kSubjectRegistered: {
      auto decoded = decode_subject_registered(bytes, version);
      return decoded.has_value() ? Status::ok() : decoded.status();
    }
    case EventKind::kObservationAccepted: {
      auto decoded = decode_observation_accepted(bytes, version);
      return decoded.has_value() ? Status::ok() : decoded.status();
    }
    case EventKind::kSubjectMutated: {
      auto decoded = decode_subject_mutated(bytes, version);
      return decoded.has_value() ? Status::ok() : decoded.status();
    }
    case EventKind::kSubjectRetired: {
      auto decoded = decode_subject_retired(bytes, version);
      return decoded.has_value() ? Status::ok() : decoded.status();
    }
    case EventKind::kRelationshipAsserted:
    case EventKind::kRelationshipRetracted: {
      auto decoded = decode_relationship(bytes, version);
      return decoded.has_value() ? Status::ok() : decoded.status();
    }
    case EventKind::kReconciliationRecorded: {
      auto decoded = decode_reconciliation_recorded(bytes, version);
      return decoded.has_value() ? Status::ok() : decoded.status();
    }
    case EventKind::kCorrectionRecorded: {
      auto decoded = decode_correction_recorded(bytes, version);
      return decoded.has_value() ? Status::ok() : decoded.status();
    }
    case EventKind::kPolicyAttested: {
      auto decoded = decode_policy_attested(bytes, version);
      return decoded.has_value() ? Status::ok() : decoded.status();
    }
  }
  return malformed_input(ErrorCode::kInvalidEnumValue, "event kind is not known");
}

}  // namespace fsl::payload

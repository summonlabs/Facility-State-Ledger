// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fsl/event.hpp"

#include <array>

#include "detail/codec.hpp"

namespace fsl {
namespace {

struct EventKindName {
  EventKind kind;
  std::string_view name;
};

constexpr std::array<EventKindName, 13> kEventKindNames{{
    {EventKind::kLedgerOpened, "ledger-opened"},
    {EventKind::kEpochOpened, "epoch-opened"},
    {EventKind::kEpochClosed, "epoch-closed"},
    {EventKind::kGenerationAdvanced, "generation-advanced"},
    {EventKind::kSubjectRegistered, "subject-registered"},
    {EventKind::kObservationAccepted, "observation-accepted"},
    {EventKind::kSubjectMutated, "subject-mutated"},
    {EventKind::kSubjectRetired, "subject-retired"},
    {EventKind::kRelationshipAsserted, "relationship-asserted"},
    {EventKind::kRelationshipRetracted, "relationship-retracted"},
    {EventKind::kReconciliationRecorded, "reconciliation-recorded"},
    {EventKind::kCorrectionRecorded, "correction-recorded"},
    {EventKind::kPolicyAttested, "policy-attested"},
}};

}  // namespace

std::string_view to_string(EventKind kind) noexcept {
  for (const auto& entry : kEventKindNames) {
    if (entry.kind == kind) {
      return entry.name;
    }
  }
  return "unknown";
}

std::optional<EventKind> event_kind_from_string(std::string_view name) noexcept {
  for (const auto& entry : kEventKindNames) {
    if (entry.name == name) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

bool event_kind_is_epoch_control(EventKind kind) noexcept {
  switch (kind) {
    case EventKind::kLedgerOpened:
    case EventKind::kEpochOpened:
    case EventKind::kEpochClosed:
    case EventKind::kGenerationAdvanced:
      return true;
    case EventKind::kSubjectRegistered:
    case EventKind::kObservationAccepted:
    case EventKind::kSubjectMutated:
    case EventKind::kSubjectRetired:
    case EventKind::kRelationshipAsserted:
    case EventKind::kRelationshipRetracted:
    case EventKind::kReconciliationRecorded:
    case EventKind::kCorrectionRecorded:
    case EventKind::kPolicyAttested:
      return false;
  }
  return false;
}

Result<SubmittedObservation> SubmittedObservation::create(SubmittedObservationFields fields) {
  if (!fields.event_id.has_value()) {
    return invalid_argument(ErrorCode::kMissingRequiredAttribute, "submission is missing an event identity");
  }
  if (fields.event_id->is_nil()) {
    return invalid_argument(ErrorCode::kHeaderFieldInvalid,
                            "the nil identity is not a valid event identity");
  }
  if (fields.idempotency_token.has_value() && fields.idempotency_token->is_nil()) {
    return invalid_argument(ErrorCode::kHeaderFieldInvalid,
                            "the nil identity is not a valid idempotency token");
  }
  if (!fields.facility_generation.has_value()) {
    return invalid_argument(ErrorCode::kMissingRequiredAttribute,
                            "submission is missing a facility generation");
  }
  if (fields.facility_generation->is_zero()) {
    return invalid_argument(ErrorCode::kInvalidEnumValue,
                            "facility generation zero is not a valid generation");
  }
  if (!fields.kind.has_value()) {
    return invalid_argument(ErrorCode::kMissingRequiredAttribute, "submission is missing an event kind");
  }
  const bool is_ledger_opened = *fields.kind == EventKind::kLedgerOpened;
  const bool is_generation_advanced = *fields.kind == EventKind::kGenerationAdvanced;
  if (is_ledger_opened || is_generation_advanced) {
    if (fields.epoch.has_value()) {
      return invalid_argument(ErrorCode::kIllegalLifecycleTransition,
                              is_ledger_opened
                                  ? "a ledger-opened event precedes every epoch and must not name one"
                                  : "the facility generation may only advance while no epoch is open, so "
                                    "a generation-advanced event must not name one");
    }
  } else if (!fields.epoch.has_value()) {
    return invalid_argument(ErrorCode::kMissingRequiredAttribute, "submission is missing a facility epoch");
  } else if (fields.epoch->is_zero()) {
    return invalid_argument(ErrorCode::kInvalidEnumValue, "facility epoch zero is not a valid epoch");
  }
  if (!fields.subject.has_value()) {
    return invalid_argument(ErrorCode::kMissingRequiredAttribute, "submission is missing a subject");
  }
  if (!fields.payload_schema.has_value()) {
    return invalid_argument(ErrorCode::kMissingRequiredAttribute,
                            "submission is missing a payload schema identifier");
  }
  if (!fields.payload_schema_version.has_value()) {
    return invalid_argument(ErrorCode::kMissingRequiredAttribute,
                            "submission is missing a payload schema version");
  }
  if (fields.payload_schema_version->is_zero()) {
    return invalid_argument(ErrorCode::kInvalidEnumValue, "payload schema version zero is invalid");
  }
  if (!fields.provenance.has_value()) {
    return invalid_argument(ErrorCode::kMissingRequiredAttribute,
                            "submission is missing a provenance record");
  }
  if (fields.payload.size() > ByteReader::kMaxSizedField) {
    return capacity_exceeded(ErrorCode::kSizeOutOfRange,
                             "payload of " + std::to_string(fields.payload.size()) +
                                 " bytes exceeds the structural limit");
  }
  if (fields.correction_target.has_value()) {
    if (fields.correction_target->is_nil()) {
      return invalid_argument(ErrorCode::kHeaderFieldInvalid,
                              "the nil identity is not a valid correction target");
    }
    if (*fields.correction_target == *fields.event_id) {
      return invalid_argument(ErrorCode::kSelfReference, "an event cannot correct itself");
    }
    if (*fields.kind != EventKind::kCorrectionRecorded) {
      return invalid_argument(ErrorCode::kCorrectionTargetUnexpected,
                              "a correction target is only meaningful on a " +
                                  std::string(to_string(EventKind::kCorrectionRecorded)) + " event");
    }
  }

  SubmittedObservation observation(std::move(*fields.event_id),
                                   fields.idempotency_token,
                                   *fields.facility_generation,
                                   fields.epoch,
                                   *fields.kind,
                                   std::move(*fields.subject),
                                   fields.correction_target,
                                   std::move(*fields.payload_schema),
                                   *fields.payload_schema_version,
                                   std::move(fields.payload),
                                   std::move(*fields.provenance),
                                   Digest{});
  // The content digest covers exactly the caller-controlled portion, so two
  // submissions that agree on every caller-controlled field hash equally no
  // matter when or where they were accepted.
  const std::vector<std::uint8_t> canonical = detail::encode_submission(observation);
  observation.content_digest_ = Sha256::hash(canonical.data(), canonical.size());
  return observation;
}

std::vector<std::uint8_t> SubmittedObservation::canonical_submission_bytes() const {
  return detail::encode_submission(*this);
}

}  // namespace fsl

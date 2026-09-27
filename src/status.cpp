// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fsl/status.hpp"

#include <array>
#include <utility>

namespace fsl {
namespace {

struct CategoryName {
  ErrorCategory category;
  std::string_view name;
};

constexpr std::array<CategoryName, 17> kCategoryNames{{
    {ErrorCategory::kNone, "none"},
    {ErrorCategory::kInvalidArgument, "invalid-argument"},
    {ErrorCategory::kMalformedInput, "malformed-input"},
    {ErrorCategory::kUnsupportedVersion, "unsupported-version"},
    {ErrorCategory::kIntegrityFailure, "integrity-failure"},
    {ErrorCategory::kNotFound, "not-found"},
    {ErrorCategory::kConflict, "conflict"},
    {ErrorCategory::kStaleGeneration, "stale-generation"},
    {ErrorCategory::kStaleEpoch, "stale-epoch"},
    {ErrorCategory::kStaleSource, "stale-source"},
    {ErrorCategory::kCapacityExceeded, "capacity-exceeded"},
    {ErrorCategory::kIoFailure, "io-failure"},
    {ErrorCategory::kRecoveryRequired, "recovery-required"},
    {ErrorCategory::kLocked, "locked"},
    {ErrorCategory::kClosed, "closed"},
    {ErrorCategory::kReadOnly, "read-only"},
    {ErrorCategory::kInternal, "internal"},
}};

struct CodeName {
  ErrorCode code;
  std::string_view name;
};

// The spelling of every code is part of the stable public contract. Codes are
// appended, never renumbered, and never renamed.
constexpr std::array<CodeName, 109> kCodeNames{{
    {ErrorCode::kOk, "ok"},
    {ErrorCode::kValueEmpty, "value-empty"},
    {ErrorCode::kValueTooLong, "value-too-long"},
    {ErrorCode::kSyntaxInvalid, "syntax-invalid"},
    {ErrorCode::kInvalidUtf8, "invalid-utf8"},
    {ErrorCode::kInvalidHexEncoding, "invalid-hex-encoding"},
    {ErrorCode::kInvalidEnumValue, "invalid-enum-value"},
    {ErrorCode::kInvalidBooleanText, "invalid-boolean-text"},
    {ErrorCode::kInvalidIntegerText, "invalid-integer-text"},
    {ErrorCode::kArithmeticOverflow, "arithmetic-overflow"},
    {ErrorCode::kNullInput, "null-input"},
    {ErrorCode::kBadMagic, "bad-magic"},
    {ErrorCode::kUnsupportedFormatVersion, "unsupported-format-version"},
    {ErrorCode::kUnsupportedPayloadSchema, "unsupported-payload-schema"},
    {ErrorCode::kRecordLengthInvalid, "record-length-invalid"},
    {ErrorCode::kHeaderFieldInvalid, "header-field-invalid"},
    {ErrorCode::kCountOutOfRange, "count-out-of-range"},
    {ErrorCode::kSizeOutOfRange, "size-out-of-range"},
    {ErrorCode::kDuplicateKey, "duplicate-key"},
    {ErrorCode::kMissingRequiredAttribute, "missing-required-attribute"},
    {ErrorCode::kUnknownAttribute, "unknown-attribute"},
    {ErrorCode::kUnexpectedTrailingBytes, "unexpected-trailing-bytes"},
    {ErrorCode::kTruncatedInput, "truncated-input"},
    {ErrorCode::kDigestMismatch, "digest-mismatch"},
    {ErrorCode::kChainMismatch, "chain-mismatch"},
    {ErrorCode::kHeaderChecksumMismatch, "header-checksum-mismatch"},
    {ErrorCode::kSegmentIdentityMismatch, "segment-identity-mismatch"},
    {ErrorCode::kLedgerIdentityMismatch, "ledger-identity-mismatch"},
    {ErrorCode::kManifestAheadOfLog, "manifest-ahead-of-log"},
    {ErrorCode::kIndexInconsistent, "index-inconsistent"},
    {ErrorCode::kMidFileCorruption, "mid-file-corruption"},
    {ErrorCode::kCommittedRecordLost, "committed-record-lost"},
    {ErrorCode::kSequenceNotMonotonic, "sequence-not-monotonic"},
    {ErrorCode::kSequenceGap, "sequence-gap"},
    {ErrorCode::kSequenceForgery, "sequence-forgery"},
    {ErrorCode::kStaleFacilityGeneration, "stale-facility-generation"},
    {ErrorCode::kFutureFacilityGeneration, "future-facility-generation"},
    {ErrorCode::kStaleEpoch, "stale-epoch"},
    {ErrorCode::kEpochClosed, "epoch-closed"},
    {ErrorCode::kUnknownEpoch, "unknown-epoch"},
    {ErrorCode::kStaleSourceGeneration, "stale-source-generation"},
    {ErrorCode::kSourceSequenceRegression, "source-sequence-regression"},
    {ErrorCode::kDuplicateEventId, "duplicate-event-id"},
    {ErrorCode::kDuplicateIdempotencyToken, "duplicate-idempotency-token"},
    {ErrorCode::kIllegalLifecycleTransition, "illegal-lifecycle-transition"},
    {ErrorCode::kSubjectAlreadyRegistered, "subject-already-registered"},
    {ErrorCode::kSubjectNotRegistered, "subject-not-registered"},
    {ErrorCode::kSubjectKindMismatch, "subject-kind-mismatch"},
    {ErrorCode::kSubjectRetired, "subject-retired"},
    {ErrorCode::kMissingSubjectLocation, "missing-subject-location"},
    {ErrorCode::kDanglingSubjectReference, "dangling-subject-reference"},
    {ErrorCode::kCorrectionTargetNotFound, "correction-target-not-found"},
    {ErrorCode::kBatchEmpty, "batch-empty"},
    {ErrorCode::kBatchTooLarge, "batch-too-large"},
    {ErrorCode::kSelfReference, "self-reference"},
    {ErrorCode::kLedgerNotOpen, "ledger-not-open"},
    {ErrorCode::kLedgerAlreadyOpen, "ledger-already-open"},
    {ErrorCode::kLedgerClosed, "ledger-closed"},
    {ErrorCode::kLedgerLocked, "ledger-locked"},
    {ErrorCode::kLedgerNotFound, "ledger-not-found"},
    {ErrorCode::kLedgerExists, "ledger-exists"},
    {ErrorCode::kSegmentSealed, "segment-sealed"},
    {ErrorCode::kSegmentNotActive, "segment-not-active"},
    {ErrorCode::kNoActiveSegment, "no-active-segment"},
    {ErrorCode::kRecoveryPolicyRejected, "recovery-policy-rejected"},
    {ErrorCode::kUncommittedTailDiscarded, "uncommitted-tail-discarded"},
    {ErrorCode::kManifestRebuilt, "manifest-rebuilt"},
    {ErrorCode::kIndexRebuilt, "index-rebuilt"},
    {ErrorCode::kTailTruncated, "tail-truncated"},
    {ErrorCode::kReadOnlyHandle, "read-only-handle"},
    {ErrorCode::kConcurrentModification, "concurrent-modification"},
    {ErrorCode::kCapacityLimitExceeded, "capacity-limit-exceeded"},
    {ErrorCode::kNotFound, "not-found"},
    {ErrorCode::kRangeInvalid, "range-invalid"},
    {ErrorCode::kLimitRequired, "limit-required"},
    {ErrorCode::kLimitOutOfRange, "limit-out-of-range"},
    {ErrorCode::kCursorInvalid, "cursor-invalid"},
    {ErrorCode::kFileOpenFailed, "file-open-failed"},
    {ErrorCode::kFileReadFailed, "file-read-failed"},
    {ErrorCode::kFileWriteFailed, "file-write-failed"},
    {ErrorCode::kFileFlushFailed, "file-flush-failed"},
    {ErrorCode::kFileRenameFailed, "file-rename-failed"},
    {ErrorCode::kDirectoryOperationFailed, "directory-operation-failed"},
    {ErrorCode::kPermissionDenied, "permission-denied"},
    {ErrorCode::kPathInvalid, "path-invalid"},
    {ErrorCode::kPathTraversal, "path-traversal"},
    {ErrorCode::kNotARegularFile, "not-a-regular-file"},
    {ErrorCode::kFileLockFailed, "file-lock-failed"},
    {ErrorCode::kFileTruncateFailed, "file-truncate-failed"},
    {ErrorCode::kInvariantViolated, "invariant-violated"},
    {ErrorCode::kInternalError, "internal-error"},
    {ErrorCode::kCommitMarkerMismatch, "commit-marker-mismatch"},
    {ErrorCode::kSegmentMissing, "segment-missing"},
    {ErrorCode::kSegmentOutOfOrder, "segment-out-of-order"},
    {ErrorCode::kFaultInjected, "fault-injected"},
    {ErrorCode::kWriterFenced, "writer-fenced"},
    {ErrorCode::kStateReconstructionMismatch, "state-reconstruction-mismatch"},
    {ErrorCode::kCorrectionTargetUnexpected, "correction-target-unexpected"},
}};

}  // namespace

std::string_view to_string(ErrorCategory category) noexcept {
  for (const auto& entry : kCategoryNames) {
    if (entry.category == category) {
      return entry.name;
    }
  }
  return "unknown";
}

std::optional<ErrorCategory> error_category_from_string(std::string_view name) noexcept {
  for (const auto& entry : kCategoryNames) {
    if (entry.name == name) {
      return entry.category;
    }
  }
  return std::nullopt;
}

std::string_view to_string(ErrorCode code) noexcept {
  for (const auto& entry : kCodeNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "unknown";
}

std::optional<ErrorCode> error_code_from_string(std::string_view name) noexcept {
  for (const auto& entry : kCodeNames) {
    if (entry.name == name) {
      return entry.code;
    }
  }
  return std::nullopt;
}

std::string Status::to_string() const {
  std::string result;
  result.reserve(message_.size() + 48);
  result.append(fsl::to_string(category_));
  result.push_back('/');
  result.append(fsl::to_string(code_));
  if (!message_.empty()) {
    result.append(": ");
    result.append(message_);
  }
  return result;
}

Status invalid_argument(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kInvalidArgument, code, std::move(message));
}
Status malformed_input(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kMalformedInput, code, std::move(message));
}
Status unsupported_version(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kUnsupportedVersion, code, std::move(message));
}
Status integrity_failure(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kIntegrityFailure, code, std::move(message));
}
Status not_found(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kNotFound, code, std::move(message));
}
Status conflict(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kConflict, code, std::move(message));
}
Status stale_generation(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kStaleGeneration, code, std::move(message));
}
Status stale_epoch(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kStaleEpoch, code, std::move(message));
}
Status stale_source(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kStaleSource, code, std::move(message));
}
Status capacity_exceeded(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kCapacityExceeded, code, std::move(message));
}
Status io_failure(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kIoFailure, code, std::move(message));
}
Status recovery_required(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kRecoveryRequired, code, std::move(message));
}
Status locked(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kLocked, code, std::move(message));
}
Status closed(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kClosed, code, std::move(message));
}
Status read_only(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kReadOnly, code, std::move(message));
}
Status internal_error(ErrorCode code, std::string message) {
  return Status(ErrorCategory::kInternal, code, std::move(message));
}

}  // namespace fsl

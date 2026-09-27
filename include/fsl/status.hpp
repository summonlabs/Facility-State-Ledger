// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

/// \file status.hpp
/// Stable, machine-readable error model shared by every public entry point.

namespace fsl {

/// Coarse failure domain. Callers may branch on this; it is part of the stable
/// public contract and values are never renumbered.
enum class ErrorCategory : std::uint8_t {
  kNone = 0,
  /// Caller supplied an argument that cannot be interpreted.
  kInvalidArgument = 1,
  /// Persisted or imported bytes are malformed.
  kMalformedInput = 2,
  /// Bytes are well formed but use a version this build does not implement.
  kUnsupportedVersion = 3,
  /// Integrity verification failed for authoritative durable state.
  kIntegrityFailure = 4,
  /// The requested identity/range does not exist in the committed log.
  kNotFound = 5,
  /// The request conflicts with committed state (duplicate, illegal transition).
  kConflict = 6,
  /// The envelope targets a facility generation that is no longer current.
  kStaleGeneration = 7,
  /// The envelope targets a facility epoch that is not the open epoch.
  kStaleEpoch = 8,
  /// The producing component presented a superseded source generation.
  kStaleSource = 9,
  /// A resource bound (payload, batch, query limit, metadata) was exceeded.
  kCapacityExceeded = 10,
  /// Operating-system level failure.
  kIoFailure = 11,
  /// Durable state exists but cannot be used until an explicit recovery runs.
  kRecoveryRequired = 12,
  /// Another process holds the exclusive writer lock for this ledger.
  kLocked = 13,
  /// The ledger handle is closed or was never open.
  kClosed = 14,
  /// Read-only handle rejected a mutation.
  kReadOnly = 15,
  /// A documented internal invariant no longer holds; the state is untrusted.
  kInternal = 16,
};

/// Machine-readable category name, stable across releases.
[[nodiscard]] std::string_view to_string(ErrorCategory category) noexcept;
/// Parses a category name produced by to_string. Returns nullopt when unknown.
[[nodiscard]] std::optional<ErrorCategory> error_category_from_string(std::string_view name) noexcept;

/// Fine-grained, stable diagnostic code. Values are never renumbered; new codes
/// are appended. Callers may branch on these for deterministic handling.
enum class ErrorCode : std::uint16_t {
  kOk = 0,

  // Argument and text validation.
  kValueEmpty = 1,
  kValueTooLong = 2,
  kSyntaxInvalid = 3,
  kInvalidUtf8 = 4,
  kInvalidHexEncoding = 5,
  kInvalidEnumValue = 6,
  kInvalidBooleanText = 7,
  kInvalidIntegerText = 8,
  kArithmeticOverflow = 9,
  kNullInput = 10,

  // Structural validation of untrusted bytes.
  kBadMagic = 20,
  kUnsupportedFormatVersion = 21,
  kUnsupportedPayloadSchema = 22,
  kRecordLengthInvalid = 23,
  kHeaderFieldInvalid = 24,
  kCountOutOfRange = 25,
  kSizeOutOfRange = 26,
  kDuplicateKey = 27,
  kMissingRequiredAttribute = 28,
  kUnknownAttribute = 29,
  kUnexpectedTrailingBytes = 30,
  kTruncatedInput = 31,

  // Integrity.
  kDigestMismatch = 40,
  kChainMismatch = 41,
  kHeaderChecksumMismatch = 42,
  kSegmentIdentityMismatch = 43,
  kLedgerIdentityMismatch = 44,
  kManifestAheadOfLog = 45,
  kIndexInconsistent = 46,
  kMidFileCorruption = 47,
  kCommittedRecordLost = 48,

  // Ordering and admission.
  kSequenceNotMonotonic = 60,
  kSequenceGap = 61,
  kSequenceForgery = 62,
  kStaleFacilityGeneration = 63,
  kFutureFacilityGeneration = 64,
  kStaleEpoch = 65,
  kEpochClosed = 66,
  kUnknownEpoch = 67,
  kStaleSourceGeneration = 68,
  kSourceSequenceRegression = 69,
  kDuplicateEventId = 70,
  kDuplicateIdempotencyToken = 71,
  kIllegalLifecycleTransition = 72,
  kSubjectAlreadyRegistered = 73,
  kSubjectNotRegistered = 74,
  kSubjectKindMismatch = 75,
  kSubjectRetired = 76,
  kMissingSubjectLocation = 77,
  kDanglingSubjectReference = 78,
  kCorrectionTargetNotFound = 79,
  kBatchEmpty = 80,
  kBatchTooLarge = 81,
  kSelfReference = 82,

  // Ledger lifecycle and durability plumbing.
  kLedgerNotOpen = 100,
  kLedgerAlreadyOpen = 101,
  kLedgerClosed = 102,
  kLedgerLocked = 103,
  kLedgerNotFound = 104,
  kLedgerExists = 105,
  kSegmentSealed = 106,
  kSegmentNotActive = 107,
  kNoActiveSegment = 108,
  kRecoveryPolicyRejected = 109,
  kUncommittedTailDiscarded = 110,
  kManifestRebuilt = 111,
  kIndexRebuilt = 112,
  kTailTruncated = 113,
  kReadOnlyHandle = 114,
  kConcurrentModification = 115,

  // Capacity bounds.
  kCapacityLimitExceeded = 130,

  // Queries.
  kNotFound = 150,
  kRangeInvalid = 151,
  kLimitRequired = 152,
  kLimitOutOfRange = 153,
  kCursorInvalid = 154,

  // Input/output.
  kFileOpenFailed = 170,
  kFileReadFailed = 171,
  kFileWriteFailed = 172,
  kFileFlushFailed = 173,
  kFileRenameFailed = 174,
  kDirectoryOperationFailed = 175,
  kPermissionDenied = 176,
  kPathInvalid = 177,
  kPathTraversal = 178,
  kNotARegularFile = 179,
  kFileLockFailed = 180,
  kFileTruncateFailed = 181,

  // Internal.
  kInvariantViolated = 200,
  kInternalError = 201,

  // Physical framing and structural consistency.
  kCommitMarkerMismatch = 210,
  kSegmentMissing = 211,
  kSegmentOutOfOrder = 212,
  /// A configured fault injector was invoked at a durability boundary.
  kFaultInjected = 213,
  /// A write was attempted by a handle whose ownership of the directory ended.
  kWriterFenced = 214,
  /// Reconstructed derived state disagreed with the authoritative log.
  kStateReconstructionMismatch = 215,
  /// A correction target was supplied on a kind that does not accept one.
  kCorrectionTargetUnexpected = 216,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;
[[nodiscard]] std::optional<ErrorCode> error_code_from_string(std::string_view name) noexcept;

/// A machine-readable failure plus a concise human explanation.
///
/// A default-constructed Status is success. Status is copyable and movable; it
/// is intentionally small enough to travel through Result without allocation
/// in the success path.
class Status {
 public:
  Status() noexcept = default;

  Status(ErrorCategory category, ErrorCode code, std::string message)
      : category_(category), code_(code), message_(std::move(message)) {}

  /// Successful status.
  [[nodiscard]] static Status ok() noexcept { return Status{}; }

  [[nodiscard]] bool is_ok() const noexcept { return category_ == ErrorCategory::kNone && code_ == ErrorCode::kOk; }
  [[nodiscard]] bool is_error() const noexcept { return !is_ok(); }

  [[nodiscard]] ErrorCategory category() const noexcept { return category_; }
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

  /// Stable single-line rendering: "<category>/<code>: <message>".
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Status& lhs, const Status& rhs) noexcept {
    return lhs.category_ == rhs.category_ && lhs.code_ == rhs.code_ && lhs.message_ == rhs.message_;
  }

 private:
  ErrorCategory category_ = ErrorCategory::kNone;
  ErrorCode code_ = ErrorCode::kOk;
  std::string message_;
};

/// Result of an operation that produces a value.
///
/// Exactly one of "value" or "status" is present. Result is [[nodiscard]] so a
/// rejected mutation cannot be silently ignored. Neither T nor Status is
/// default-constructed on the error path, so T need not be default
/// constructible.
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  /// True when the operation succeeded. Lets a caller treat a value-carrying
  /// result and a valueless one with the same predicate.
  [[nodiscard]] bool is_ok() const noexcept { return has_value(); }
  [[nodiscard]] bool is_error() const noexcept { return !has_value(); }

  [[nodiscard]] const Status& status() const noexcept { return status_; }

  /// Precondition: has_value(). Violating it throws std::bad_optional_access
  /// rather than producing undefined behaviour.
  [[nodiscard]] T& value() & { return value_.value(); }
  [[nodiscard]] const T& value() const& { return value_.value(); }
  [[nodiscard]] T&& value() && { return std::move(value_.value()); }

  /// Precondition: has_value().
  [[nodiscard]] T* operator->() { return &value_.value(); }
  [[nodiscard]] const T* operator->() const { return &value_.value(); }
  [[nodiscard]] T& operator*() & { return value_.value(); }
  [[nodiscard]] const T& operator*() const& { return value_.value(); }

  /// Value when present, otherwise the supplied fallback.
  template <class U>
  [[nodiscard]] T value_or(U&& fallback) const& {
    return has_value() ? *value_ : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  std::optional<T> value_;
  Status status_;
};

/// Result of an operation that produces no value.
///
/// An operation with no value *is* its status, so this type converts to Status
/// without loss. That keeps call sites that already hold a Status uniform.
template <>
class [[nodiscard]] Result<void> {
 public:
  Result() noexcept = default;
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return status_.is_ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] bool is_ok() const noexcept { return status_.is_ok(); }
  [[nodiscard]] bool is_error() const noexcept { return status_.is_error(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] Status to_status() const { return status_; }
  operator Status() const noexcept { return status_; }  // NOLINT(google-explicit-constructor)

 private:
  Status status_;
};

// -- Convenience constructors ------------------------------------------------
// These keep call sites short while preserving the category/code vocabulary.

[[nodiscard]] Status invalid_argument(ErrorCode code, std::string message);
[[nodiscard]] Status malformed_input(ErrorCode code, std::string message);
[[nodiscard]] Status unsupported_version(ErrorCode code, std::string message);
[[nodiscard]] Status integrity_failure(ErrorCode code, std::string message);
[[nodiscard]] Status not_found(ErrorCode code, std::string message);
[[nodiscard]] Status conflict(ErrorCode code, std::string message);
[[nodiscard]] Status stale_generation(ErrorCode code, std::string message);
[[nodiscard]] Status stale_epoch(ErrorCode code, std::string message);
[[nodiscard]] Status stale_source(ErrorCode code, std::string message);
[[nodiscard]] Status capacity_exceeded(ErrorCode code, std::string message);
[[nodiscard]] Status io_failure(ErrorCode code, std::string message);
[[nodiscard]] Status recovery_required(ErrorCode code, std::string message);
[[nodiscard]] Status locked(ErrorCode code, std::string message);
[[nodiscard]] Status closed(ErrorCode code, std::string message);
[[nodiscard]] Status read_only(ErrorCode code, std::string message);
[[nodiscard]] Status internal_error(ErrorCode code, std::string message);

}  // namespace fsl

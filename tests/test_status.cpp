// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "fsl/status.hpp"
#include "support/test_support.hpp"

/// \file test_status.cpp
/// The stable error vocabulary and the Result contract.

namespace {

FSL_TEST(error_category_names_round_trip) {
  const fsl::ErrorCategory categories[] = {
      fsl::ErrorCategory::kNone,           fsl::ErrorCategory::kInvalidArgument,
      fsl::ErrorCategory::kMalformedInput, fsl::ErrorCategory::kUnsupportedVersion,
      fsl::ErrorCategory::kIntegrityFailure, fsl::ErrorCategory::kNotFound,
      fsl::ErrorCategory::kConflict,       fsl::ErrorCategory::kStaleGeneration,
      fsl::ErrorCategory::kStaleEpoch,     fsl::ErrorCategory::kStaleSource,
      fsl::ErrorCategory::kCapacityExceeded, fsl::ErrorCategory::kIoFailure,
      fsl::ErrorCategory::kRecoveryRequired, fsl::ErrorCategory::kLocked,
      fsl::ErrorCategory::kClosed,         fsl::ErrorCategory::kReadOnly,
      fsl::ErrorCategory::kInternal,
  };
  for (const fsl::ErrorCategory category : categories) {
    const std::string_view name = fsl::to_string(category);
    FSL_CHECK(!name.empty());
    FSL_CHECK(name != "unknown");
    const auto parsed = fsl::error_category_from_string(name);
    FSL_CHECK(parsed.has_value());
    FSL_CHECK(parsed.value() == category);
  }
}

FSL_TEST(error_codes_round_trip) {
  // Every code in the stable vocabulary must have a name and parse back to
  // itself. A code that loses its name silently breaks machine consumers.
  const fsl::ErrorCode codes[] = {
      fsl::ErrorCode::kOk,
      fsl::ErrorCode::kValueEmpty,
      fsl::ErrorCode::kSyntaxInvalid,
      fsl::ErrorCode::kInvalidUtf8,
      fsl::ErrorCode::kBadMagic,
      fsl::ErrorCode::kUnsupportedFormatVersion,
      fsl::ErrorCode::kUnsupportedPayloadSchema,
      fsl::ErrorCode::kTruncatedInput,
      fsl::ErrorCode::kDigestMismatch,
      fsl::ErrorCode::kChainMismatch,
      fsl::ErrorCode::kCommittedRecordLost,
      fsl::ErrorCode::kSequenceForgery,
      fsl::ErrorCode::kStaleFacilityGeneration,
      fsl::ErrorCode::kStaleEpoch,
      fsl::ErrorCode::kStaleSourceGeneration,
      fsl::ErrorCode::kDuplicateEventId,
      fsl::ErrorCode::kDuplicateIdempotencyToken,
      fsl::ErrorCode::kSubjectAlreadyRegistered,
      fsl::ErrorCode::kSubjectRetired,
      fsl::ErrorCode::kBatchEmpty,
      fsl::ErrorCode::kBatchTooLarge,
      fsl::ErrorCode::kLedgerLocked,
      fsl::ErrorCode::kCapacityLimitExceeded,
      fsl::ErrorCode::kLimitRequired,
      fsl::ErrorCode::kNotARegularFile,
      fsl::ErrorCode::kPathTraversal,
      fsl::ErrorCode::kCommitMarkerMismatch,
      fsl::ErrorCode::kWriterFenced,
      fsl::ErrorCode::kCorrectionTargetUnexpected,
  };
  for (const fsl::ErrorCode code : codes) {
    const std::string_view name = fsl::to_string(code);
    FSL_CHECK(!name.empty());
    FSL_CHECK(name != "unknown");
    const auto parsed = fsl::error_code_from_string(name);
    FSL_CHECK(parsed.has_value());
    FSL_CHECK(parsed.value() == code);
  }
  FSL_CHECK(!fsl::error_code_from_string("no-such-code").has_value());
  FSL_CHECK(!fsl::error_category_from_string("no-such-category").has_value());
}

FSL_TEST(status_default_is_success) {
  const fsl::Status status;
  FSL_CHECK(status.is_ok());
  FSL_CHECK(!status.is_error());
  FSL_CHECK(status.category() == fsl::ErrorCategory::kNone);
  FSL_CHECK(status.code() == fsl::ErrorCode::kOk);
  FSL_CHECK(status.message().empty());
}

FSL_TEST(status_renders_category_code_and_message) {
  const auto status =
      fsl::malformed_input(fsl::ErrorCode::kTruncatedInput, "the record ends early");
  FSL_CHECK_EQ(status.category(), fsl::ErrorCategory::kMalformedInput);
  FSL_CHECK_EQ(status.code(), fsl::ErrorCode::kTruncatedInput);
  FSL_CHECK_EQ(status.to_string(), std::string("malformed-input/truncated-input: the record ends early"));
}

FSL_TEST(convenience_constructors_use_distinct_categories) {
  FSL_CHECK_EQ(fsl::invalid_argument(fsl::ErrorCode::kSyntaxInvalid, "x").category(),
               fsl::ErrorCategory::kInvalidArgument);
  FSL_CHECK_EQ(fsl::malformed_input(fsl::ErrorCode::kBadMagic, "x").category(),
               fsl::ErrorCategory::kMalformedInput);
  FSL_CHECK_EQ(fsl::unsupported_version(fsl::ErrorCode::kUnsupportedFormatVersion, "x").category(),
               fsl::ErrorCategory::kUnsupportedVersion);
  FSL_CHECK_EQ(fsl::integrity_failure(fsl::ErrorCode::kDigestMismatch, "x").category(),
               fsl::ErrorCategory::kIntegrityFailure);
  FSL_CHECK_EQ(fsl::not_found(fsl::ErrorCode::kNotFound, "x").category(), fsl::ErrorCategory::kNotFound);
  FSL_CHECK_EQ(fsl::conflict(fsl::ErrorCode::kDuplicateEventId, "x").category(),
               fsl::ErrorCategory::kConflict);
  FSL_CHECK_EQ(fsl::stale_generation(fsl::ErrorCode::kStaleFacilityGeneration, "x").category(),
               fsl::ErrorCategory::kStaleGeneration);
  FSL_CHECK_EQ(fsl::stale_epoch(fsl::ErrorCode::kStaleEpoch, "x").category(),
               fsl::ErrorCategory::kStaleEpoch);
  FSL_CHECK_EQ(fsl::stale_source(fsl::ErrorCode::kStaleSourceGeneration, "x").category(),
               fsl::ErrorCategory::kStaleSource);
  FSL_CHECK_EQ(fsl::capacity_exceeded(fsl::ErrorCode::kCapacityLimitExceeded, "x").category(),
               fsl::ErrorCategory::kCapacityExceeded);
  FSL_CHECK_EQ(fsl::io_failure(fsl::ErrorCode::kFileReadFailed, "x").category(),
               fsl::ErrorCategory::kIoFailure);
  FSL_CHECK_EQ(fsl::recovery_required(fsl::ErrorCode::kRecoveryPolicyRejected, "x").category(),
               fsl::ErrorCategory::kRecoveryRequired);
  FSL_CHECK_EQ(fsl::locked(fsl::ErrorCode::kLedgerLocked, "x").category(), fsl::ErrorCategory::kLocked);
  FSL_CHECK_EQ(fsl::closed(fsl::ErrorCode::kLedgerClosed, "x").category(), fsl::ErrorCategory::kClosed);
  FSL_CHECK_EQ(fsl::read_only(fsl::ErrorCode::kReadOnlyHandle, "x").category(),
               fsl::ErrorCategory::kReadOnly);
  FSL_CHECK_EQ(fsl::internal_error(fsl::ErrorCode::kInvariantViolated, "x").category(),
               fsl::ErrorCategory::kInternal);
}

FSL_TEST(result_holds_exactly_one_of_value_or_status) {
  const fsl::Result<int> good(42);
  FSL_CHECK(good.has_value());
  FSL_CHECK(static_cast<bool>(good));
  FSL_CHECK_EQ(good.value(), 42);
  FSL_CHECK_EQ(*good, 42);
  FSL_CHECK(good.status().is_ok());

  const fsl::Result<int> bad(fsl::conflict(fsl::ErrorCode::kDuplicateEventId, "already committed"));
  FSL_CHECK(!bad.has_value());
  FSL_CHECK(!static_cast<bool>(bad));
  FSL_CHECK(bad.status().is_error());
  FSL_CHECK_EQ(bad.status().code(), fsl::ErrorCode::kDuplicateEventId);
  FSL_CHECK_EQ(bad.value_or(7), 7);
}

FSL_TEST(result_does_not_require_default_constructible_payload) {
  // A type with no default constructor must still be usable as a Result payload;
  // the error path must not construct one.
  struct NoDefault {
    explicit NoDefault(int value) : value(value) {}
    int value;
  };
  const fsl::Result<NoDefault> good(NoDefault(5));
  FSL_CHECK(good.has_value());
  FSL_CHECK_EQ(good.value().value, 5);
  const fsl::Result<NoDefault> bad(fsl::Status(fsl::ErrorCategory::kInternal, fsl::ErrorCode::kInternalError,
                                               "no value"));
  FSL_CHECK(!bad.has_value());
}

FSL_TEST(result_void_converts_to_status) {
  const fsl::Result<void> good;
  const fsl::Status as_status = good;
  FSL_CHECK(as_status.is_ok());
  const fsl::Result<void> bad(fsl::io_failure(fsl::ErrorCode::kFileReadFailed, "cannot read"));
  const fsl::Status error = bad;
  FSL_CHECK(error.is_error());
  FSL_CHECK_EQ(error.code(), fsl::ErrorCode::kFileReadFailed);
}

FSL_TEST(result_value_on_error_throws_rather_than_corrupting) {
  const fsl::Result<int> bad(fsl::internal_error(fsl::ErrorCode::kInternalError, "no value"));
  bool threw = false;
  try {
    (void)bad.value();
  } catch (const std::bad_optional_access&) {
    threw = true;
  }
  FSL_CHECK(threw);
}

}  // namespace

FSL_TEST_MAIN("test_status")

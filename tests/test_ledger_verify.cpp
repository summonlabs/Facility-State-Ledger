// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <vector>

#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_ledger_verify.cpp
/// Integrity verification scopes and the index-versus-log consistency check.

namespace {

[[nodiscard]] fsl::Result<fsl::Ledger> build_journal(fsl_test::TempDirectory& temp,
                                                     std::uint64_t events,
                                                     std::size_t segment_bytes) {
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.max_segment_bytes = segment_bytes;
  auto created = fsl::Ledger::create(temp.path(), options);
  if (!created.has_value()) {
    return created.status();
  }
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  if (!seed.has_value()) {
    return seed.status();
  }
  for (std::uint64_t i = 0; i < events; ++i) {
    auto appended =
        fsl_test::record_observation(ledger, seed->asset, "obs-" + std::to_string(i), 200 + i, 12);
    if (!appended.has_value()) {
      return appended.status();
    }
  }
  return ledger;
}

FSL_TEST(a_fresh_ledger_verifies_completely) {
  fsl_test::TempDirectory temp("verify_fresh");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();

  const fsl::VerifyReport report = ledger.verify();
  FSL_CHECK(report.ok());
  FSL_CHECK_EQ(report.records_verified, std::uint64_t{1});
  FSL_CHECK(report.findings.empty());
  FSL_CHECK(report.index_verified);
  FSL_CHECK(report.index_consistent);
  FSL_CHECK(report.first_sequence.has_value());
  FSL_CHECK_EQ(report.first_sequence->value(), std::uint64_t{1});
  FSL_CHECK_EQ(report.last_sequence->value(), std::uint64_t{1});
}

FSL_TEST(a_full_verification_walks_every_segment_and_agrees_with_the_manifest) {
  fsl_test::TempDirectory temp("verify_full");
  auto made = build_journal(temp, 60, 3000);
  FSL_REQUIRE_OK(made);
  auto segments = made->segments();
  FSL_REQUIRE_OK(segments);
  FSL_REQUIRE(segments->size() > 1);

  fsl::VerifyRequest request;
  request.scope = fsl::VerifyScope::kFull;
  const fsl::VerifyReport report = made->verify(request);
  FSL_CHECK(report.ok());
  FSL_CHECK_EQ(report.records_verified, made->watermark().value().sequence->value());
  FSL_CHECK_EQ(report.segments_verified, segments->size());
  FSL_CHECK(report.chain_at_end == made->watermark().value().chain);
  FSL_CHECK_EQ(report.last_sequence->value(), made->watermark().value().sequence->value());
}

FSL_TEST(the_manifest_tail_scope_validates_the_committed_segment) {
  fsl_test::TempDirectory temp("verify_tail");
  auto made = build_journal(temp, 30, 4096);
  FSL_REQUIRE_OK(made);

  fsl::VerifyRequest request;
  request.scope = fsl::VerifyScope::kManifestTail;
  const fsl::VerifyReport report = made->verify(request);
  FSL_CHECK(report.ok());
  FSL_CHECK(report.records_verified > 0);
  FSL_CHECK_EQ(report.last_sequence->value(), made->watermark().value().sequence->value());
}

FSL_TEST(verification_can_start_from_a_checkpoint) {
  fsl_test::TempDirectory temp("verify_checkpoint");
  auto made = build_journal(temp, 24, 8192);
  FSL_REQUIRE_OK(made);

  auto checkpoint = made->create_checkpoint();
  FSL_REQUIRE_OK(checkpoint);
  FSL_CHECK_EQ(checkpoint->sequence.value(), made->watermark().value().sequence->value());

  // Commit more after the anchor, so the verification genuinely starts later.
  auto seed = fsl_test::seed_facility(*made);
  FSL_REQUIRE_OK(seed);
  for (std::uint64_t i = 0; i < 5; ++i) {
    FSL_REQUIRE_OK(fsl_test::record_observation(*made, seed->asset, "after", 9000 + i, 4));
  }

  fsl::VerifyRequest request;
  request.scope = fsl::VerifyScope::kFromCheckpoint;
  request.checkpoint_sequence = checkpoint->sequence;
  const fsl::VerifyReport report = made->verify(request);
  FSL_CHECK(report.ok());
  FSL_CHECK_EQ(report.records_verified, std::uint64_t{5});
  FSL_CHECK_EQ(report.last_sequence->value(), made->watermark().value().sequence->value());
}

FSL_TEST(a_verification_from_a_missing_checkpoint_is_not_found) {
  fsl_test::TempDirectory temp("verify_missing_checkpoint");
  auto made = build_journal(temp, 4, 8192);
  FSL_REQUIRE_OK(made);

  fsl::VerifyRequest request;
  request.scope = fsl::VerifyScope::kFromCheckpoint;
  const fsl::VerifyReport without_sequence = made->verify(request);
  FSL_CHECK(without_sequence.status.is_error());
  FSL_CHECK_EQ(without_sequence.status.code(), fsl::ErrorCode::kMissingRequiredAttribute);

  request.checkpoint_sequence = fsl::LedgerSequence(999);
  const fsl::VerifyReport missing = made->verify(request);
  FSL_CHECK(missing.status.is_error());
  FSL_CHECK_EQ(missing.status.code(), fsl::ErrorCode::kNotFound);
}

FSL_TEST(verification_detects_an_index_that_disagrees_with_the_log) {
  fsl_test::TempDirectory temp("verify_index");
  auto made = build_journal(temp, 8, 8192);
  FSL_REQUIRE_OK(made);

  const fsl::VerifyReport clean = made->verify();
  FSL_CHECK(clean.ok());
  FSL_CHECK(clean.index_consistent);

  // Delete the index generation and reopen: the ledger must rebuild it from the
  // log rather than trusting the missing files.
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(temp.child("index"), error)) {
    std::filesystem::remove(entry.path(), error);
  }
  FSL_REQUIRE_OK(made->close());

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  fsl::Ledger ledger = std::move(reopened).value();
  FSL_CHECK(ledger.open_report().index_rebuilt);
  FSL_CHECK(ledger.open_report().replayed_events > 0);

  const fsl::VerifyReport rebuilt = ledger.verify();
  FSL_CHECK(rebuilt.ok());
  FSL_CHECK(rebuilt.index_consistent);
}

FSL_TEST(verification_can_skip_the_index_check) {
  fsl_test::TempDirectory temp("verify_no_index");
  auto made = build_journal(temp, 5, 8192);
  FSL_REQUIRE_OK(made);
  fsl::VerifyRequest request;
  request.verify_index = false;
  const fsl::VerifyReport report = made->verify(request);
  FSL_CHECK(report.ok());
  FSL_CHECK(!report.index_verified);
}

FSL_TEST(verification_honours_a_record_budget) {
  fsl_test::TempDirectory temp("verify_budget");
  auto made = build_journal(temp, 20, 8192);
  FSL_REQUIRE_OK(made);

  fsl::VerifyRequest request;
  request.max_records = 3;
  const fsl::VerifyReport report = made->verify(request);
  // A truncated walk cannot reach the watermark, so the report must say so
  // rather than claim success.
  FSL_CHECK(report.status.is_error());
  FSL_CHECK(report.records_verified >= 3);
  FSL_CHECK(!report.findings.empty());
}

FSL_TEST(verification_reports_verified_segment_metadata) {
  fsl_test::TempDirectory temp("verify_segments");
  auto made = build_journal(temp, 12, 4096);
  FSL_REQUIRE_OK(made);

  auto listing = made->segments();
  FSL_REQUIRE_OK(listing);
  for (const fsl::SegmentInfo& info : listing.value()) {
    auto verified = made->segment(info.index);
    FSL_REQUIRE_OK(verified);
    FSL_CHECK(verified->verified);
    FSL_CHECK_EQ(verified->index.value(), info.index.value());
    FSL_CHECK_EQ(verified->event_count, info.event_count);
  }

  auto missing = made->segment(fsl::SegmentIndex(999));
  FSL_REQUIRE_ERROR(missing, fsl::ErrorCode::kSegmentMissing);
}

}  // namespace

FSL_TEST_MAIN("test_ledger_verify")

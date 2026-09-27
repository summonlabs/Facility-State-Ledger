// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <filesystem>
#include <set>
#include <vector>

#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_ledger_basics.cpp
/// Creation, opening, identity, the committed prefix, and handle lifecycle.

namespace {

FSL_TEST(create_produces_an_identified_ledger_with_one_committed_event) {
  fsl_test::TempDirectory temp("basics_create");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();

  FSL_CHECK(ledger.open_report().created);
  auto identity = ledger.identity();
  FSL_REQUIRE_OK(identity);
  FSL_CHECK(!identity->id.is_nil());
  FSL_CHECK_EQ(identity->segment_format_version, std::uint64_t{1});

  auto watermark = ledger.watermark();
  FSL_REQUIRE_OK(watermark);
  FSL_REQUIRE(watermark->sequence.has_value());
  FSL_CHECK_EQ(watermark->sequence->value(), std::uint64_t{1});
  FSL_CHECK_EQ(watermark->event_count, std::uint64_t{1});

  auto first = ledger.read(fsl::LedgerSequence(1));
  FSL_REQUIRE_OK(first);
  FSL_CHECK(first->kind() == fsl::EventKind::kLedgerOpened);
  FSL_CHECK(!first->epoch().has_value());
  FSL_CHECK(first->facility_generation() == fsl::FacilityGeneration(1));

  auto state = ledger.state();
  FSL_REQUIRE_OK(state);
  FSL_CHECK(!state->open_epoch.has_value());
  FSL_CHECK(!state->latest_epoch.has_value());
  FSL_CHECK(ledger.is_writable());
  FSL_CHECK(ledger.is_durable());

  FSL_REQUIRE_OK(ledger.close());
}

FSL_TEST(identity_is_stable_across_close_and_reopen) {
  fsl_test::TempDirectory temp("basics_identity");
  fsl::LedgerId original;
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    original = ledger.identity().value().id;
    FSL_REQUIRE_OK(ledger.close());
  }
  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  fsl::Ledger ledger = std::move(reopened).value();
  FSL_CHECK(ledger.identity().value().id == original);
  FSL_CHECK(!ledger.open_report().created);
  FSL_REQUIRE_OK(ledger.close());
}

FSL_TEST(opening_a_directory_that_holds_no_ledger_requires_create) {
  fsl_test::TempDirectory temp("basics_missing");
  auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                  fsl_test::deterministic_options());
  FSL_REQUIRE_ERROR(opened, fsl::ErrorCode::kLedgerNotFound);

  auto missing = fsl::Ledger::open(temp.child("does-not-exist"), fsl::OpenMode::kReadWrite,
                                   fsl_test::deterministic_options());
  FSL_REQUIRE_ERROR(missing, fsl::ErrorCode::kLedgerNotFound);
}

FSL_TEST(create_on_an_existing_ledger_opens_it_rather_than_resetting_it) {
  fsl_test::TempDirectory temp("basics_recreate");
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    FSL_REQUIRE_OK(fsl_test::seed_facility(ledger));
    FSL_REQUIRE_OK(ledger.close());
  }
  auto again = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(again);
  fsl::Ledger ledger = std::move(again).value();
  auto watermark = ledger.watermark();
  FSL_REQUIRE_OK(watermark);
  FSL_CHECK(watermark->sequence->value() > 1);
  FSL_CHECK(!ledger.open_report().created);
  FSL_REQUIRE_OK(ledger.close());
}

FSL_TEST(a_second_writable_handle_for_one_directory_is_refused) {
  fsl_test::TempDirectory temp("basics_lock");
  auto first = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(first);
  fsl::Ledger writer = std::move(first).value();

  auto second = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                  fsl_test::deterministic_options());
  FSL_REQUIRE_ERROR(second, fsl::ErrorCode::kLedgerLocked);
  FSL_CHECK_EQ(second.status().category(), fsl::ErrorCategory::kLocked);

  FSL_REQUIRE_OK(writer.close());

  auto after_close = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                       fsl_test::deterministic_options());
  FSL_REQUIRE_OK(after_close);
  FSL_REQUIRE_OK(after_close.value().close());
}

FSL_TEST(a_read_only_handle_coexists_with_a_writer_by_default) {
  fsl_test::TempDirectory temp("basics_reader");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger writer = std::move(created).value();
  FSL_REQUIRE_OK(fsl_test::seed_facility(writer));

  auto reader = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadOnly,
                                  fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reader);
  fsl::Ledger read_handle = std::move(reader).value();
  FSL_CHECK(!read_handle.is_writable());
  FSL_CHECK(!read_handle.is_durable());

  auto watermark = read_handle.watermark();
  FSL_REQUIRE_OK(watermark);
  FSL_CHECK_EQ(watermark->sequence->value(), writer.watermark().value().sequence->value());

  // A reader must never mutate.
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(0xDEADULL, 1).subject(fsl_test::subject_ref(fsl::SubjectKind::kLocation, "l"));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_ERROR(read_handle.append(observation.value()), fsl::ErrorCode::kReadOnlyHandle);

  FSL_REQUIRE_OK(read_handle.close());
  FSL_REQUIRE_OK(writer.close());
}

FSL_TEST(a_shared_reader_lock_excludes_writers_when_requested) {
  fsl_test::TempDirectory temp("basics_shared_reader");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger writer = std::move(created).value();
  FSL_REQUIRE_OK(writer.close());

  fsl::LedgerOptions reader_options = fsl_test::deterministic_options();
  reader_options.reader_lock = fsl::ReaderLockPolicy::kShared;
  auto reader = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadOnly, reader_options);
  FSL_REQUIRE_OK(reader);
  fsl::Ledger read_handle = std::move(reader).value();

  auto blocked = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                   fsl_test::deterministic_options());
  FSL_REQUIRE_ERROR(blocked, fsl::ErrorCode::kLedgerLocked);

  FSL_REQUIRE_OK(read_handle.close());
  auto now_allowed = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                       fsl_test::deterministic_options());
  FSL_REQUIRE_OK(now_allowed);
  FSL_REQUIRE_OK(now_allowed.value().close());
}

FSL_TEST(committed_sequences_are_unique_and_contiguous) {
  fsl_test::TempDirectory temp("basics_sequences");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  FSL_REQUIRE_OK(fsl_test::seed_facility(ledger));
  for (std::uint64_t i = 0; i < 40; ++i) {
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, fsl_test::subject_ref(fsl::SubjectKind::kRack,
                                                                             "site-a.hall-1.row-1.rack-1"),
                                                "obs-" + std::to_string(i), 1000 + i, 8));
  }

  std::set<std::uint64_t> seen;
  for (std::uint64_t sequence = 1; sequence <= ledger.watermark().value().sequence->value(); ++sequence) {
    auto event = ledger.read(fsl::LedgerSequence(sequence));
    FSL_REQUIRE_OK(event);
    FSL_CHECK_EQ(event->sequence().value(), sequence);
    FSL_CHECK(seen.insert(sequence).second);
  }
  FSL_CHECK_EQ(seen.size(), static_cast<std::size_t>(ledger.watermark().value().sequence->value()));
}

FSL_TEST(reading_outside_the_committed_prefix_is_not_found) {
  fsl_test::TempDirectory temp("basics_range");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();

  FSL_REQUIRE_ERROR(ledger.read(fsl::LedgerSequence(0)), fsl::ErrorCode::kNotFound);
  FSL_REQUIRE_ERROR(ledger.read(fsl::LedgerSequence(2)), fsl::ErrorCode::kNotFound);
  FSL_REQUIRE_ERROR(ledger.read(fsl::LedgerSequence(1'000'000)), fsl::ErrorCode::kNotFound);

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  auto identity = builder.fields().event_id.value();
  FSL_REQUIRE_ERROR(ledger.find_event(identity), fsl::ErrorCode::kNotFound);
  FSL_REQUIRE_ERROR(ledger.find_event(fsl::EventId{}), fsl::ErrorCode::kHeaderFieldInvalid);
  FSL_REQUIRE_ERROR(ledger.find_idempotency(fsl::IdempotencyToken{}), fsl::ErrorCode::kHeaderFieldInvalid);
}

FSL_TEST(closed_handles_refuse_every_operation_and_close_is_idempotent) {
  fsl_test::TempDirectory temp("basics_closed");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  FSL_REQUIRE_OK(ledger.close());
  FSL_CHECK(!ledger.is_open());
  FSL_REQUIRE_OK(ledger.close());

  FSL_REQUIRE_ERROR(ledger.state(), fsl::ErrorCode::kLedgerClosed);
  FSL_REQUIRE_ERROR(ledger.watermark(), fsl::ErrorCode::kLedgerClosed);
  FSL_REQUIRE_ERROR(ledger.read(fsl::LedgerSequence(1)), fsl::ErrorCode::kLedgerClosed);
  FSL_REQUIRE_ERROR(ledger.rotate_segment(), fsl::ErrorCode::kLedgerClosed);
  FSL_REQUIRE_ERROR(ledger.create_checkpoint(), fsl::ErrorCode::kLedgerClosed);
  FSL_REQUIRE_ERROR(ledger.flush(), fsl::ErrorCode::kLedgerClosed);
  FSL_REQUIRE_ERROR(ledger.rebuild_index(), fsl::ErrorCode::kLedgerClosed);

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_ERROR(ledger.append(observation.value()), fsl::ErrorCode::kLedgerClosed);

  const fsl::RecoveryReport report = ledger.recover();
  FSL_CHECK(report.status.is_error());
  FSL_CHECK_EQ(report.status.code(), fsl::ErrorCode::kLedgerClosed);
}

FSL_TEST(statistics_track_committed_work) {
  fsl_test::TempDirectory temp("basics_stats");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  const fsl::LedgerStats before = ledger.stats();
  // Creating the ledger committed its own creation record.
  FSL_CHECK_EQ(before.durable_commits, std::uint64_t{1});
  FSL_CHECK(before.durable);
  FSL_CHECK(before.writable);

  FSL_REQUIRE_OK(fsl_test::seed_facility(ledger));
  const fsl::LedgerStats after = ledger.stats();
  FSL_CHECK(after.durable_commits >= 4);
  FSL_CHECK(after.flush_calls >= 4);
  FSL_CHECK(after.dedupe_entries >= 4);
  FSL_CHECK(after.subjects_known >= 3);
  FSL_CHECK(after.committed_events >= 4);
  FSL_CHECK_EQ(after.committed_events, ledger.watermark().value().sequence->value());
}

FSL_TEST(a_volatile_ledger_reports_itself_as_not_durable) {
  fsl_test::TempDirectory temp("basics_volatile");
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.durable_commits = false;
  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  FSL_CHECK(!ledger.is_durable());
  FSL_CHECK(!ledger.stats().durable);
  auto state = ledger.state();
  FSL_REQUIRE_OK(state);
  FSL_CHECK(!state->durable);

  FSL_REQUIRE_OK(fsl_test::seed_facility(ledger));
  FSL_REQUIRE_OK(ledger.close());

  // State written without durability flushes is still consistent after a clean
  // reopen, and the reopened handle reports the durability of its own options.
  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  FSL_CHECK(reopened.value().is_durable());
  auto watermark = reopened.value().watermark();
  FSL_REQUIRE_OK(watermark);
  FSL_CHECK(watermark->sequence->value() > 1);
  FSL_REQUIRE_OK(reopened.value().close());
}

FSL_TEST(options_bounds_are_validated_before_use) {
  fsl_test::TempDirectory temp("basics_options");
  fsl::LedgerOptions zero_postings = fsl_test::deterministic_options();
  zero_postings.max_index_postings = 0;
  FSL_REQUIRE_ERROR(fsl::Ledger::create(temp.child("a"), zero_postings),
                    fsl::ErrorCode::kLimitOutOfRange);

  fsl::LedgerOptions tiny_segment = fsl_test::deterministic_options();
  tiny_segment.max_segment_bytes = 8;
  FSL_REQUIRE_ERROR(fsl::Ledger::create(temp.child("b"), tiny_segment),
                    fsl::ErrorCode::kLimitOutOfRange);

  fsl::LedgerOptions huge_payload = fsl_test::deterministic_options();
  huge_payload.max_payload_bytes = 1ULL << 40;
  FSL_REQUIRE_ERROR(fsl::Ledger::create(temp.child("c"), huge_payload),
                    fsl::ErrorCode::kLimitOutOfRange);
}

FSL_TEST(a_directory_holding_an_unrelated_file_is_not_a_ledger) {
  fsl_test::TempDirectory temp("basics_unrelated");
  std::filesystem::create_directories(temp.child("segments"));
  {
    std::FILE* file = std::fopen(temp.child("segments/segment-0000000000000001.fsl").string().c_str(), "wb");
    FSL_REQUIRE(file != nullptr);
    const char bytes[8] = {'n', 'o', 't', 'a', 's', 'e', 'g', 'm'};
    FSL_CHECK_EQ(std::fwrite(bytes, 1, sizeof(bytes), file), sizeof(bytes));
    std::fclose(file);
  }
  auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                  fsl_test::deterministic_options());
  FSL_REQUIRE_ERROR(opened, fsl::ErrorCode::kManifestRebuilt);
  FSL_CHECK_EQ(opened.status().category(), fsl::ErrorCategory::kRecoveryRequired);
}

}  // namespace

FSL_TEST_MAIN("test_ledger_basics")

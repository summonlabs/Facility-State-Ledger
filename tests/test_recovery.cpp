// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <string>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "detail/frame.hpp"
#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_recovery.cpp
/// Recovery after interrupted writes: unacknowledged tails, an index that ran
/// ahead of the watermark, manifest damage, and the refusal to open a ledger
/// whose committed records are missing.

namespace {

[[nodiscard]] std::filesystem::path segment_path(const std::filesystem::path& root, std::uint64_t index) {
  return root / "segments" /
         ("segment-" + std::string(16 - std::min<std::size_t>(16, std::to_string(index).size()), '0') +
          std::to_string(index) + ".fsl");
}

[[nodiscard]] bool append_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.string().c_str(), "ab");
  if (file == nullptr) {
    return false;
  }
  const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  return written == bytes.size();
}

[[nodiscard]] bool truncate_file(const std::filesystem::path& path, std::uint64_t size) {
  std::error_code error;
  std::filesystem::resize_file(path, size, error);
  return !error;
}

[[nodiscard]] std::uint64_t size_of(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

[[nodiscard]] bool flip_byte(const std::filesystem::path& path, std::uint64_t offset) {
  std::FILE* file = std::fopen(path.string().c_str(), "r+b");
  if (file == nullptr) {
    return false;
  }
  if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }
  const int value = std::fgetc(file);
  if (value == EOF || std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }
  const auto altered = static_cast<unsigned char>(value ^ 0x01);
  const bool ok = std::fputc(altered, file) != EOF;
  std::fclose(file);
  return ok;
}

/// The single index file of a freshly built ledger, discovered rather than
/// assumed so that a layout change is caught by the assertions below.
[[nodiscard]] std::filesystem::path find_index_file(const std::filesystem::path& root) {
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(root / "index", error)) {
    if (entry.is_regular_file()) {
      return entry.path();
    }
  }
  return {};
}
[[nodiscard]] bool overwrite_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  return written == bytes.size();
}

struct JournalState {
  std::uint64_t committed_sequence = 0;
  fsl::Digest chain;
  std::vector<fsl::Digest> integrity;
};

[[nodiscard]] fsl::Result<JournalState> capture(const fsl::Ledger& ledger) {
  JournalState state;
  auto watermark = ledger.watermark();
  if (!watermark.has_value()) {
    return watermark.status();
  }
  state.committed_sequence = watermark->sequence.has_value() ? watermark->sequence->value() : 0;
  state.chain = watermark->chain;
  for (std::uint64_t sequence = 1; sequence <= state.committed_sequence; ++sequence) {
    auto event = ledger.read(fsl::LedgerSequence(sequence));
    if (!event.has_value()) {
      return event.status();
    }
    state.integrity.push_back(event->integrity());
  }
  return state;
}

void check_prefix_preserved(const fsl::Ledger& ledger, const JournalState& expected) {
  const auto watermark = ledger.watermark();
  FSL_REQUIRE(watermark.has_value());
  FSL_CHECK_EQ(watermark->sequence->value(), expected.committed_sequence);
  FSL_CHECK(watermark->chain == expected.chain);
  for (std::uint64_t sequence = 1; sequence <= expected.committed_sequence; ++sequence) {
    const auto event = ledger.read(fsl::LedgerSequence(sequence));
    FSL_REQUIRE(event.has_value());
    FSL_CHECK(event->integrity() == expected.integrity[static_cast<std::size_t>(sequence - 1)]);
  }
}

FSL_TEST(an_unacknowledged_tail_is_discarded_and_the_prefix_is_preserved) {
  fsl_test::TempDirectory temp("recovery_tail");
  JournalState expected;
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    for (std::uint64_t i = 0; i < 6; ++i) {
      FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 100 + i, 8));
    }
    auto captured = capture(ledger);
    FSL_REQUIRE_OK(captured);
    expected = std::move(captured).value();
    FSL_REQUIRE_OK(ledger.close());
  }

  const std::filesystem::path segment = segment_path(temp.path(), 1);
  const std::uint64_t committed_size = size_of(segment);
  // Simulate frames that reached the file but whose commit was never
  // acknowledged: a complete-looking frame followed by a torn one.
  std::vector<std::uint8_t> tail;
  const std::vector<std::uint8_t> tail_body{1, 2, 3};
  fsl::detail::encode_frame(fsl::detail::FrameKind::kEvent, 100, expected.committed_sequence + 1,
                            expected.committed_sequence + 1, tail_body, fsl::Digest{}, tail);
  FSL_REQUIRE(append_bytes(segment, tail));
  FSL_REQUIRE(append_bytes(segment, std::vector<std::uint8_t>{0x41, 0x42, 0x43}));
  FSL_CHECK(size_of(segment) > committed_size);

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  fsl::Ledger ledger = std::move(reopened).value();
  FSL_CHECK(ledger.open_report().recovered);
  FSL_CHECK(ledger.open_report().truncated_tail_bytes > 0);
  FSL_CHECK(!ledger.open_report().notes.empty());
  FSL_CHECK_EQ(size_of(segment), committed_size);
  check_prefix_preserved(ledger, expected);
  FSL_CHECK(ledger.verify().ok());

  // The reclaimed space is usable: the next commit continues the sequence.
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);
  auto appended = fsl_test::record_observation(ledger, seed->asset, "after-recovery", 500, 4);
  FSL_REQUIRE_OK(appended);
  FSL_CHECK_EQ(appended->event.sequence().value(), expected.committed_sequence + 1);
}

FSL_TEST(the_recovery_policy_can_refuse_to_discard_an_unacknowledged_tail) {
  fsl_test::TempDirectory temp("recovery_refuse");
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    FSL_REQUIRE_OK(ledger.close());
  }
  const std::filesystem::path segment = segment_path(temp.path(), 1);
  const std::uint64_t committed_size = size_of(segment);
  FSL_REQUIRE(append_bytes(segment, std::vector<std::uint8_t>{9, 9, 9, 9}));

  fsl::LedgerOptions strict = fsl_test::deterministic_options();
  strict.recovery = fsl::RecoveryPolicy::kRefuseOnUncommittedTail;
  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite, strict);
  FSL_REQUIRE_ERROR(reopened, fsl::ErrorCode::kRecoveryPolicyRejected);
  FSL_CHECK_EQ(reopened.status().category(), fsl::ErrorCategory::kRecoveryRequired);
  // The refused open must not have modified anything.
  FSL_CHECK_EQ(size_of(segment), committed_size + 4);

  // The permissive policy still opens the same directory.
  auto permissive = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                      fsl_test::deterministic_options());
  FSL_REQUIRE_OK(permissive);
  FSL_CHECK_EQ(size_of(segment), committed_size);
  FSL_REQUIRE_OK(permissive.value().close());
}

FSL_TEST(a_dry_run_recovers_nothing_and_changes_nothing) {
  fsl_test::TempDirectory temp("recovery_dry_run");
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    FSL_REQUIRE_OK(ledger.close());
  }
  const std::filesystem::path segment = segment_path(temp.path(), 1);
  const std::uint64_t committed_size = size_of(segment);
  FSL_REQUIRE(append_bytes(segment, std::vector<std::uint8_t>{1, 2, 3}));

  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.recovery = fsl::RecoveryPolicy::kRefuseOnUncommittedTail;
  auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite, options);
  FSL_REQUIRE_ERROR(opened, fsl::ErrorCode::kRecoveryPolicyRejected);

  // A permissive handle performs the dry run.
  auto permissive = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                      fsl_test::deterministic_options());
  FSL_REQUIRE_OK(permissive);
  fsl::Ledger ledger = std::move(permissive).value();
  FSL_CHECK_EQ(size_of(segment), committed_size);  // already truncated at open

  fsl::RecoveryRequest request;
  request.dry_run = true;
  const fsl::RecoveryReport report = ledger.recover(request);
  FSL_CHECK(report.status.is_ok());
  FSL_CHECK(report.dry_run);
  FSL_CHECK(!report.truncated);
  FSL_CHECK_EQ(report.truncated_tail_bytes, std::uint64_t{0});

  const fsl::RecoveryReport real = ledger.recover();
  FSL_CHECK(real.status.is_ok());
  FSL_CHECK(!real.truncated);
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(an_index_that_ran_ahead_of_the_watermark_is_trimmed) {
  fsl_test::TempDirectory temp("recovery_index_ahead");
  JournalState expected;
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 100, 4));
    auto captured = capture(ledger);
    FSL_REQUIRE_OK(captured);
    expected = std::move(captured).value();
    FSL_REQUIRE_OK(ledger.close());
  }

  // Append an index entry for a sequence the manifest never acknowledged. This
  // is exactly the state a crash between the segment flush and the index write
  // can leave behind in reverse: an index that is ahead of the log.
  const std::filesystem::path index_file = find_index_file(temp.path());
  FSL_REQUIRE(!index_file.empty());
  fsl::detail::IndexEntry entry;
  entry.kind = fsl::detail::IndexKind::kEventId;
  entry.sequence = expected.committed_sequence + 5;
  entry.extra = static_cast<std::uint64_t>(fsl::EventKind::kObservationAccepted);
  entry.key.assign(16, 0x5A);
  std::vector<std::uint8_t> frame;
  fsl::detail::encode_frame(fsl::detail::FrameKind::kIndexEntry, 900, entry.sequence, 0,
                            fsl::detail::encode_index_entry(entry), fsl::Digest{}, frame);
  const std::uint64_t before = size_of(index_file);
  FSL_REQUIRE(append_bytes(index_file, frame));

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  fsl::Ledger ledger = std::move(reopened).value();
  FSL_CHECK(ledger.open_report().index_truncated);
  FSL_CHECK_EQ(size_of(index_file), before);
  check_prefix_preserved(ledger, expected);
  FSL_CHECK(ledger.verify().ok());

  // Derived state must not have absorbed the phantom entry.
  FSL_REQUIRE_ERROR(ledger.find_event(fsl::EventId::from_span(entry.key)),
                    fsl::ErrorCode::kNotFound);
}

FSL_TEST(a_corrupt_index_is_rebuilt_from_the_authoritative_log) {
  fsl_test::TempDirectory temp("recovery_index_corrupt");
  JournalState expected;
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    for (std::uint64_t i = 0; i < 5; ++i) {
      FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 100 + i, 4));
    }
    auto captured = capture(ledger);
    FSL_REQUIRE_OK(captured);
    expected = std::move(captured).value();
    FSL_REQUIRE_OK(ledger.close());
  }

  const std::filesystem::path index_file = find_index_file(temp.path());
  FSL_REQUIRE(!index_file.empty());
  FSL_REQUIRE(size_of(index_file) > 24);
  FSL_REQUIRE(flip_byte(index_file, 12));

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  fsl::Ledger ledger = std::move(reopened).value();
  FSL_CHECK(ledger.open_report().index_rebuilt);
  FSL_CHECK(ledger.open_report().replayed_events > 0);
  check_prefix_preserved(ledger, expected);
  FSL_CHECK(ledger.verify().ok());

  fsl::Query query;
  query.kind = fsl::EventKind::kObservationAccepted;
  query.limit = 100;
  auto page = ledger.query(query);
  FSL_REQUIRE_OK(page);
  FSL_CHECK_EQ(page->events.size(), std::size_t{5});
}

FSL_TEST(a_damaged_primary_manifest_falls_back_to_the_previous_publication) {
  fsl_test::TempDirectory temp("recovery_manifest_backup");
  JournalState expected;
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    for (std::uint64_t i = 0; i < 4; ++i) {
      FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 100 + i, 4));
    }
    auto captured = capture(ledger);
    FSL_REQUIRE_OK(captured);
    expected = std::move(captured).value();
    FSL_REQUIRE_OK(ledger.close());
  }

  // The backup holds the publication from before the last commit, so a fallback
  // must open at that earlier watermark and treat the extra frames as a tail.
  const std::filesystem::path manifest = temp.path() / "ledger.manifest";
  FSL_REQUIRE(overwrite_file(manifest, std::vector<std::uint8_t>(64, 0xFF)));

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  fsl::Ledger ledger = std::move(reopened).value();
  FSL_CHECK(ledger.open_report().manifest_recovered_from_backup);
  const auto watermark = ledger.watermark();
  FSL_REQUIRE_OK(watermark);
  FSL_CHECK(watermark->sequence.has_value());
  FSL_CHECK(watermark->sequence->value() <= expected.committed_sequence);
  FSL_CHECK(watermark->sequence->value() >= 1);
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(a_ledger_with_no_usable_manifest_requires_explicit_recovery) {
  fsl_test::TempDirectory temp("recovery_manifest_lost");
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    FSL_REQUIRE_OK(ledger.close());
  }
  FSL_REQUIRE(overwrite_file(temp.path() / "ledger.manifest", std::vector<std::uint8_t>(32, 0x00)));
  FSL_REQUIRE(overwrite_file(temp.path() / "ledger.manifest.bak", std::vector<std::uint8_t>(32, 0x11)));

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_ERROR(reopened, fsl::ErrorCode::kManifestRebuilt);
  FSL_CHECK_EQ(reopened.status().category(), fsl::ErrorCategory::kRecoveryRequired);
  FSL_CHECK(reopened.status().message().find("manifest") != std::string::npos);
}

FSL_TEST(a_missing_committed_record_refuses_to_open) {
  fsl_test::TempDirectory temp("recovery_lost_record");
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    for (std::uint64_t i = 0; i < 4; ++i) {
      FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 100 + i, 4));
    }
    FSL_REQUIRE_OK(ledger.close());
  }
  const std::filesystem::path segment = segment_path(temp.path(), 1);
  const std::uint64_t size = size_of(segment);
  FSL_REQUIRE(size > 200);
  FSL_REQUIRE(truncate_file(segment, size / 2));

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_ERROR(reopened, fsl::ErrorCode::kCommittedRecordLost);
  FSL_CHECK_EQ(reopened.status().category(), fsl::ErrorCategory::kIntegrityFailure);
}

FSL_TEST(a_mid_file_corruption_refuses_to_open) {
  fsl_test::TempDirectory temp("recovery_mid_corruption");
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    for (std::uint64_t i = 0; i < 6; ++i) {
      FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 100 + i, 8));
    }
    FSL_REQUIRE_OK(ledger.close());
  }
  const std::filesystem::path segment = segment_path(temp.path(), 1);
  // Flip a byte inside the payload region of the first record.
  FSL_REQUIRE(flip_byte(segment, 120));

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_ERROR(reopened, fsl::ErrorCode::kMidFileCorruption);
  FSL_CHECK_EQ(reopened.status().category(), fsl::ErrorCategory::kIntegrityFailure);

  // The damaged ledger can still be inspected, and the diagnosis names the
  // segment and offset.
  auto diagnostic = fsl::Ledger::open(temp.path(), fsl::OpenMode::kDiagnose,
                                      fsl_test::deterministic_options());
  FSL_REQUIRE_OK(diagnostic);
  fsl::Ledger inspect = std::move(diagnostic).value();
  FSL_CHECK(!inspect.is_writable());
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_ERROR(inspect.append(observation.value()), fsl::ErrorCode::kReadOnlyHandle);
  const fsl::VerifyReport report = inspect.verify();
  FSL_CHECK(!report.ok());
  FSL_CHECK(!report.findings.empty());
  FSL_REQUIRE_OK(inspect.close());
}

FSL_TEST(recovery_is_idempotent_and_repeated_open_is_stable) {
  fsl_test::TempDirectory temp("recovery_repeat");
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    FSL_REQUIRE_OK(ledger.close());
  }
  const std::filesystem::path segment = segment_path(temp.path(), 1);
  FSL_REQUIRE(append_bytes(segment, std::vector<std::uint8_t>{7, 7, 7, 7, 7, 7, 7, 7}));
  const std::uint64_t committed = size_of(segment) - 8;

  for (int round = 0; round < 4; ++round) {
    auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
    FSL_REQUIRE_OK(opened);
    fsl::Ledger ledger = std::move(opened).value();
    if (round == 0) {
      FSL_CHECK(ledger.open_report().recovered);
    } else {
      FSL_CHECK(!ledger.open_report().recovered);
    }
    FSL_CHECK_EQ(size_of(segment), committed);
    FSL_CHECK(ledger.verify().ok());
    FSL_REQUIRE_OK(ledger.close());
  }
}

}  // namespace

FSL_TEST_MAIN("test_recovery")

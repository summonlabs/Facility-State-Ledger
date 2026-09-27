// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "detail/frame.hpp"
#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_corruption.cpp
/// Adversarial persistence: truncation, bit flips, forged headers, foreign
/// segments, and tails crafted to look like committed records.

namespace {

[[nodiscard]] std::filesystem::path segment_path(const std::filesystem::path& root, std::uint64_t index) {
  std::string digits = std::to_string(index);
  digits.insert(0, 16 - std::min<std::size_t>(16, digits.size()), '0');
  return root / "segments" / ("segment-" + digits + ".fsl");
}

[[nodiscard]] bool write_at(const std::filesystem::path& path,
                            std::uint64_t offset,
                            const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.string().c_str(), "r+b");
  if (file == nullptr) {
    return false;
  }
  if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }
  const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  return written == bytes.size();
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::vector<std::uint8_t> bytes;
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return bytes;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size > 0) {
    bytes.resize(static_cast<std::size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
      bytes.clear();
    }
  }
  std::fclose(file);
  return bytes;
}

[[nodiscard]] bool overwrite(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  return written == bytes.size();
}

/// Builds a ledger with a known committed prefix and returns after closing it.
void build(fsl_test::TempDirectory& temp, std::uint64_t events, std::size_t segment_bytes = 64u * 1024u) {
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.max_segment_bytes = segment_bytes;
  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);
  for (std::uint64_t i = 0; i < events; ++i) {
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs-" + std::to_string(i),
                                                300 + i, 24));
  }
  FSL_REQUIRE_OK(ledger.close());
}

FSL_TEST(every_committed_byte_is_protected_by_the_record_chain) {
  fsl_test::TempDirectory temp("corruption_bytes");
  build(temp, 8);
  const std::filesystem::path segment = segment_path(temp.path(), 1);
  const std::vector<std::uint8_t> original = read_file(segment);
  FSL_REQUIRE(original.size() > fsl::detail::kSegmentHeaderSize + 100);

  // Walk a sample of offsets across the committed region. Some alterations are
  // caught by the frame checksum, others by the chain; every one of them makes
  // the ledger refuse to open.
  for (std::uint64_t offset = fsl::detail::kSegmentHeaderSize + 12; offset < original.size();
       offset += 37) {
    std::vector<std::uint8_t> altered = original;
    altered[static_cast<std::size_t>(offset)] ^= 0x80;
    FSL_REQUIRE(overwrite(segment, altered));

    auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
    FSL_CHECK(!opened.has_value());
    if (!opened.has_value()) {
      const fsl::ErrorCategory category = opened.status().category();
      FSL_CHECK(category == fsl::ErrorCategory::kIntegrityFailure ||
                category == fsl::ErrorCategory::kMalformedInput ||
                category == fsl::ErrorCategory::kUnsupportedVersion);
    }
    FSL_REQUIRE(overwrite(segment, original));
  }

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  FSL_CHECK(reopened.value().verify().ok());
}

FSL_TEST(a_forged_tail_frame_is_discarded_rather_than_adopted) {
  fsl_test::TempDirectory temp("corruption_forged_tail");
  std::uint64_t committed = 0;
  std::vector<uint8_t> chain;
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 400, 8));
    committed = ledger.watermark().value().sequence->value();
    FSL_REQUIRE_OK(ledger.close());
  }

  // A structurally valid frame that no commit ever acknowledged.
  std::vector<std::uint8_t> frame;
  fsl::detail::encode_frame(fsl::detail::FrameKind::kEvent, 900, committed + 1, committed + 1,
                            std::vector<std::uint8_t>(32, 0x7E), fsl::Digest{}, frame);
  const std::filesystem::path segment = segment_path(temp.path(), 1);
  std::vector<std::uint8_t> contents = read_file(segment);
  contents.insert(contents.end(), frame.begin(), frame.end());
  FSL_REQUIRE(overwrite(segment, contents));

  auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                  fsl_test::deterministic_options());
  FSL_REQUIRE_OK(opened);
  fsl::Ledger ledger = std::move(opened).value();
  FSL_CHECK_EQ(ledger.watermark().value().sequence->value(), committed);
  FSL_CHECK(ledger.open_report().truncated_tail_bytes >= frame.size());
  FSL_REQUIRE_ERROR(ledger.read(fsl::LedgerSequence(committed + 1)), fsl::ErrorCode::kNotFound);
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(a_segment_from_another_ledger_is_refused) {
  fsl_test::TempDirectory first("corruption_foreign_a");
  fsl_test::TempDirectory second("corruption_foreign_b");
  build(first, 3);
  build(second, 3);

  const std::vector<std::uint8_t> foreign = read_file(segment_path(second.path(), 1));
  FSL_REQUIRE(!foreign.empty());
  FSL_REQUIRE(overwrite(segment_path(first.path(), 1), foreign));

  auto opened = fsl::Ledger::open(first.path(), fsl::OpenMode::kReadWrite,
                                  fsl_test::deterministic_options());
  FSL_REQUIRE_ERROR(opened, fsl::ErrorCode::kLedgerIdentityMismatch);
  FSL_CHECK_EQ(opened.status().category(), fsl::ErrorCategory::kIntegrityFailure);
}

FSL_TEST(a_segment_whose_declared_index_is_wrong_is_refused) {
  fsl_test::TempDirectory temp("corruption_segment_index");
  build(temp, 3, 2000);
  const std::filesystem::path second = segment_path(temp.path(), 2);
  FSL_REQUIRE(std::filesystem::exists(second));

  std::vector<std::uint8_t> contents = read_file(second);
  FSL_REQUIRE(contents.size() > fsl::detail::kSegmentHeaderSize);
  // Rewrite the segment index field while keeping the header checksum valid.
  const std::uint64_t wrong_index = 9;
  for (std::size_t i = 0; i < 8; ++i) {
    contents[32 + i] = static_cast<std::uint8_t>((wrong_index >> (8 * i)) & 0xFFu);
  }
  const std::uint32_t crc = fsl::crc32c(contents.data(), fsl::detail::kSegmentHeaderSize - 4);
  for (std::size_t i = 0; i < 4; ++i) {
    contents[fsl::detail::kSegmentHeaderSize - 4 + i] =
        static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFu);
  }
  FSL_REQUIRE(overwrite(second, contents));

  auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                  fsl_test::deterministic_options());
  FSL_REQUIRE_ERROR(opened, fsl::ErrorCode::kSegmentIdentityMismatch);
}

FSL_TEST(a_missing_segment_is_refused) {
  fsl_test::TempDirectory temp("corruption_missing_segment");
  build(temp, 3, 2000);
  std::error_code error;
  FSL_REQUIRE(std::filesystem::remove(segment_path(temp.path(), 2), error));
  FSL_CHECK(!error);

  auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                  fsl_test::deterministic_options());
  FSL_REQUIRE_ERROR(opened, fsl::ErrorCode::kSegmentMissing);
}

FSL_TEST(an_absurd_frame_length_is_rejected_without_allocating) {
  fsl_test::TempDirectory temp("corruption_length");
  build(temp, 3);
  const std::filesystem::path segment = segment_path(temp.path(), 1);
  std::vector<std::uint8_t> contents = read_file(segment);
  FSL_REQUIRE(contents.size() > 200);

  // Declare a four-gigabyte frame in the first record.
  const std::vector<std::uint8_t> huge{0xFF, 0xFF, 0xFF, 0xFF};
  FSL_REQUIRE(write_at(segment, fsl::detail::kSegmentHeaderSize, huge));

  auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                  fsl_test::deterministic_options());
  FSL_CHECK(!opened.has_value());
  if (!opened.has_value()) {
    FSL_CHECK(opened.status().is_error());
  }
}

FSL_TEST(a_corrupt_checkpoint_is_reported_rather_than_trusted) {
  fsl_test::TempDirectory temp("corruption_checkpoint");
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    FSL_REQUIRE_OK(ledger.create_checkpoint());
    FSL_REQUIRE_OK(ledger.close());
  }
  std::error_code error;
  std::filesystem::path checkpoint;
  for (const auto& entry : std::filesystem::directory_iterator(temp.child("checkpoints"), error)) {
    checkpoint = entry.path();
  }
  FSL_REQUIRE(!checkpoint.empty());

  std::vector<std::uint8_t> contents = read_file(checkpoint);
  FSL_REQUIRE(contents.size() > 64);
  contents[60] ^= 0xFF;
  FSL_REQUIRE(overwrite(checkpoint, contents));

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  auto checkpoints = reopened.value().checkpoints();
  FSL_REQUIRE_OK(checkpoints);
  FSL_CHECK(checkpoints->empty());

  fsl::VerifyRequest request;
  request.scope = fsl::VerifyScope::kFromCheckpoint;
  request.checkpoint_sequence = fsl::LedgerSequence(1);
  const fsl::VerifyReport report = reopened.value().verify(request);
  FSL_CHECK(report.status.is_error());
  FSL_REQUIRE_OK(reopened.value().close());
}

FSL_TEST(a_truncated_manifest_is_rejected_and_the_backup_is_used) {
  fsl_test::TempDirectory temp("corruption_manifest");
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    FSL_REQUIRE_OK(ledger.close());
  }
  std::vector<std::uint8_t> manifest = read_file(temp.path() / "ledger.manifest");
  FSL_REQUIRE(manifest.size() > 100);
  manifest.resize(manifest.size() / 2);
  FSL_REQUIRE(overwrite(temp.path() / "ledger.manifest", manifest));

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  FSL_CHECK(reopened.value().open_report().manifest_recovered_from_backup);
  FSL_CHECK(reopened.value().verify().ok() || !reopened.value().open_report().notes.empty());
  FSL_REQUIRE_OK(reopened.value().close());
}

FSL_TEST(a_directory_named_like_a_segment_file_is_rejected) {
  fsl_test::TempDirectory temp("corruption_directory");
  build(temp, 2);
  std::error_code error;
  std::filesystem::remove(segment_path(temp.path(), 1), error);
  FSL_REQUIRE(std::filesystem::create_directories(segment_path(temp.path(), 1), error));
  FSL_CHECK(!error);

  auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                  fsl_test::deterministic_options());
  FSL_CHECK(!opened.has_value());
  if (!opened.has_value()) {
    FSL_CHECK(opened.status().category() == fsl::ErrorCategory::kInvalidArgument ||
              opened.status().category() == fsl::ErrorCategory::kIntegrityFailure ||
              opened.status().category() == fsl::ErrorCategory::kIoFailure);
  }
}

FSL_TEST(a_ledger_survives_repeated_open_close_cycles) {
  fsl_test::TempDirectory temp("corruption_cycles");
  build(temp, 6, 4096);
  std::uint64_t expected = 0;
  {
    auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
    FSL_REQUIRE_OK(opened);
    expected = opened.value().watermark().value().sequence->value();
    FSL_REQUIRE_OK(opened.value().close());
  }
  for (int round = 0; round < 8; ++round) {
    auto opened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
    FSL_REQUIRE_OK(opened);
    fsl::Ledger ledger = std::move(opened).value();
    FSL_CHECK_EQ(ledger.watermark().value().sequence->value(), expected);
    FSL_CHECK(ledger.verify().ok());
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    auto appended = fsl_test::record_observation(ledger, seed->asset, "cycle", 700 + static_cast<std::uint64_t>(round), 4);
    FSL_REQUIRE_OK(appended);
    FSL_CHECK_EQ(appended->event.sequence().value(), expected + 1);
    expected += 1;
    FSL_REQUIRE_OK(ledger.close());
  }
}

}  // namespace

FSL_TEST_MAIN("test_corruption")

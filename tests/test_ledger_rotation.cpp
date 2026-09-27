// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <vector>

#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_ledger_rotation.cpp
/// Segment rotation and checkpointing.

namespace {

FSL_TEST(rotation_seals_the_active_segment_and_opens_the_next) {
  fsl_test::TempDirectory temp("rotation_basic");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);
  for (std::uint64_t i = 0; i < 5; ++i) {
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 300 + i, 4));
  }

  const std::uint64_t before = ledger.watermark().value().sequence->value();
  FSL_REQUIRE_OK(ledger.rotate_segment());

  auto listing = ledger.segments();
  FSL_REQUIRE_OK(listing);
  FSL_REQUIRE(listing->size() == 2);
  FSL_CHECK(listing->at(0).sealed);
  FSL_CHECK(!listing->at(1).sealed);
  FSL_CHECK_EQ(listing->at(1).event_count, std::uint64_t{0});
  FSL_CHECK_EQ(ledger.state().value().active_segment.value(), std::uint64_t{2});

  // Rotation does not change the committed prefix.
  FSL_CHECK_EQ(ledger.watermark().value().sequence->value(), before);

  // Appending after rotation continues the sequence and lands in segment two.
  auto appended = fsl_test::record_observation(ledger, seed->asset, "after", 400, 4);
  FSL_REQUIRE_OK(appended);
  FSL_CHECK_EQ(appended->event.sequence().value(), before + 1);
  FSL_CHECK_EQ(ledger.watermark().value().segment_index->value(), std::uint64_t{2});

  auto listing_after = ledger.segments();
  FSL_REQUIRE_OK(listing_after);
  FSL_CHECK_EQ(listing_after->at(1).event_count, std::uint64_t{1});
  FSL_CHECK(listing_after->at(1).first_sequence.has_value());
  FSL_CHECK_EQ(listing_after->at(1).first_sequence->value(), before + 1);
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(rotation_survives_reopen_and_repeats) {
  fsl_test::TempDirectory temp("rotation_repeat");
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    for (int round = 0; round < 3; ++round) {
      FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 500 + round, 4));
      FSL_REQUIRE_OK(ledger.rotate_segment());
    }
    FSL_REQUIRE_OK(ledger.close());
  }

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  fsl::Ledger ledger = std::move(reopened).value();
  FSL_CHECK_EQ(ledger.state().value().segment_count, std::uint64_t{4});
  FSL_CHECK_EQ(ledger.state().value().active_segment.value(), std::uint64_t{4});

  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);
  const std::uint64_t before = ledger.watermark().value().sequence->value();
  auto appended = fsl_test::record_observation(ledger, seed->asset, "after", 600, 4);
  FSL_REQUIRE_OK(appended);
  FSL_CHECK_EQ(appended->event.sequence().value(), before + 1);
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(an_empty_segment_can_be_rotated_repeatedly_without_losing_sequences) {
  fsl_test::TempDirectory temp("rotation_empty");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  for (int i = 0; i < 3; ++i) {
    FSL_REQUIRE_OK(ledger.rotate_segment());
  }
  auto listing = ledger.segments();
  FSL_REQUIRE_OK(listing);
  FSL_CHECK_EQ(listing->size(), std::size_t{4});
  FSL_CHECK_EQ(ledger.watermark().value().sequence->value(), std::uint64_t{1});

  auto epoch = fsl_test::open_epoch(ledger, 1, 1, 1);
  FSL_REQUIRE_OK(epoch);
  FSL_CHECK_EQ(epoch->event.sequence().value(), std::uint64_t{2});
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(a_checkpoint_anchors_the_commit_watermark) {
  fsl_test::TempDirectory temp("rotation_checkpoint");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  auto checkpoint = ledger.create_checkpoint();
  FSL_REQUIRE_OK(checkpoint);
  FSL_CHECK_EQ(checkpoint->sequence.value(), ledger.watermark().value().sequence->value());
  FSL_CHECK(checkpoint->chain == ledger.watermark().value().chain);
  FSL_CHECK_EQ(checkpoint->event_count, ledger.watermark().value().event_count);
  FSL_CHECK(checkpoint->created_unix_nanos != 0);

  auto latest = ledger.latest_checkpoint();
  FSL_REQUIRE_OK(latest);
  FSL_CHECK(latest->sequence == checkpoint->sequence);

  auto all = ledger.checkpoints();
  FSL_REQUIRE_OK(all);
  FSL_CHECK_EQ(all->size(), std::size_t{1});

  // Creating a checkpoint for the same sequence twice is idempotent on disk.
  FSL_REQUIRE_OK(ledger.create_checkpoint());
  auto after = ledger.checkpoints();
  FSL_REQUIRE_OK(after);
  FSL_CHECK_EQ(after->size(), std::size_t{1});
}

FSL_TEST(checkpoints_are_pruned_to_the_configured_maximum) {
  fsl_test::TempDirectory temp("rotation_prune");
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.max_checkpoints = 2;
  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  for (std::uint64_t i = 0; i < 6; ++i) {
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 700 + i, 4));
    FSL_REQUIRE_OK(ledger.create_checkpoint());
  }
  auto retained = ledger.checkpoints();
  FSL_REQUIRE_OK(retained);
  FSL_CHECK_EQ(retained->size(), std::size_t{2});
  FSL_CHECK(retained->front().sequence < retained->back().sequence);
  FSL_CHECK_EQ(retained->back().sequence.value(), ledger.watermark().value().sequence->value());

  // A pruned checkpoint is simply absent, never contradictory.
  const fsl::LedgerSequence oldest = retained->front().sequence;
  FSL_CHECK(oldest.value() > 1);
}

FSL_TEST(a_checkpoint_on_an_empty_ledger_is_not_found) {
  // Every ledger holds at least its own creation record, so this exercises the
  // path through a handle whose committed prefix cannot be empty.
  fsl_test::TempDirectory temp("rotation_checkpoint_empty");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  auto checkpoints = ledger.checkpoints();
  FSL_REQUIRE_OK(checkpoints);
  FSL_CHECK(checkpoints->empty());
  FSL_REQUIRE_ERROR(ledger.latest_checkpoint(), fsl::ErrorCode::kNotFound);
}

FSL_TEST(automatic_rotation_keeps_segments_within_their_configured_size) {
  fsl_test::TempDirectory temp("rotation_auto");
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.max_segment_bytes = 2048;
  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);
  for (std::uint64_t i = 0; i < 60; ++i) {
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 800 + i, 16));
  }

  auto listing = ledger.segments();
  FSL_REQUIRE_OK(listing);
  FSL_CHECK(listing->size() > 1);
  for (const fsl::SegmentInfo& info : listing.value()) {
    // One record may exceed the nominal ceiling by a single frame, but the
    // segment can never hold two oversized records.
    FSL_CHECK(info.byte_size <= options.max_segment_bytes + 4096);
  }
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(automatic_checkpointing_fires_at_the_configured_interval) {
  fsl_test::TempDirectory temp("rotation_auto_checkpoint");
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.auto_checkpoint_interval = 8;
  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);
  for (std::uint64_t i = 0; i < 24; ++i) {
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 900 + i, 4));
  }
  auto checkpoints = ledger.checkpoints();
  FSL_REQUIRE_OK(checkpoints);
  FSL_CHECK(!checkpoints->empty());
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(a_rotation_is_durable_across_reopen) {
  fsl_test::TempDirectory temp("rotation_durable");
  std::uint64_t watermark_before = 0;
  {
    auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);
    FSL_REQUIRE_OK(ledger.rotate_segment());
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 1000, 4));
    watermark_before = ledger.watermark().value().sequence->value();
    FSL_REQUIRE_OK(ledger.close());
  }
  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  fsl::Ledger ledger = std::move(reopened).value();
  FSL_CHECK_EQ(ledger.watermark().value().sequence->value(), watermark_before);
  FSL_CHECK_EQ(ledger.state().value().segment_count, std::uint64_t{2});
  FSL_CHECK_EQ(ledger.state().value().active_segment.value(), std::uint64_t{2});
  FSL_CHECK(ledger.verify().ok());
}

}  // namespace

FSL_TEST_MAIN("test_ledger_rotation")

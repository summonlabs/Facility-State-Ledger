// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <vector>

#include "detail/frame.hpp"
#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_ledger_batches.cpp
/// Atomic batch semantics: all-or-nothing publication, ordering, and the
/// interaction between a batch and the commit watermark.

namespace {

using fsl_test::subject_ref;

[[nodiscard]] fsl::SubmittedObservation observation_for(const fsl::SubjectRef& subject,
                                                        std::uint64_t index,
                                                        const std::string& reference) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(0xBA7CULL, index).subject(subject).payload(
      fsl::payload::encode(fsl::payload::ObservationAccepted{reference, {1, 2, 3}}));
  return std::move(builder.build()).value();
}

FSL_TEST(a_batch_commits_every_submission_in_order) {
  fsl_test::TempDirectory temp("batch_order");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  const std::uint64_t before = ledger.watermark().value().sequence->value();
  std::vector<fsl::SubmittedObservation> batch;
  for (std::uint64_t i = 0; i < 16; ++i) {
    batch.push_back(observation_for(seed->asset, i, "obs-" + std::to_string(i)));
  }

  auto outcome = ledger.append_batch(batch);
  FSL_REQUIRE_OK(outcome);
  FSL_CHECK_EQ(outcome->events.size(), std::size_t{16});
  FSL_CHECK_EQ(outcome->duplicates, std::uint64_t{0});
  FSL_CHECK(!outcome->nothing_written);
  for (std::size_t i = 0; i < outcome->events.size(); ++i) {
    FSL_CHECK_EQ(outcome->events[i].sequence().value(), before + 1 + i);
    FSL_CHECK_EQ(outcome->events[i].accepted_at().logical_tick.value(), before + 1 + i);
  }
  FSL_CHECK_EQ(ledger.watermark().value().sequence->value(), before + 16);
}

FSL_TEST(a_batch_containing_one_invalid_submission_commits_nothing) {
  fsl_test::TempDirectory temp("batch_atomic");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  const std::uint64_t before = ledger.watermark().value().sequence->value();
  const fsl::Digest chain_before = ledger.watermark().value().chain;
  const std::uint64_t segments_before = ledger.segments().value().front().byte_size;

  std::vector<fsl::SubmittedObservation> batch;
  batch.push_back(observation_for(seed->asset, 0, "obs-0"));
  batch.push_back(observation_for(seed->asset, 1, "obs-1"));
  // The third submission targets a subject that does not exist.
  batch.push_back(observation_for(subject_ref(fsl::SubjectKind::kAsset, "site-a.ghost"), 2, "obs-2"));
  batch.push_back(observation_for(seed->asset, 3, "obs-3"));

  auto outcome = ledger.append_batch(batch);
  FSL_REQUIRE_ERROR(outcome, fsl::ErrorCode::kSubjectNotRegistered);
  FSL_CHECK_EQ(ledger.watermark().value().sequence->value(), before);
  FSL_CHECK(ledger.watermark().value().chain == chain_before);
  // Read the committed extent into plain values: binding a reference into a
  // temporary snapshot would outlive the snapshot.
  const std::uint64_t size_after_rejection = ledger.segments().value().front().byte_size;
  FSL_CHECK_EQ(size_after_rejection, segments_before);

  // Nothing from the failed batch may be found afterwards.
  for (std::uint64_t i = 0; i < 4; ++i) {
    fsl::EventId identity = fsl::EventId::from_words(0xBA7CULL, i);
    FSL_REQUIRE_ERROR(ledger.find_event(identity), fsl::ErrorCode::kNotFound);
  }
}

FSL_TEST(an_empty_or_oversized_batch_is_rejected) {
  fsl_test::TempDirectory temp("batch_bounds");
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.max_batch_events = 4;
  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  const std::vector<fsl::SubmittedObservation> empty;
  FSL_REQUIRE_ERROR(ledger.append_batch(empty), fsl::ErrorCode::kBatchEmpty);

  std::vector<fsl::SubmittedObservation> too_many;
  for (std::uint64_t i = 0; i < 5; ++i) {
    too_many.push_back(observation_for(seed->asset, i, "obs"));
  }
  FSL_REQUIRE_ERROR(ledger.append_batch(too_many), fsl::ErrorCode::kBatchTooLarge);
}

FSL_TEST(a_batch_may_mix_retries_with_new_events) {
  fsl_test::TempDirectory temp("batch_mixed");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  std::vector<fsl::SubmittedObservation> first;
  first.push_back(observation_for(seed->asset, 0, "obs-0"));
  first.push_back(observation_for(seed->asset, 1, "obs-1"));
  auto initial = ledger.append_batch(first);
  FSL_REQUIRE_OK(initial);

  // Retrying the whole batch plus two new events: the retried pair is answered
  // idempotently and only the new pair is written.
  std::vector<fsl::SubmittedObservation> retry;
  retry.push_back(observation_for(seed->asset, 0, "obs-0"));
  retry.push_back(observation_for(seed->asset, 1, "obs-1"));
  retry.push_back(observation_for(seed->asset, 2, "obs-2"));
  retry.push_back(observation_for(seed->asset, 3, "obs-3"));
  auto outcome = ledger.append_batch(retry);
  FSL_REQUIRE_OK(outcome);
  FSL_CHECK_EQ(outcome->events.size(), std::size_t{4});
  FSL_CHECK_EQ(outcome->duplicates, std::uint64_t{2});
  FSL_CHECK(!outcome->nothing_written);
  FSL_CHECK(outcome->events[0].sequence() == initial->events[0].sequence());
  FSL_CHECK(outcome->events[1].sequence() == initial->events[1].sequence());

  // Retrying the whole batch again writes nothing at all.
  auto repeated = ledger.append_batch(retry);
  FSL_REQUIRE_OK(repeated);
  FSL_CHECK_EQ(repeated->duplicates, std::uint64_t{4});
  FSL_CHECK(repeated->nothing_written);
}

FSL_TEST(a_batch_may_not_contain_the_same_identity_twice) {
  fsl_test::TempDirectory temp("batch_internal_duplicate");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  std::vector<fsl::SubmittedObservation> batch;
  batch.push_back(observation_for(seed->asset, 7, "obs-a"));
  batch.push_back(observation_for(seed->asset, 7, "obs-b"));
  FSL_REQUIRE_ERROR(ledger.append_batch(batch), fsl::ErrorCode::kDuplicateEventId);

  const std::uint64_t before = ledger.watermark().value().sequence->value();
  FSL_CHECK_EQ(before, ledger.watermark().value().sequence->value());
}

FSL_TEST(a_batch_larger_than_a_segment_rotates_first) {
  fsl_test::TempDirectory temp("batch_rotation");
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.max_segment_bytes = 4096;
  options.max_batch_events = 256;
  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  std::uint64_t committed = 0;
  for (std::uint64_t round = 0; round < 6; ++round) {
    std::vector<fsl::SubmittedObservation> batch;
    for (std::uint64_t i = 0; i < 32; ++i) {
      batch.push_back(observation_for(seed->asset, round * 100 + i, "obs"));
    }
    auto outcome = ledger.append_batch(batch);
    FSL_REQUIRE_OK(outcome);
    FSL_CHECK_EQ(outcome->events.size(), std::size_t{32});
    committed += 32;
  }
  FSL_CHECK_EQ(ledger.watermark().value().event_count, committed + 5);

  auto segments = ledger.segments();
  FSL_REQUIRE_OK(segments);
  FSL_CHECK(segments->size() > 1);

  // Sequences remain contiguous across every segment.
  const std::uint64_t upper = ledger.watermark().value().sequence->value();
  for (std::uint64_t sequence = 1; sequence <= upper; ++sequence) {
    auto event = ledger.read(fsl::LedgerSequence(sequence));
    FSL_REQUIRE_OK(event);
    FSL_CHECK_EQ(event->sequence().value(), sequence);
  }
  const fsl::VerifyReport report = ledger.verify();
  FSL_CHECK(report.ok());
  FSL_CHECK_EQ(report.records_verified, upper);
}

FSL_TEST(a_batch_is_durable_at_a_single_acknowledgement_boundary) {
  fsl_test::TempDirectory temp("batch_durable");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  const fsl::LedgerStats before = ledger.stats();
  std::vector<fsl::SubmittedObservation> batch;
  for (std::uint64_t i = 0; i < 32; ++i) {
    batch.push_back(observation_for(seed->asset, i, "obs"));
  }
  const std::uint64_t commits_before = before.durable_commits;
  const std::uint64_t flushes_before = before.flush_calls;
  auto outcome = ledger.append_batch(batch);
  FSL_REQUIRE_OK(outcome);

  const fsl::LedgerStats after = ledger.stats();
  // One commit, and the flush count grows by a small constant rather than per
  // event: the batch shares a single publication.
  FSL_CHECK_EQ(after.durable_commits, commits_before + 1);
  FSL_CHECK(after.flush_calls < flushes_before + 32);
  FSL_CHECK_EQ(after.committed_frames, before.committed_frames + 32);
}

}  // namespace

FSL_TEST_MAIN("test_ledger_batches")

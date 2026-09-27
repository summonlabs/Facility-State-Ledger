// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <vector>

#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_ledger_idempotency.cpp
/// Duplicate semantics keyed by event identity and by retry token.

namespace {

using fsl_test::subject_ref;

FSL_TEST(a_repeated_submission_is_answered_idempotently) {
  fsl_test::TempDirectory temp("idem_repeat");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);
  const std::uint64_t before = ledger.watermark().value().sequence->value();

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(0x1111ULL, 1)
      .subject(seed->asset)
      .token(0x2222ULL, 1)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-1", {1, 2, 3}}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);

  auto first = ledger.append(observation.value());
  FSL_REQUIRE_OK(first);
  FSL_CHECK(!first->duplicate);
  const fsl::LedgerSequence committed = first->event.sequence();
  FSL_CHECK_EQ(committed.value(), before + 1);

  auto second = ledger.append(observation.value());
  FSL_REQUIRE_OK(second);
  FSL_CHECK(second->duplicate);
  FSL_CHECK(second->event.sequence() == committed);
  FSL_CHECK(second->event.event_id() == first->event.event_id());
  FSL_CHECK(second->event.integrity() == first->event.integrity());
  FSL_CHECK_EQ(ledger.watermark().value().sequence->value(), committed.value());
  FSL_CHECK_EQ(ledger.stats().duplicate_appends, std::uint64_t{1});
}

FSL_TEST(reusing_an_event_identity_with_different_content_is_a_conflict) {
  fsl_test::TempDirectory temp("idem_conflict");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(0x3333ULL, 1)
      .subject(seed->asset)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-1", {1}}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_OK(ledger.append(observation.value()));

  fsl_test::SubmissionBuilder different(fsl::EventKind::kObservationAccepted);
  different.event_id(0x3333ULL, 1)
      .subject(seed->asset)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-2", {2}}));
  auto conflicting = different.build();
  FSL_REQUIRE_OK(conflicting);
  FSL_REQUIRE_ERROR(ledger.append(conflicting.value()), fsl::ErrorCode::kDuplicateEventId);
}

FSL_TEST(reusing_a_retry_token_with_different_content_is_a_conflict) {
  fsl_test::TempDirectory temp("idem_token_conflict");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(0x4444ULL, 1)
      .subject(seed->asset)
      .token(0x5555ULL, 1)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-1", {1}}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_OK(ledger.append(observation.value()));

  // A different event identity but the same token with different content.
  fsl_test::SubmissionBuilder different(fsl::EventKind::kObservationAccepted);
  different.event_id(0x4444ULL, 2)
      .subject(seed->asset)
      .token(0x5555ULL, 1)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-2", {2}}));
  auto conflicting = different.build();
  FSL_REQUIRE_OK(conflicting);
  FSL_REQUIRE_ERROR(ledger.append(conflicting.value()), fsl::ErrorCode::kDuplicateIdempotencyToken);

  // A new token with the same content as an earlier event is a distinct event.
  fsl_test::SubmissionBuilder fresh(fsl::EventKind::kObservationAccepted);
  fresh.event_id(0x4444ULL, 3)
      .subject(seed->asset)
      .token(0x5555ULL, 2)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-2", {2}}));
  auto distinct = fresh.build();
  FSL_REQUIRE_OK(distinct);
  auto outcome = ledger.append(distinct.value());
  FSL_REQUIRE_OK(outcome);
  FSL_CHECK(!outcome->duplicate);
}

FSL_TEST(a_duplicate_is_recognised_when_the_envelope_carries_a_fresh_token) {
  fsl_test::TempDirectory temp("idem_fresh_token");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(0x6666ULL, 1)
      .subject(seed->asset)
      .token(0x7777ULL, 1)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs", {9}}));
  auto first = builder.build();
  FSL_REQUIRE_OK(first);
  FSL_REQUIRE_OK(ledger.append(first.value()));

  fsl_test::SubmissionBuilder retry(fsl::EventKind::kObservationAccepted);
  retry.event_id(0x6666ULL, 1)
      .subject(seed->asset)
      .token(0x7777ULL, 2)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs", {9}}));
  auto retried = retry.build();
  FSL_REQUIRE_OK(retried);
  auto outcome = ledger.append(retried.value());
  FSL_REQUIRE_OK(outcome);
  FSL_CHECK(outcome->duplicate);
}

FSL_TEST(duplicate_detection_survives_close_and_reopen) {
  fsl_test::TempDirectory temp("idem_restart");
  fsl::EventId identity;
  fsl::IdempotencyToken token;
  {
    auto seeded = fsl_test::open_journal(temp.path());
    FSL_REQUIRE_OK(seeded);
    fsl::Ledger ledger = std::move(seeded).value();
    auto seed = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed);

    fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
    builder.event_id(0x8888ULL, 1)
        .subject(seed->asset)
        .token(0x9999ULL, 1)
        .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs", {1}}));
    auto observation = builder.build();
    FSL_REQUIRE_OK(observation);
    FSL_REQUIRE_OK(ledger.append(observation.value()));
    identity = observation->event_id();
    token = *observation->idempotency_token();
    FSL_REQUIRE_OK(ledger.close());
  }

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  fsl::Ledger ledger = std::move(reopened).value();

  auto by_event = ledger.find_event(identity);
  FSL_REQUIRE_OK(by_event);
  auto by_token = ledger.find_idempotency(token);
  FSL_REQUIRE_OK(by_token);
  FSL_CHECK(by_token->sequence() == by_event->sequence());

  // Re-appending the identical submission after the restart is still a
  // duplicate: derived state was reconstructed from the log.
  const std::uint64_t before = ledger.watermark().value().sequence->value();
  fsl_test::SubmissionBuilder again(fsl::EventKind::kObservationAccepted);
  again.event_id(0x8888ULL, 1)
      .subject(fsl::SubjectKind::kAsset, "site-a.hall-1.row-1.rack-1.asset-1")
      .token(0x9999ULL, 1)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs", {1}}));
  auto repeated = again.build();
  FSL_REQUIRE_OK(repeated);
  auto outcome = ledger.append(repeated.value());
  FSL_REQUIRE_OK(outcome);
  FSL_CHECK(outcome->duplicate);
  FSL_CHECK_EQ(ledger.watermark().value().sequence->value(), before);
}

FSL_TEST(a_duplicate_is_recognised_even_when_the_epoch_has_moved_on) {
  fsl_test::TempDirectory temp("idem_stale_retry");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(0xAAAAULL, 1)
      .epoch(1)
      .subject(seed->asset)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs", {1}}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_OK(ledger.append(observation.value()));

  FSL_REQUIRE_OK(fsl_test::close_epoch(ledger, 1, 900));
  FSL_REQUIRE_OK(fsl_test::open_epoch(ledger, 2, 1, 901));

  // The retry still names epoch 1, which is closed. Admission policy would
  // reject it; duplicate resolution runs first and answers idempotently.
  auto outcome = ledger.append(observation.value());
  FSL_REQUIRE_OK(outcome);
  FSL_CHECK(outcome->duplicate);

  // The same envelope with different content is judged by policy, not treated
  // as a duplicate, and is rejected as stale rather than conflicting.
  fsl_test::SubmissionBuilder altered(fsl::EventKind::kObservationAccepted);
  altered.event_id(0xAAAAULL, 2)
      .epoch(1)
      .subject(seed->asset)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"other", {2}}));
  auto altered_observation = altered.build();
  FSL_REQUIRE_OK(altered_observation);
  FSL_REQUIRE_ERROR(ledger.append(altered_observation.value()), fsl::ErrorCode::kEpochClosed);
}

FSL_TEST(duplicate_detection_survives_an_index_rebuild) {
  fsl_test::TempDirectory temp("idem_rebuild");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(0xBBBBULL, 1)
      .subject(seed->asset)
      .token(0xCCCCULL, 1)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs", {1}}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_OK(ledger.append(observation.value()));

  FSL_REQUIRE_OK(ledger.rebuild_index());
  FSL_CHECK(ledger.open_report().index_rebuilt);

  auto outcome = ledger.append(observation.value());
  FSL_REQUIRE_OK(outcome);
  FSL_CHECK(outcome->duplicate);
  FSL_REQUIRE_OK(ledger.find_event(observation->event_id()));
  FSL_REQUIRE_OK(ledger.find_idempotency(*observation->idempotency_token()));
}

FSL_TEST(a_rebuilt_index_still_answers_queries_correctly) {
  fsl_test::TempDirectory temp("idem_rebuild_query");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);
  for (std::uint64_t i = 0; i < 12; ++i) {
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs-" + std::to_string(i), 1000 + i, 8));
  }

  fsl::Query request;
  request.subject = seed->asset;
  request.limit = 100;
  auto before = ledger.query(request);
  FSL_REQUIRE_OK(before);
  // Twelve observations plus the subject's own registration record.
  FSL_CHECK_EQ(before->events.size(), std::size_t{13});

  FSL_REQUIRE_OK(ledger.rebuild_index());

  auto after = ledger.query(request);
  FSL_REQUIRE_OK(after);
  FSL_CHECK_EQ(after->events.size(), before->events.size());
  for (std::size_t i = 0; i < after->events.size(); ++i) {
    FSL_CHECK(after->events[i].sequence() == before->events[i].sequence());
    FSL_CHECK(after->events[i].integrity() == before->events[i].integrity());
  }
}

FSL_TEST(the_dedupe_bound_rejects_rather_than_forgetting) {
  fsl_test::TempDirectory temp("idem_bound");
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.max_dedupe_entries = 8;
  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  // The opening sequence already consumed several identities; keep appending
  // until the bound is reached and confirm the refusal is explicit.
  bool refused = false;
  for (std::uint64_t i = 0; i < 32 && !refused; ++i) {
    const auto outcome =
        fsl_test::record_observation(ledger, seed->asset, "obs", 2000 + i, 4);
    if (!outcome.has_value()) {
      FSL_CHECK_EQ(outcome.status().code(), fsl::ErrorCode::kCapacityLimitExceeded);
      FSL_CHECK_EQ(outcome.status().category(), fsl::ErrorCategory::kCapacityExceeded);
      refused = true;
    }
  }
  FSL_CHECK(refused);

  // The ledger remains internally consistent after a refusal.
  FSL_CHECK(ledger.verify().ok());
  const std::uint64_t committed = ledger.watermark().value().sequence->value();
  for (std::uint64_t sequence = 1; sequence <= committed; ++sequence) {
    FSL_REQUIRE_OK(ledger.read(fsl::LedgerSequence(sequence)));
  }
}

}  // namespace

FSL_TEST_MAIN("test_ledger_idempotency")

// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <set>
#include <vector>

#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_ledger_queries.cpp
/// Bounded historical queries: limits, ranges, predicates and cursors.

namespace {

using fsl_test::subject_ref;

struct Seeded {
  Seeded(fsl::Ledger ledger, fsl_test::FacilitySeed seed)
      : ledger(std::move(ledger)), seed(std::move(seed)) {}

  fsl::Ledger ledger;
  fsl_test::FacilitySeed seed;
};

[[nodiscard]] fsl::Result<Seeded> make_seeded(fsl_test::TempDirectory& temp,
                                              const fsl::LedgerOptions& options) {
  auto created = fsl::Ledger::create(temp.path(), options);
  if (!created.has_value()) {
    return created.status();
  }
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  if (!seed.has_value()) {
    return seed.status();
  }
  fsl_test::FacilitySeed facility = std::move(seed).value();
  for (std::uint64_t i = 0; i < 20; ++i) {
    auto appended =
        fsl_test::record_observation(ledger, facility.asset, "obs-" + std::to_string(i), 100 + i, 8);
    if (!appended.has_value()) {
      return appended.status();
    }
  }
  return Seeded(std::move(ledger), std::move(facility));
}

FSL_TEST(a_query_requires_a_limit_within_the_configured_bound) {
  fsl_test::TempDirectory temp("query_limit");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  fsl::Query no_limit;
  FSL_REQUIRE_ERROR(made->ledger.query(no_limit), fsl::ErrorCode::kLimitRequired);

  fsl::Query too_large;
  too_large.limit = 1'000'000;
  FSL_REQUIRE_ERROR(made->ledger.query(too_large), fsl::ErrorCode::kLimitOutOfRange);
}

FSL_TEST(a_query_rejects_an_inverted_range) {
  fsl_test::TempDirectory temp("query_range");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);
  fsl::Query query;
  query.from = fsl::LedgerSequence(10);
  query.to = fsl::LedgerSequence(4);
  query.limit = 8;
  FSL_REQUIRE_ERROR(made->ledger.query(query), fsl::ErrorCode::kRangeInvalid);
}

FSL_TEST(a_sequence_range_query_returns_ascending_events) {
  fsl_test::TempDirectory temp("query_range_ok");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  fsl::Query query;
  query.from = fsl::LedgerSequence(3);
  query.to = fsl::LedgerSequence(9);
  query.limit = 100;
  auto page = made->ledger.query(query);
  FSL_REQUIRE_OK(page);
  FSL_CHECK_EQ(page->events.size(), std::size_t{7});
  for (std::size_t i = 0; i < page->events.size(); ++i) {
    FSL_CHECK_EQ(page->events[i].sequence().value(), 3 + i);
    if (i > 0) {
      FSL_CHECK(page->events[i - 1].sequence() < page->events[i].sequence());
    }
  }
  FSL_CHECK(!page->next_cursor.has_value());
}

FSL_TEST(a_subject_query_returns_only_that_subject_and_paginates) {
  fsl_test::TempDirectory temp("query_subject");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  std::vector<std::uint64_t> collected;
  fsl::Query query;
  query.subject = made->seed.asset;
  query.limit = 7;
  while (true) {
    auto page = made->ledger.query(query);
    FSL_REQUIRE_OK(page);
    if (page->source != fsl::QuerySource::kIndexed) {
      FSL_CHECK(page->source == fsl::QuerySource::kIndexed);
    }
    for (const fsl::EventEnvelope& event : page->events) {
      FSL_CHECK(event.subject() == made->seed.asset);
      collected.push_back(event.sequence().value());
    }
    if (!page->next_cursor.has_value()) {
      break;
    }
    query.after = page->next_cursor;
  }
  // Twenty observations plus the asset's registration record.
  FSL_CHECK_EQ(collected.size(), std::size_t{21});
  std::set<std::uint64_t> unique(collected.begin(), collected.end());
  FSL_CHECK_EQ(unique.size(), collected.size());
}

FSL_TEST(a_kind_query_returns_only_that_kind) {
  fsl_test::TempDirectory temp("query_kind");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  fsl::Query query;
  query.kind = fsl::EventKind::kObservationAccepted;
  query.limit = 100;
  auto page = made->ledger.query(query);
  FSL_REQUIRE_OK(page);
  FSL_CHECK_EQ(page->events.size(), std::size_t{20});
  for (const fsl::EventEnvelope& event : page->events) {
    FSL_CHECK(event.kind() == fsl::EventKind::kObservationAccepted);
  }
}

FSL_TEST(a_source_query_returns_only_that_source) {
  fsl_test::TempDirectory temp("query_source");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  fsl::Query query;
  query.source = fsl::SourceComponentId::parse("test.harness").value();
  query.limit = 100;
  auto page = made->ledger.query(query);
  FSL_REQUIRE_OK(page);
  FSL_CHECK(page->events.size() >= 24);
  for (const fsl::EventEnvelope& event : page->events) {
    FSL_CHECK(event.provenance().source().str() == "test.harness");
  }
}

FSL_TEST(an_event_identity_query_returns_at_most_the_named_event) {
  fsl_test::TempDirectory temp("query_identity");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  auto known = made->ledger.read(fsl::LedgerSequence(5));
  FSL_REQUIRE_OK(known);

  fsl::Query query;
  query.event_id = known->event_id();
  query.limit = 10;
  auto page = made->ledger.query(query);
  FSL_REQUIRE_OK(page);
  FSL_CHECK_EQ(page->events.size(), std::size_t{1});
  FSL_CHECK(page->events.front().event_id() == known->event_id());

  fsl::Query unknown;
  unknown.event_id = fsl::EventId::from_words(0xDEADULL, 0xBEEFULL);
  unknown.limit = 10;
  auto empty = made->ledger.query(unknown);
  FSL_REQUIRE_OK(empty);
  FSL_CHECK(empty->events.empty());
  FSL_CHECK(empty->exhausted);
}

FSL_TEST(a_generation_and_epoch_query_matches_the_envelope) {
  fsl_test::TempDirectory temp("query_generation");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  fsl::Query by_generation;
  by_generation.facility_generation = fsl::FacilityGeneration(1);
  by_generation.limit = 100;
  auto generation_page = made->ledger.query(by_generation);
  FSL_REQUIRE_OK(generation_page);
  FSL_CHECK(generation_page->events.size() >= 24);

  fsl::Query by_epoch;
  by_epoch.epoch = fsl::FacilityEpoch(1);
  by_epoch.limit = 100;
  auto epoch_page = made->ledger.query(by_epoch);
  FSL_REQUIRE_OK(epoch_page);
  FSL_CHECK_EQ(epoch_page->events.size(), generation_page->events.size() - 1);
  for (const fsl::EventEnvelope& event : epoch_page->events) {
    FSL_REQUIRE(event.epoch().has_value());
    FSL_CHECK_EQ(event.epoch()->value(), std::uint64_t{1});
  }

  fsl::Query unknown_epoch;
  unknown_epoch.epoch = fsl::FacilityEpoch(42);
  unknown_epoch.limit = 100;
  auto none = made->ledger.query(unknown_epoch);
  FSL_REQUIRE_OK(none);
  FSL_CHECK(none->events.empty());
}

FSL_TEST(an_unindexed_query_scans_a_bounded_window_and_refuses_a_wider_one) {
  fsl_test::TempDirectory temp("query_scan");
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.max_scan_window = 10;
  auto made = make_seeded(temp, options);
  FSL_REQUIRE_OK(made);

  // A range query has no posting source, so it walks the window.
  fsl::Query small;
  small.from = fsl::LedgerSequence(1);
  small.to = fsl::LedgerSequence(8);
  small.limit = 100;
  auto scanned = made->ledger.query(small);
  FSL_REQUIRE_OK(scanned);
  FSL_CHECK(scanned->source == fsl::QuerySource::kLinearScan);
  FSL_CHECK_EQ(scanned->events.size(), std::size_t{8});
  FSL_CHECK_EQ(scanned->records_examined, std::uint64_t{8});

  fsl::Query wide;
  wide.from = fsl::LedgerSequence(1);
  wide.to = fsl::LedgerSequence(1000);
  wide.limit = 100;
  FSL_REQUIRE_ERROR(made->ledger.query(wide), fsl::ErrorCode::kCapacityLimitExceeded);
}

FSL_TEST(a_limit_bounds_the_page_and_reports_that_it_is_not_exhausted) {
  fsl_test::TempDirectory temp("query_page");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  fsl::Query query;
  query.kind = fsl::EventKind::kObservationAccepted;
  query.limit = 5;
  auto page = made->ledger.query(query);
  FSL_REQUIRE_OK(page);
  FSL_CHECK_EQ(page->events.size(), std::size_t{5});
  FSL_CHECK(!page->exhausted);
  FSL_REQUIRE(page->next_cursor.has_value());
  FSL_CHECK(page->next_cursor->value() >= page->events.back().sequence().value());
}

FSL_TEST(point_lookups_find_committed_records_and_nothing_else) {
  fsl_test::TempDirectory temp("query_point");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  auto event = made->ledger.read(fsl::LedgerSequence(2));
  FSL_REQUIRE_OK(event);
  auto by_id = made->ledger.find_event(event->event_id());
  FSL_REQUIRE_OK(by_id);
  FSL_CHECK(by_id->sequence() == event->sequence());
  FSL_CHECK(by_id->integrity() == event->integrity());

  FSL_REQUIRE_ERROR(made->ledger.find_event(fsl::EventId::from_words(1, 1)), fsl::ErrorCode::kNotFound);
  FSL_REQUIRE_ERROR(made->ledger.find_idempotency(fsl::IdempotencyToken::from_words(1, 1)),
                    fsl::ErrorCode::kNotFound);
}

FSL_TEST(a_query_never_returns_a_record_the_log_does_not_hold) {
  fsl_test::TempDirectory temp("query_authority");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  // The subject index also holds the registration records; every returned event
  // must be byte-identical to what a sequential replay delivers.
  fsl::Query query;
  query.subject = made->seed.asset;
  query.limit = 100;
  auto page = made->ledger.query(query);
  FSL_REQUIRE_OK(page);
  FSL_REQUIRE(!page->events.empty());

  std::vector<fsl::EventEnvelope> replayed;
  const auto status =
      made->ledger.replay_each(fsl::ReplayRequest{},
                               [&replayed](const fsl::EventEnvelope& event) {
                                 replayed.push_back(event);
                                 return true;
                               });
  FSL_CHECK(status.is_ok());
  FSL_CHECK_EQ(replayed.size(), static_cast<std::size_t>(made->ledger.watermark().value().sequence->value()));
  for (const fsl::EventEnvelope& queried : page->events) {
    const std::size_t index = static_cast<std::size_t>(queried.sequence().value() - 1);
    FSL_REQUIRE(index < replayed.size());
    FSL_CHECK(replayed[index].integrity() == queried.integrity());
    FSL_CHECK(replayed[index].event_id() == queried.event_id());
  }
}

FSL_TEST(subject_and_source_listings_are_ordered_and_bounded) {
  fsl_test::TempDirectory temp("query_listings");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  auto subjects = made->ledger.subjects(100);
  FSL_REQUIRE_OK(subjects);
  FSL_CHECK(subjects->size() >= 4);
  for (std::size_t i = 1; i < subjects->size(); ++i) {
    FSL_CHECK(subjects->at(i - 1).subject < subjects->at(i).subject);
  }

  auto first_page = made->ledger.subjects(2);
  FSL_REQUIRE_OK(first_page);
  FSL_CHECK_EQ(first_page->size(), std::size_t{2});
  auto second_page = made->ledger.subjects(2, first_page->back().subject);
  FSL_REQUIRE_OK(second_page);
  if (!second_page->empty()) {
    FSL_CHECK(first_page->back().subject < second_page->front().subject);
  }

  auto sources = made->ledger.sources(100);
  FSL_REQUIRE_OK(sources);
  FSL_CHECK(!sources->empty());
  for (std::size_t i = 1; i < sources->size(); ++i) {
    FSL_CHECK(sources->at(i - 1).source < sources->at(i).source);
  }

  FSL_REQUIRE_ERROR(made->ledger.subjects(0), fsl::ErrorCode::kLimitRequired);
  FSL_REQUIRE_ERROR(made->ledger.sources(0), fsl::ErrorCode::kLimitRequired);
}

FSL_TEST(a_subject_view_reports_lifecycle_and_an_unknown_subject_is_empty) {
  fsl_test::TempDirectory temp("query_subject_view");
  auto made = make_seeded(temp, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(made);

  auto live = made->ledger.subject(made->seed.asset);
  FSL_REQUIRE_OK(live);
  FSL_CHECK(live->registered);
  FSL_CHECK(!live->retired);
  FSL_CHECK_EQ(live->event_count, std::uint64_t{21});
  FSL_CHECK(live->first_sequence.has_value());
  FSL_CHECK(live->last_sequence.has_value());
  FSL_CHECK(live->first_sequence < live->last_sequence);

  auto unknown = made->ledger.subject(subject_ref(fsl::SubjectKind::kRack, "no-such-rack"));
  FSL_REQUIRE_OK(unknown);
  FSL_CHECK(!unknown->registered);
  FSL_CHECK(!unknown->first_sequence.has_value());
}

}  // namespace

FSL_TEST_MAIN("test_ledger_queries")

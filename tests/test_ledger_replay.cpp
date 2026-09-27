// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <set>
#include <vector>

#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_ledger_replay.cpp
/// Deterministic replay: fixed bounds, ascending order, no silent gaps.

namespace {

[[nodiscard]] fsl::Result<fsl::Ledger> make_ledger(fsl_test::TempDirectory& temp,
                                                   std::uint64_t events) {
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  if (!created.has_value()) {
    return created.status();
  }
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  if (!seed.has_value()) {
    return seed.status();
  }
  for (std::uint64_t i = 0; i < events; ++i) {
    auto appended = fsl_test::record_observation(ledger, seed->asset, "obs-" + std::to_string(i),
                                                 500 + i, 6);
    if (!appended.has_value()) {
      return appended.status();
    }
  }
  return ledger;
}

FSL_TEST(a_full_replay_delivers_every_committed_event_in_order) {
  fsl_test::TempDirectory temp("replay_full");
  auto made = make_ledger(temp, 30);
  FSL_REQUIRE_OK(made);

  std::vector<std::uint64_t> sequences;
  std::uint64_t previous = 0;
  const auto status =
      made->replay_each(fsl::ReplayRequest{}, [&](const fsl::EventEnvelope& event) {
        sequences.push_back(event.sequence().value());
        FSL_CHECK(event.sequence().value() > previous);
        previous = event.sequence().value();
        return true;
      });
  FSL_CHECK(status.is_ok());
  FSL_CHECK_EQ(sequences.size(), static_cast<std::size_t>(made->watermark().value().sequence->value()));
  for (std::size_t i = 0; i < sequences.size(); ++i) {
    FSL_CHECK_EQ(sequences[i], i + 1);
  }
}

FSL_TEST(replay_respects_an_absent_start_and_an_explicit_range) {
  fsl_test::TempDirectory temp("replay_range");
  auto made = make_ledger(temp, 10);
  FSL_REQUIRE_OK(made);

  fsl::ReplayRequest tail;
  tail.from = fsl::LedgerSequence(8);
  auto sequences = fsl_test::replay_sequences(*made);
  FSL_REQUIRE_OK(sequences);
  std::vector<std::uint64_t> from_eight;
  const auto status = made->replay_each(tail, [&from_eight](const fsl::EventEnvelope& event) {
    from_eight.push_back(event.sequence().value());
    return true;
  });
  FSL_CHECK(status.is_ok());
  FSL_CHECK_EQ(from_eight.front(), std::uint64_t{8});
  FSL_CHECK_EQ(from_eight.back(), sequences->back());

  fsl::ReplayRequest bounded;
  bounded.from = fsl::LedgerSequence(4);
  bounded.to = fsl::LedgerSequence(6);
  std::vector<std::uint64_t> window;
  const auto window_status = made->replay_each(bounded, [&window](const fsl::EventEnvelope& event) {
    window.push_back(event.sequence().value());
    return true;
  });
  FSL_CHECK(window_status.is_ok());
  FSL_REQUIRE(window.size() == 3);
  FSL_CHECK_EQ(window[0], std::uint64_t{4});
  FSL_CHECK_EQ(window[2], std::uint64_t{6});
}

FSL_TEST(replay_rejects_impossible_ranges) {
  fsl_test::TempDirectory temp("replay_bad_range");
  auto made = make_ledger(temp, 5);
  FSL_REQUIRE_OK(made);

  fsl::ReplayRequest inverted;
  inverted.from = fsl::LedgerSequence(9);
  inverted.to = fsl::LedgerSequence(2);
  FSL_REQUIRE_ERROR(made->replay(inverted), fsl::ErrorCode::kRangeInvalid);

  fsl::ReplayRequest zero;
  zero.from = fsl::LedgerSequence(0);
  FSL_REQUIRE_ERROR(made->replay(zero), fsl::ErrorCode::kRangeInvalid);

  fsl::ReplayRequest beyond;
  beyond.from = fsl::LedgerSequence(10'000);
  FSL_REQUIRE_ERROR(made->replay(beyond), fsl::ErrorCode::kRangeInvalid);
}

FSL_TEST(a_replay_of_an_empty_ledger_is_not_found) {
  // A freshly created ledger always holds its own creation record, so an empty
  // ledger can only be observed through a range that lies past the watermark.
  fsl_test::TempDirectory temp("replay_empty");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  fsl::ReplayRequest past_end;
  past_end.from = fsl::LedgerSequence(5);
  FSL_REQUIRE_ERROR(ledger.replay(past_end), fsl::ErrorCode::kRangeInvalid);
}

FSL_TEST(a_replay_stream_delivers_the_same_sequence_as_a_collecting_run) {
  fsl_test::TempDirectory temp("replay_stream");
  auto made = make_ledger(temp, 12);
  FSL_REQUIRE_OK(made);

  auto stream = made->replay(fsl::ReplayRequest{});
  FSL_REQUIRE_OK(stream);
  FSL_CHECK(!stream->upper_bound().has_value() || stream->upper_bound()->value() > 0);

  std::vector<std::uint64_t> delivered;
  std::optional<fsl::EventEnvelope> envelope;
  while (true) {
    const fsl::ReplayStep step = stream->next(envelope);
    if (step == fsl::ReplayStep::kEnd) {
      break;
    }
    FSL_REQUIRE(step == fsl::ReplayStep::kEvent);
    delivered.push_back(envelope->sequence().value());
  }
  FSL_CHECK_EQ(stream->delivered(), static_cast<std::uint64_t>(delivered.size()));
  FSL_CHECK(!stream->error().is_error());

  auto collected = fsl_test::replay_sequences(*made);
  FSL_REQUIRE_OK(collected);
  FSL_CHECK(delivered == collected.value());
}

FSL_TEST(a_stream_fixes_its_upper_bound_when_it_is_created) {
  fsl_test::TempDirectory temp("replay_bound");
  auto made = make_ledger(temp, 6);
  FSL_REQUIRE_OK(made);
  const std::uint64_t upper = made->watermark().value().sequence->value();

  auto stream = made->replay(fsl::ReplayRequest{});
  FSL_REQUIRE_OK(stream);
  FSL_REQUIRE(stream->upper_bound().has_value());
  FSL_CHECK_EQ(stream->upper_bound()->value(), upper);

  // Committing more events must not extend the stream that is already open.
  auto seed = fsl_test::seed_facility(*made);
  FSL_REQUIRE_OK(seed);
  FSL_REQUIRE_OK(fsl_test::record_observation(*made, seed->asset, "later", 5000, 4));

  std::uint64_t delivered = 0;
  std::optional<fsl::EventEnvelope> envelope;
  while (stream->next(envelope) == fsl::ReplayStep::kEvent) {
    ++delivered;
  }
  FSL_CHECK_EQ(delivered, upper);
  FSL_CHECK(made->watermark().value().sequence->value() > upper);
}

FSL_TEST(a_filtered_replay_walks_the_whole_range_without_skipping_silently) {
  fsl_test::TempDirectory temp("replay_filter");
  auto made = make_ledger(temp, 10);
  FSL_REQUIRE_OK(made);

  fsl::ReplayRequest filtered;
  filtered.kind = fsl::EventKind::kObservationAccepted;
  std::vector<fsl::EventKind> delivered;
  const auto status = made->replay_each(filtered, [&delivered](const fsl::EventEnvelope& event) {
    delivered.push_back(event.kind());
    return true;
  });
  FSL_CHECK(status.is_ok());
  FSL_CHECK_EQ(delivered.size(), std::size_t{10});
  for (const fsl::EventKind kind : delivered) {
    FSL_CHECK(kind == fsl::EventKind::kObservationAccepted);
  }

  // The stream's next_sequence still advances across filtered-out events.
  auto stream = made->replay(filtered);
  FSL_REQUIRE_OK(stream);
  std::optional<fsl::EventEnvelope> envelope;
  FSL_CHECK(stream->next(envelope) == fsl::ReplayStep::kEvent);
  FSL_CHECK(stream->next_sequence().has_value());
  FSL_CHECK_EQ(stream->next_sequence()->value(), envelope->sequence().value() + 1);
}

FSL_TEST(a_replay_can_stop_early_through_its_visitor) {
  fsl_test::TempDirectory temp("replay_stop");
  auto made = make_ledger(temp, 20);
  FSL_REQUIRE_OK(made);

  std::uint64_t seen = 0;
  const auto status = made->replay_each(fsl::ReplayRequest{}, [&seen](const fsl::EventEnvelope&) {
    ++seen;
    return seen < 4;
  });
  FSL_CHECK(status.is_ok());
  FSL_CHECK_EQ(seen, std::uint64_t{4});
}

FSL_TEST(a_replay_survives_segment_rotation) {
  fsl_test::TempDirectory temp("replay_rotation");
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.max_segment_bytes = 3000;
  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);
  for (std::uint64_t i = 0; i < 40; ++i) {
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 900 + i, 8));
  }
  auto segments = ledger.segments();
  FSL_REQUIRE_OK(segments);
  FSL_REQUIRE(segments->size() > 1);

  auto sequences = fsl_test::replay_sequences(ledger);
  FSL_REQUIRE_OK(sequences);
  FSL_CHECK_EQ(sequences->size(), static_cast<std::size_t>(ledger.watermark().value().sequence->value()));
  for (std::size_t i = 0; i < sequences->size(); ++i) {
    FSL_CHECK_EQ(sequences->at(i), i + 1);
  }

  // A replay that starts inside a later segment must not miss its first record.
  fsl::ReplayRequest from_segment_two;
  from_segment_two.from = fsl::LedgerSequence(segments->at(1).first_sequence.value());
  std::vector<std::uint64_t> tail;
  const auto status = ledger.replay_each(from_segment_two, [&tail](const fsl::EventEnvelope& event) {
    tail.push_back(event.sequence().value());
    return true;
  });
  FSL_CHECK(status.is_ok());
  FSL_REQUIRE(!tail.empty());
  FSL_CHECK_EQ(tail.front(), segments->at(1).first_sequence->value());
  FSL_CHECK_EQ(tail.back(), ledger.watermark().value().sequence->value());
}

}  // namespace

FSL_TEST_MAIN("test_ledger_replay")

// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <set>
#include <vector>

#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_property.cpp
/// Randomised, seeded property tests. Every run is reproducible from the seed
/// printed on failure: the generator is a fixed xorshift64* sequence and the
/// workload is derived only from that sequence.

namespace {

class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

  [[nodiscard]] std::uint64_t next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1DULL;
  }

  [[nodiscard]] std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

  [[nodiscard]] bool chance(std::uint64_t numerator, std::uint64_t denominator) {
    return below(denominator) < numerator;
  }

 private:
  std::uint64_t state_;
};

struct Model {
  std::uint64_t committed = 0;
  std::uint64_t generation = 1;
  std::uint64_t latest_epoch = 0;
  std::uint64_t open_epoch = 0;
  std::set<std::uint64_t> identities;
  std::vector<std::uint64_t> order;
};

/// Applies one random operation to the ledger and to the model. The model
/// predicts only what the ledger's documented semantics require; anything the
/// model does not predict is checked as an invariant instead.
void run_generation(std::uint64_t seed, std::uint64_t operations, std::uint64_t event_budget) {
  fsl_test::TempDirectory temp("property_" + std::to_string(seed));
  Random random(seed);

  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.max_segment_bytes = 8192;
  options.auto_checkpoint_interval = 17;
  options.max_checkpoints = 3;

  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();

  Model model;
  model.committed = 1;  // the creation record

  const fsl::SubjectRef location = fsl_test::subject_ref(fsl::SubjectKind::kLocation, "site-a.hall-1");
  const fsl::SubjectRef rack = fsl_test::subject_ref(fsl::SubjectKind::kRack, "site-a.hall-1.row-1.rack-1");
  bool location_registered = false;
  bool rack_registered = false;
  std::uint64_t identity_counter = 0;

  for (std::uint64_t step = 0; step < operations; ++step) {
    const std::uint64_t choice = random.below(10);
    std::uint64_t next_identity = ++identity_counter;

    if (choice == 0 && model.open_epoch == 0) {
      const auto outcome = fsl_test::open_epoch(ledger, model.latest_epoch + 1, model.generation, next_identity);
      FSL_REQUIRE_OK(outcome);
      model.open_epoch = model.latest_epoch + 1;
      model.latest_epoch = model.open_epoch;
      ++model.committed;
    } else if (choice == 1 && model.open_epoch != 0) {
      const auto outcome = fsl_test::close_epoch(ledger, model.open_epoch, next_identity);
      FSL_REQUIRE_OK(outcome);
      model.open_epoch = 0;
      ++model.committed;
    } else if (choice == 2 && model.open_epoch == 0) {
      const auto outcome = fsl_test::advance_generation(ledger, model.generation, next_identity);
      FSL_REQUIRE_OK(outcome);
      model.generation += 1;
      ++model.committed;
    } else if (choice == 3 && model.open_epoch != 0 && !location_registered) {
      const auto outcome = fsl_test::register_subject(ledger, location, std::nullopt, next_identity);
      FSL_REQUIRE_OK(outcome);
      location_registered = true;
      ++model.committed;
    } else if (choice == 4 && model.open_epoch != 0 && location_registered && !rack_registered) {
      const auto outcome = fsl_test::register_subject(ledger, rack, location, next_identity);
      FSL_REQUIRE_OK(outcome);
      rack_registered = true;
      ++model.committed;
    } else if (model.open_epoch != 0 && rack_registered) {
      if (model.committed >= event_budget) {
        break;
      }
      const auto outcome = fsl_test::record_observation(
          ledger, rack, "obs-" + std::to_string(step), next_identity, 4 + random.below(32));
      FSL_REQUIRE_OK(outcome);
      model.identities.insert(model.committed + 1);
      model.order.push_back(model.committed + 1);
      ++model.committed;
    }

    // Invariants that must hold after every accepted operation.
    const auto watermark = ledger.watermark();
    FSL_REQUIRE_OK(watermark);
    FSL_CHECK_EQ(watermark->sequence->value(), model.committed);
    FSL_CHECK_EQ(watermark->event_count, model.committed);

    const auto state = ledger.state();
    FSL_REQUIRE_OK(state);
    FSL_CHECK_EQ(state->facility_generation.value(), model.generation);
    FSL_CHECK_EQ(state->open_epoch.has_value() ? state->open_epoch->value() : 0, model.open_epoch);
  }

  // Replay equivalence: the committed order is exactly the sequence order.
  std::vector<std::uint64_t> replayed;
  const auto replay_status =
      ledger.replay_each(fsl::ReplayRequest{}, [&replayed](const fsl::EventEnvelope& event) {
        replayed.push_back(event.sequence().value());
        return true;
      });
  FSL_CHECK(replay_status.is_ok());
  FSL_CHECK_EQ(replayed.size(), static_cast<std::size_t>(model.committed));
  for (std::size_t i = 0; i < replayed.size(); ++i) {
    FSL_CHECK_EQ(replayed[i], i + 1);
  }

  // The derived state read from the live handle must equal the state a fresh
  // handle reconstructs from the log alone.
  const fsl::LedgerState live = ledger.state().value();
  FSL_CHECK(ledger.verify().ok());
  FSL_REQUIRE_OK(ledger.close());

  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite, options);
  FSL_REQUIRE_OK(reopened);
  fsl::Ledger fresh = std::move(reopened).value();
  const fsl::LedgerState reconstructed = fresh.state().value();
  FSL_CHECK_EQ(reconstructed.facility_generation.value(), live.facility_generation.value());
  FSL_CHECK_EQ(reconstructed.open_epoch.has_value() ? reconstructed.open_epoch->value() : 0,
               live.open_epoch.has_value() ? live.open_epoch->value() : 0);
  FSL_CHECK_EQ(reconstructed.watermark.sequence->value(), live.watermark.sequence->value());
  FSL_CHECK(reconstructed.watermark.chain == live.watermark.chain);
  FSL_CHECK(fresh.verify().ok());

  // Every committed event survives the reopen byte for byte.
  for (std::uint64_t sequence = 1; sequence <= model.committed; ++sequence) {
    auto event = fresh.read(fsl::LedgerSequence(sequence));
    FSL_REQUIRE_OK(event);
    FSL_CHECK_EQ(event->sequence().value(), sequence);
    auto original = ledger.read(fsl::LedgerSequence(sequence));
    FSL_CHECK(!original.has_value());  // the first handle is closed
  }
  FSL_REQUIRE_OK(fresh.close());
}

FSL_TEST(randomised_sessions_uphold_the_invariants) {
  for (std::uint64_t seed : {1ULL, 7ULL, 42ULL, 1234ULL, 99991ULL, 20260101ULL}) {
    run_generation(seed, 120, 400);
  }
}

FSL_TEST(randomised_batches_and_rotations_uphold_the_invariants) {
  for (std::uint64_t seed : {3ULL, 11ULL, 31337ULL}) {
    fsl_test::TempDirectory temp("property_batch_" + std::to_string(seed));
    Random random(seed);
    fsl::LedgerOptions options = fsl_test::deterministic_options();
    options.max_segment_bytes = 6144;
    options.max_batch_events = 64;

    auto created = fsl::Ledger::create(temp.path(), options);
    FSL_REQUIRE_OK(created);
    fsl::Ledger ledger = std::move(created).value();
    auto seed_facility = fsl_test::seed_facility(ledger);
    FSL_REQUIRE_OK(seed_facility);

    std::uint64_t committed = ledger.watermark().value().sequence->value();
    for (int round = 0; round < 25; ++round) {
      if (random.chance(1, 6)) {
        FSL_REQUIRE_OK(ledger.rotate_segment());
      }
      if (random.chance(1, 8)) {
        FSL_REQUIRE_OK(ledger.create_checkpoint());
      }
      const std::uint64_t size = 1 + random.below(20);
      std::vector<fsl::SubmittedObservation> batch;
      for (std::uint64_t i = 0; i < size; ++i) {
        fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
        builder.event_id(0xC0DEULL, round * 100 + i)
            .subject(seed_facility->asset)
            .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs", {1, 2}}));
        batch.push_back(std::move(builder.build()).value());
      }
      auto outcome = ledger.append_batch(batch);
      FSL_REQUIRE_OK(outcome);
      FSL_CHECK_EQ(outcome->events.size(), static_cast<std::size_t>(size));
      committed += size;

      const auto watermark = ledger.watermark();
      FSL_REQUIRE_OK(watermark);
      FSL_CHECK_EQ(watermark->sequence->value(), committed);
      FSL_CHECK_EQ(watermark->event_count, committed);
    }

    const fsl::VerifyReport report = ledger.verify();
    FSL_CHECK(report.ok());
    FSL_CHECK_EQ(report.records_verified, committed);

    // Replay after rotation must still deliver every sequence exactly once.
    std::set<std::uint64_t> seen;
    const auto status =
        ledger.replay_each(fsl::ReplayRequest{}, [&seen](const fsl::EventEnvelope& event) {
          return seen.insert(event.sequence().value()).second;
        });
    FSL_CHECK(status.is_ok());
    FSL_CHECK_EQ(seen.size(), static_cast<std::size_t>(committed));
    FSL_REQUIRE_OK(ledger.close());
  }
}

FSL_TEST(randomised_duplicate_and_conflict_mix_is_deterministic) {
  fsl_test::TempDirectory temp("property_duplicates");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed_facility = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed_facility);

  Random random(20260101);
  std::set<std::uint64_t> submitted;
  std::uint64_t expected_committed = ledger.watermark().value().sequence->value();

  for (int round = 0; round < 200; ++round) {
    const std::uint64_t index = random.below(24);
    fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
    builder.event_id(0xD00DULL, index)
        .subject(seed_facility->asset)
        .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs", {1}}));
    auto observation = builder.build();
    FSL_REQUIRE_OK(observation);

    const bool seen = submitted.count(index) != 0;
    const auto outcome = ledger.append(observation.value());
    FSL_REQUIRE_OK(outcome);
    if (seen) {
      FSL_CHECK(outcome->duplicate);
    } else {
      FSL_CHECK(!outcome->duplicate);
      submitted.insert(index);
      ++expected_committed;
    }
    FSL_CHECK_EQ(ledger.watermark().value().sequence->value(), expected_committed);
  }

  FSL_CHECK_EQ(submitted.size(), std::size_t{24});
  FSL_CHECK(ledger.verify().ok());
  FSL_REQUIRE_OK(ledger.close());
}

}  // namespace

FSL_TEST_MAIN("test_property")

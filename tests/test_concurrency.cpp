// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_concurrency.cpp
/// The documented concurrency model: mutations are serialised inside one handle
/// and reads are lock-free against the committed prefix.

namespace {

using fsl_test::subject_ref;

FSL_TEST(concurrent_appends_commit_every_event_exactly_once) {
  fsl_test::TempDirectory temp("concurrency_append");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  constexpr int kThreads = 8;
  constexpr int kPerThread = 25;
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&ledger, &seed, &failures, thread]() {
      for (int i = 0; i < kPerThread; ++i) {
        const std::uint64_t identity = static_cast<std::uint64_t>(thread) * 1000 +
                                       static_cast<std::uint64_t>(i);
        const auto outcome = fsl_test::record_observation(ledger, seed->asset, "obs", identity, 8);
        if (!outcome.has_value()) {
          ++failures;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  FSL_CHECK_EQ(failures.load(), 0);

  const std::uint64_t committed = ledger.watermark().value().sequence->value();
  FSL_CHECK_EQ(committed, static_cast<std::uint64_t>(kThreads * kPerThread) + 5);

  std::set<std::uint64_t> sequences;
  for (std::uint64_t sequence = 1; sequence <= committed; ++sequence) {
    auto event = ledger.read(fsl::LedgerSequence(sequence));
    FSL_REQUIRE_OK(event);
    FSL_CHECK(sequences.insert(event->sequence().value()).second);
  }
  FSL_CHECK_EQ(sequences.size(), static_cast<std::size_t>(committed));
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(concurrent_retries_of_one_identity_commit_it_once) {
  fsl_test::TempDirectory temp("concurrency_duplicate");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(0x5EEDULL, 1)
      .subject(seed->asset)
      .token(0x5EEDULL, 2)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs", {1, 2, 3}}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);

  constexpr int kThreads = 8;
  std::atomic<int> committed{0};
  std::atomic<int> duplicates{0};
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&]() {
      const auto outcome = ledger.append(observation.value());
      if (!outcome.has_value()) {
        ++failures;
        return;
      }
      if (outcome->duplicate) {
        ++duplicates;
      } else {
        ++committed;
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  FSL_CHECK_EQ(failures.load(), 0);
  FSL_CHECK_EQ(committed.load(), 1);
  FSL_CHECK_EQ(duplicates.load(), kThreads - 1);

  fsl::Query query;
  query.event_id = observation->event_id();
  query.limit = 16;
  auto page = ledger.query(query);
  FSL_REQUIRE_OK(page);
  FSL_CHECK_EQ(page->events.size(), std::size_t{1});
}

FSL_TEST(readers_observe_a_consistent_committed_prefix_while_a_writer_runs) {
  fsl_test::TempDirectory temp("concurrency_readers");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  std::atomic<bool> writing{true};
  std::atomic<int> reader_failures{0};
  std::atomic<std::uint64_t> observations{0};

  std::thread reader([&]() {
    while (writing.load(std::memory_order_acquire)) {
      const auto watermark = ledger.watermark();
      if (!watermark.has_value()) {
        ++reader_failures;
        break;
      }
      if (!watermark->sequence.has_value()) {
        continue;
      }
      const std::uint64_t upper = watermark->sequence->value();
      // Every sequence at or below the watermark that this reader observes must
      // be readable, and the chain must never go backwards between two reads.
      auto first = ledger.read(fsl::LedgerSequence(1));
      if (!first.has_value()) {
        ++reader_failures;
        break;
      }
      auto last = ledger.read(fsl::LedgerSequence(upper));
      if (!last.has_value()) {
        ++reader_failures;
        break;
      }
      if (last->sequence().value() != upper) {
        ++reader_failures;
        break;
      }
      const auto status = ledger.replay_each(
          fsl::ReplayRequest{}, [upper](const fsl::EventEnvelope& event) {
            return event.sequence().value() < upper;
          });
      if (status.is_error()) {
        ++reader_failures;
        break;
      }
      observations.fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (std::uint64_t i = 0; i < 150; ++i) {
    const auto outcome = fsl_test::record_observation(ledger, seed->asset, "obs", 5000 + i, 8);
    if (!outcome.has_value()) {
      ++reader_failures;
      break;
    }
  }
  writing.store(false, std::memory_order_release);
  reader.join();

  FSL_CHECK_EQ(reader_failures.load(), 0);
  FSL_CHECK(observations.load() > 0);
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(concurrent_batches_do_not_interleave_their_sequences) {
  fsl_test::TempDirectory temp("concurrency_batches");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  constexpr int kThreads = 4;
  constexpr std::uint64_t kBatch = 10;
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&ledger, &seed, &failures, thread]() {
      for (int round = 0; round < 5; ++round) {
        std::vector<fsl::SubmittedObservation> batch;
        for (std::uint64_t i = 0; i < kBatch; ++i) {
          fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
          builder.event_id(0xB000ULL + static_cast<std::uint64_t>(thread),
                           static_cast<std::uint64_t>(round) * 100 + i)
              .subject(seed->asset)
              .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs", {1}}));
          batch.push_back(std::move(builder.build()).value());
        }
        const auto outcome = ledger.append_batch(batch);
        if (!outcome.has_value() || outcome->events.size() != kBatch) {
          ++failures;
          return;
        }
        // A batch is contiguous: its sequences must be consecutive.
        for (std::size_t i = 1; i < outcome->events.size(); ++i) {
          if (outcome->events[i].sequence().value() !=
              outcome->events[i - 1].sequence().value() + 1) {
            ++failures;
            return;
          }
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  FSL_CHECK_EQ(failures.load(), 0);
  FSL_CHECK_EQ(ledger.watermark().value().event_count, 5 + kThreads * 5 * kBatch);
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(shutdown_with_work_queued_commits_only_completed_work) {
  fsl_test::TempDirectory temp("concurrency_shutdown");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);

  std::atomic<std::uint64_t> committed{0};
  std::atomic<bool> stop{false};
  std::atomic<int> post_close_attempts{0};
  std::atomic<int> post_close_errors{0};
  std::thread worker([&]() {
    std::uint64_t index = 0;
    while (!stop.load(std::memory_order_acquire)) {
      const auto outcome = fsl_test::record_observation(ledger, seed->asset, "obs", 9000 + index, 8);
      if (outcome.has_value()) {
        committed.fetch_add(1, std::memory_order_relaxed);
      } else {
        post_close_attempts.fetch_add(1, std::memory_order_relaxed);
        // After close, every attempt must fail in a defined way.
        if (outcome.status().code() == fsl::ErrorCode::kLedgerClosed) {
          post_close_errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
      ++index;
    }
  });

  while (committed.load(std::memory_order_relaxed) < 20) {
    std::this_thread::yield();
  }
  FSL_REQUIRE_OK(ledger.close());
  for (int i = 0; i < 50; ++i) {
    const auto outcome = fsl_test::seed_facility(ledger);
    FSL_CHECK(!outcome.has_value());
    if (!outcome.has_value()) {
      FSL_CHECK_EQ(outcome.status().code(), fsl::ErrorCode::kLedgerClosed);
    }
  }
  stop.store(true, std::memory_order_release);
  worker.join();

  // Everything the handle acknowledged is committed, and the ledger is healthy.
  auto reopened = fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadWrite,
                                    fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reopened);
  fsl::Ledger check = std::move(reopened).value();
  const fsl::VerifyReport report = check.verify();
  FSL_CHECK(report.ok());
  FSL_CHECK(report.records_verified >= committed.load());
  FSL_REQUIRE_OK(check.close());
}

FSL_TEST(a_replay_stream_is_independent_of_concurrent_writers) {
  fsl_test::TempDirectory temp("concurrency_replay");
  auto seeded = fsl_test::open_journal(temp.path());
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  auto seed = fsl_test::seed_facility(ledger);
  FSL_REQUIRE_OK(seed);
  for (std::uint64_t i = 0; i < 20; ++i) {
    FSL_REQUIRE_OK(fsl_test::record_observation(ledger, seed->asset, "obs", 100 + i, 8));
  }

  auto stream = ledger.replay(fsl::ReplayRequest{});
  FSL_REQUIRE_OK(stream);
  const std::uint64_t bound = stream->upper_bound()->value();

  std::atomic<bool> done{false};
  std::thread writer([&]() {
    std::uint64_t index = 0;
    while (!done.load(std::memory_order_acquire)) {
      const auto outcome = fsl_test::record_observation(ledger, seed->asset, "more", 20000 + index, 4);
      if (!outcome.has_value()) {
        break;
      }
      ++index;
    }
  });

  std::uint64_t delivered = 0;
  std::optional<fsl::EventEnvelope> envelope;
  std::uint64_t previous = 0;
  while (true) {
    const fsl::ReplayStep step = stream->next(envelope);
    if (step == fsl::ReplayStep::kEnd) {
      break;
    }
    FSL_REQUIRE(step == fsl::ReplayStep::kEvent);
    FSL_CHECK(envelope->sequence().value() > previous);
    previous = envelope->sequence().value();
    ++delivered;
  }
  FSL_CHECK_EQ(delivered, bound);
  FSL_CHECK(!stream->error().is_error());

  done.store(true, std::memory_order_release);
  writer.join();
}

}  // namespace

FSL_TEST_MAIN("test_concurrency")

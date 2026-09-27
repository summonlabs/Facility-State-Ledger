// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "fsl/clock.hpp"
#include "fsl/ledger.hpp"
#include "fsl/payload.hpp"

/// \file facility_journal.cpp
/// End-to-end walk through the public API: create a ledger, open an epoch,
/// register subjects, record an observation, mutate, assert a relationship,
/// retire, close the epoch, then verify, query, replay and export.
///
/// Everything is deterministic: a ManualClock supplies the recorded monotonic
/// readings and every event identity is derived from a counter, so two runs
/// against the same directory produce the same audit output.

namespace {

int fail(const char* step, const fsl::Status& status) {
  std::fprintf(stderr, "%s failed: %s\n", step, status.to_string().c_str());
  return 1;
}

template <class T>
bool require(const char* step, const fsl::Result<T>& result, T& out) {
  if (!result.has_value()) {
    fail(step, result.status());
    return false;
  }
  out = std::move(const_cast<fsl::Result<T>&>(result).value());
  return true;
}

[[nodiscard]] fsl::SubjectRef subject(fsl::SubjectKind kind, const std::string& key) {
  return std::move(fsl::SubjectRef::create(kind, key).value());
}

[[nodiscard]] fsl::Result<fsl::AppendOutcome> submit(fsl::Ledger& ledger,
                                                     fsl::SubmittedObservationFields fields) {
  auto observation = fsl::SubmittedObservation::create(std::move(fields));
  if (!observation.has_value()) {
    return observation.status();
  }
  return ledger.append(observation.value());
}

[[nodiscard]] fsl::SubmittedObservationFields base_fields(fsl::Ledger& ledger,
                                                          fsl::EventKind kind,
                                                          std::uint64_t index,
                                                          const fsl::SubjectRef& target) {
  const auto state = ledger.state();
  fsl::SubmittedObservationFields fields;
  fields.kind = kind;
  fields.event_id = fsl::EventId::from_words(0x0FAC1A17ULL, index);
  fields.facility_generation = state.value().facility_generation;
  if (kind != fsl::EventKind::kLedgerOpened && kind != fsl::EventKind::kGenerationAdvanced) {
    fields.epoch = state.value().open_epoch;
  }
  fields.subject = target;
  fields.payload_schema = fsl::payload::required_schema(kind);
  fields.payload_schema_version = fsl::payload::required_schema_version(kind);
  auto source = fsl::SourceComponentId::parse("example.facility-journal").value();
  auto provenance = fsl::ProvenanceRecord::create(std::move(source), fsl::SourceGeneration::first(),
                                                  fsl::SourceSequence(index), 1767225600000000000ULL, {});
  fields.provenance = std::move(provenance).value();
  return fields;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path directory =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() / "fsl_facility_journal_example";
  std::error_code error;
  std::filesystem::remove_all(directory, error);

  fsl::ManualClock clock(1'000'000'000ULL, 1767225600000000000ULL);

  fsl::LedgerOptions options;
  options.clock = &clock;
  options.segment_timestamp_unix_nanos = 1767225600000000000ULL;

  std::puts("== create ==");
  auto created = fsl::Ledger::create(directory, options);
  if (!created.has_value()) {
    return fail("create", created.status());
  }
  fsl::Ledger ledger = std::move(created).value();
  std::printf("ledger id   : %s\n", ledger.identity().value().id.to_hex().c_str());
  std::printf("created     : %s\n", ledger.open_report().created ? "yes" : "no");

  const fsl::SubjectRef journal = subject(fsl::SubjectKind::kLedger, "facility-state-ledger");
  const fsl::SubjectRef location = subject(fsl::SubjectKind::kLocation, "site-a/hall-1");
  const fsl::SubjectRef rack = subject(fsl::SubjectKind::kRack, "site-a/hall-1/row-1/rack-1");
  const fsl::SubjectRef power = subject(fsl::SubjectKind::kPowerDomain, "site-a/feed-a");

  std::puts("\n== open epoch 1 ==");
  {
    fsl::SubmittedObservationFields fields =
        base_fields(ledger, fsl::EventKind::kEpochOpened, 1, journal);
    fields.payload = fsl::payload::encode(fsl::payload::EpochOpened{fsl::FacilityEpoch(1),
                                                                    fsl::FacilityGeneration(1)});
    auto outcome = submit(ledger, std::move(fields));
    if (!outcome.has_value()) {
      return fail("open epoch", outcome.status());
    }
    std::printf("sequence %llu committed\n",
                static_cast<unsigned long long>(outcome->event.sequence().value()));
  }

  std::puts("\n== register the facility structure ==");
  const std::pair<const fsl::SubjectRef*, std::optional<fsl::SubjectRef>> registrations[] = {
      {&location, std::nullopt},
      {&rack, std::optional<fsl::SubjectRef>(location)},
      {&power, std::nullopt},
  };
  std::uint64_t index = 2;
  for (const auto& registration : registrations) {
    fsl::SubmittedObservationFields fields =
        base_fields(ledger, fsl::EventKind::kSubjectRegistered, index++, *registration.first);
    fields.payload = fsl::payload::encode(fsl::payload::SubjectRegistered{
        *registration.first, registration.second});
    auto outcome = submit(ledger, std::move(fields));
    if (!outcome.has_value()) {
      return fail("register subject", outcome.status());
    }
    std::printf("registered %s at sequence %llu\n", registration.first->to_string().c_str(),
                static_cast<unsigned long long>(outcome->event.sequence().value()));
  }

  std::puts("\n== record observations ==");
  for (int i = 0; i < 3; ++i) {
    fsl::SubmittedObservationFields fields =
        base_fields(ledger, fsl::EventKind::kObservationAccepted, index++, rack);
    fsl::payload::ObservationAccepted observation;
    observation.observation_ref = "thermal-sample-" + std::to_string(i);
    observation.body.assign(16, static_cast<std::uint8_t>(i));
    fields.payload = fsl::payload::encode(observation);
    auto outcome = submit(ledger, std::move(fields));
    if (!outcome.has_value()) {
      return fail("record observation", outcome.status());
    }
    std::printf("observation %s at sequence %llu\n", observation.observation_ref.c_str(),
                static_cast<unsigned long long>(outcome->event.sequence().value()));
  }

  std::puts("\n== mutate, relate, retire ==");
  {
    fsl::SubmittedObservationFields fields =
        base_fields(ledger, fsl::EventKind::kSubjectMutated, index++, rack);
    fsl::payload::SubjectMutated mutation;
    mutation.mutation_ref = "firmware-update";
    mutation.body = {1, 2, 3, 4};
    fields.payload = fsl::payload::encode(mutation);
    auto outcome = submit(ledger, std::move(fields));
    if (!outcome.has_value()) {
      return fail("mutate", outcome.status());
    }

    fsl::SubmittedObservationFields relation =
        base_fields(ledger, fsl::EventKind::kRelationshipAsserted, index++, rack);
    relation.payload = fsl::payload::encode(
        fsl::payload::Relationship{power, fsl::RelationshipKind::kPoweredBy});
    auto related = submit(ledger, std::move(relation));
    if (!related.has_value()) {
      return fail("assert relationship", related.status());
    }

    fsl::SubmittedObservationFields retirement =
        base_fields(ledger, fsl::EventKind::kSubjectRetired, index++, rack);
    retirement.payload =
        fsl::payload::encode(fsl::payload::SubjectRetired{"rack decommissioned in the example"});
    auto retired = submit(ledger, std::move(retirement));
    if (!retired.has_value()) {
      return fail("retire", retired.status());
    }
    std::printf("rack retired at sequence %llu\n",
                static_cast<unsigned long long>(retired->event.sequence().value()));
  }

  std::puts("\n== close epoch 1 ==");
  {
    fsl::SubmittedObservationFields fields =
        base_fields(ledger, fsl::EventKind::kEpochClosed, index++, journal);
    fields.payload = fsl::payload::encode(fsl::payload::EpochClosed{fsl::FacilityEpoch(1)});
    auto outcome = submit(ledger, std::move(fields));
    if (!outcome.has_value()) {
      return fail("close epoch", outcome.status());
    }
  }

  std::puts("\n== state ==");
  const auto state = ledger.state();
  if (!state.has_value()) {
    return fail("state", state.status());
  }
  std::printf("committed sequence : %llu\n",
              static_cast<unsigned long long>(state->watermark.sequence->value()));
  std::printf("committed events   : %llu\n",
              static_cast<unsigned long long>(state->watermark.event_count));
  std::printf("facility generation: %llu\n",
              static_cast<unsigned long long>(state->facility_generation.value()));
  std::printf("open epoch         : %s\n",
              state->open_epoch.has_value() ? std::to_string(state->open_epoch->value()).c_str()
                                            : "none");
  std::printf("latest epoch       : %s\n",
              state->latest_epoch.has_value() ? std::to_string(state->latest_epoch->value()).c_str()
                                              : "none");
  std::printf("segments           : %llu\n",
              static_cast<unsigned long long>(state->segment_count));

  std::puts("\n== verify ==");
  const fsl::VerifyReport report = ledger.verify();
  std::printf("verification: %s (%llu records over %llu segments, chain %s)\n",
              report.status.to_string().c_str(),
              static_cast<unsigned long long>(report.records_verified),
              static_cast<unsigned long long>(report.segments_verified),
              report.chain_at_end.to_hex().c_str());
  if (!report.ok()) {
    return 1;
  }

  std::puts("\n== bounded query for the rack ==");
  fsl::Query query;
  query.subject = rack;
  query.limit = 10;
  auto page = ledger.query(query);
  if (!page.has_value()) {
    return fail("query", page.status());
  }
  std::printf("%llu records for %s, examined %llu, source %s\n",
              static_cast<unsigned long long>(page->events.size()), rack.to_string().c_str(),
              static_cast<unsigned long long>(page->records_examined),
              std::string(fsl::to_string(page->source)).c_str());
  for (const fsl::EventEnvelope& event : page->events) {
    std::printf("  %llu %s\n", static_cast<unsigned long long>(event.sequence().value()),
                std::string(fsl::to_string(event.kind())).c_str());
  }

  std::puts("\n== replay ==");
  std::uint64_t replayed = 0;
  const fsl::Result<std::uint64_t> replay_result =
      ledger.replay_each(fsl::ReplayRequest{}, [&replayed](const fsl::EventEnvelope& event) {
        std::printf("  %llu %-24s %s\n", static_cast<unsigned long long>(event.sequence().value()),
                    std::string(fsl::to_string(event.kind())).c_str(),
                    event.subject().to_string().c_str());
        ++replayed;
        return true;
      });
  if (!replay_result.has_value()) {
    return fail("replay", replay_result.status());
  }
  std::printf("replayed %llu events\n", static_cast<unsigned long long>(replayed));

  std::puts("\n== canonical audit export ==");
  auto export_text = ledger.export_audit_json();
  if (!export_text.has_value()) {
    return fail("export", export_text.status());
  }
  std::fputs(export_text->c_str(), stdout);

  const auto closed = ledger.close();
  if (!closed.has_value()) {
    return fail("close", closed);
  }

  std::puts("\n== reopen as a reader ==");
  auto reopened = fsl::Ledger::open(directory, fsl::OpenMode::kReadOnly, options);
  if (!reopened.has_value()) {
    return fail("reopen", reopened.status());
  }
  fsl::Ledger reader = std::move(reopened).value();
  const auto reader_state = reader.state();
  if (!reader_state.has_value()) {
    return fail("reader state", reader_state.status());
  }
  std::printf("reopened at sequence %llu, writable=%s, replayed %llu events to rebuild state\n",
              static_cast<unsigned long long>(reader_state->watermark.sequence->value()),
              reader.is_writable() ? "yes" : "no",
              static_cast<unsigned long long>(reader.open_report().replayed_events));
  const fsl::VerifyReport reread = reader.verify();
  std::printf("reverified: %s\n", reread.status.to_string().c_str());
  const auto closed_reader = reader.close();
  if (!closed_reader.has_value()) {
    return fail("close reader", closed_reader);
  }

  std::printf("\nexample complete; ledger left at %s\n", directory.string().c_str());
  return reread.ok() ? 0 : 1;
}

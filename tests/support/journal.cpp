// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "support/journal.hpp"

#include <utility>

namespace fsl_test {
namespace {

[[nodiscard]] fsl::Result<fsl::AppendOutcome> submit(fsl::Ledger& ledger,
                                                     fsl::SubmittedObservationFields fields) {
  auto observation = fsl::SubmittedObservation::create(std::move(fields));
  if (!observation.has_value()) {
    return observation.status();
  }
  return ledger.append(observation.value());
}

/// Supplies the generation and epoch that are current for the handle, so that a
/// helper never fabricates a stale envelope. Lifecycle kinds that must not name
/// an epoch are left alone.
void adopt_current_authority(const fsl::Ledger& ledger, fsl::SubmittedObservationFields& fields) {
  const auto state = ledger.state();
  if (!state.has_value()) {
    return;
  }
  const bool epoch_control = fields.kind.has_value() &&
                             (*fields.kind == fsl::EventKind::kLedgerOpened ||
                              *fields.kind == fsl::EventKind::kGenerationAdvanced);
  if (epoch_control) {
    return;
  }
  if (state->open_epoch.has_value()) {
    fields.epoch = state->open_epoch;
  }
  fields.facility_generation = state->facility_generation;
}

}  // namespace

fsl::Result<fsl::Ledger> open_journal(const std::filesystem::path& dir,
                                      const fsl::LedgerOptions& options) {
  std::error_code error;
  const bool exists = std::filesystem::exists(dir / "ledger.manifest", error) && !error;
  if (exists) {
    return fsl::Ledger::open(dir, fsl::OpenMode::kReadWrite, options);
  }
  return fsl::Ledger::create(dir, options);
}

fsl::Result<fsl::AppendOutcome> open_epoch(fsl::Ledger& ledger,
                                           std::uint64_t epoch,
                                           std::uint64_t generation,
                                           std::uint64_t identity_index) {
  SubmissionBuilder builder(fsl::EventKind::kEpochOpened);
  builder.event_id(0xE001ULL, identity_index)
      .epoch(epoch)
      .generation(generation)
      .subject(fsl::SubjectKind::kLedger, "facility-state-ledger")
      .payload(fsl::payload::encode(fsl::payload::EpochOpened{fsl::FacilityEpoch(epoch),
                                                              fsl::FacilityGeneration(generation)}));
  return submit(ledger, builder.fields());
}

fsl::Result<fsl::AppendOutcome> close_epoch(fsl::Ledger& ledger,
                                            std::uint64_t epoch,
                                            std::uint64_t identity_index) {
  SubmissionBuilder builder(fsl::EventKind::kEpochClosed);
  builder.event_id(0xE002ULL, identity_index)
      .epoch(epoch)
      .subject(fsl::SubjectKind::kLedger, "facility-state-ledger")
      .payload(fsl::payload::encode(fsl::payload::EpochClosed{fsl::FacilityEpoch(epoch)}));
  // Closing an epoch belongs to the generation that is current when it closes,
  // which the caller does not restate.
  fsl::SubmittedObservationFields built = builder.fields();
  const auto state = ledger.state();
  if (state.has_value()) {
    built.facility_generation = state->facility_generation;
  }
  return submit(ledger, std::move(built));
}

fsl::Result<fsl::AppendOutcome> advance_generation(fsl::Ledger& ledger,
                                                   std::uint64_t from,
                                                   std::uint64_t identity_index) {
  // The generation may only advance while no epoch is open, so this event
  // deliberately carries no epoch.
  SubmissionBuilder builder(fsl::EventKind::kGenerationAdvanced);
  builder.event_id(0xE003ULL, identity_index)
      .generation(from)
      .subject(fsl::SubjectKind::kLedger, "facility-state-ledger")
      .payload(fsl::payload::encode(fsl::payload::GenerationAdvanced{fsl::FacilityGeneration(from),
                                                                     fsl::FacilityGeneration(from + 1)}));
  return submit(ledger, builder.fields());
}

fsl::Result<fsl::AppendOutcome> register_subject(fsl::Ledger& ledger,
                                                 const fsl::SubjectRef& subject,
                                                 const std::optional<fsl::SubjectRef>& location,
                                                 std::uint64_t identity_index) {
  SubmissionBuilder builder(fsl::EventKind::kSubjectRegistered);
  builder.event_id(0xE004ULL, identity_index)
      .subject(subject)
      .payload(fsl::payload::encode(fsl::payload::SubjectRegistered{subject, location}));
  fsl::SubmittedObservationFields built = builder.fields();
  adopt_current_authority(ledger, built);
  return submit(ledger, std::move(built));
}

fsl::Result<fsl::AppendOutcome> retire_subject(fsl::Ledger& ledger,
                                               const fsl::SubjectRef& subject,
                                               const std::string& reason,
                                               std::uint64_t identity_index) {
  SubmissionBuilder builder(fsl::EventKind::kSubjectRetired);
  builder.event_id(0xE005ULL, identity_index)
      .subject(subject)
      .payload(fsl::payload::encode(fsl::payload::SubjectRetired{reason}));
  fsl::SubmittedObservationFields built = builder.fields();
  adopt_current_authority(ledger, built);
  return submit(ledger, std::move(built));
}

fsl::Result<fsl::AppendOutcome> record_observation(fsl::Ledger& ledger,
                                                   const fsl::SubjectRef& subject,
                                                   const std::string& reference,
                                                   std::uint64_t identity_index,
                                                   std::size_t body_bytes) {
  SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  fsl::payload::ObservationAccepted observation;
  observation.observation_ref = reference;
  observation.body.assign(body_bytes, static_cast<std::uint8_t>(identity_index & 0xFFu));
  builder.event_id(0xE006ULL, identity_index).subject(subject).payload(fsl::payload::encode(observation));
  fsl::SubmittedObservationFields built = builder.fields();
  adopt_current_authority(ledger, built);
  return submit(ledger, std::move(built));
}

fsl::Result<fsl::AppendOutcome> mutate_subject(fsl::Ledger& ledger,
                                               const fsl::SubjectRef& subject,
                                               const std::string& reference,
                                               std::uint64_t identity_index) {
  SubmissionBuilder builder(fsl::EventKind::kSubjectMutated);
  fsl::payload::SubjectMutated mutation;
  mutation.mutation_ref = reference;
  mutation.body.assign(16, static_cast<std::uint8_t>(identity_index & 0xFFu));
  builder.event_id(0xE007ULL, identity_index).subject(subject).payload(fsl::payload::encode(mutation));
  fsl::SubmittedObservationFields built = builder.fields();
  adopt_current_authority(ledger, built);
  return submit(ledger, std::move(built));
}

fsl::Result<fsl::AppendOutcome> assert_relationship(fsl::Ledger& ledger,
                                                    const fsl::SubjectRef& subject,
                                                    const fsl::SubjectRef& related,
                                                    fsl::RelationshipKind kind,
                                                    std::uint64_t identity_index) {
  SubmissionBuilder builder(fsl::EventKind::kRelationshipAsserted);
  builder.event_id(0xE008ULL, identity_index)
      .subject(subject)
      .payload(fsl::payload::encode(fsl::payload::Relationship{related, kind}));
  fsl::SubmittedObservationFields built = builder.fields();
  adopt_current_authority(ledger, built);
  return submit(ledger, std::move(built));
}

fsl::Result<fsl::AppendOutcome> retract_relationship(fsl::Ledger& ledger,
                                                     const fsl::SubjectRef& subject,
                                                     const fsl::SubjectRef& related,
                                                     fsl::RelationshipKind kind,
                                                     std::uint64_t identity_index) {
  SubmissionBuilder builder(fsl::EventKind::kRelationshipRetracted);
  builder.event_id(0xE009ULL, identity_index)
      .subject(subject)
      .payload(fsl::payload::encode(fsl::payload::Relationship{related, kind}));
  fsl::SubmittedObservationFields built = builder.fields();
  adopt_current_authority(ledger, built);
  return submit(ledger, std::move(built));
}

fsl::Result<FacilitySeed> seed_facility(fsl::Ledger& ledger, std::uint64_t first_identity_index) {
  auto epoch = open_epoch(ledger, 1, 1, first_identity_index);
  if (!epoch.has_value()) {
    return epoch.status();
  }
  FacilitySeed seed{subject_ref(fsl::SubjectKind::kLocation, "site-a.hall-1"),
                    subject_ref(fsl::SubjectKind::kRack, "site-a.hall-1.row-1.rack-1"),
                    subject_ref(fsl::SubjectKind::kAsset, "site-a.hall-1.row-1.rack-1.asset-1")};

  auto location = register_subject(ledger, seed.location, std::nullopt, first_identity_index + 1);
  if (!location.has_value()) {
    return location.status();
  }
  auto rack = register_subject(ledger, seed.rack, seed.location, first_identity_index + 2);
  if (!rack.has_value()) {
    return rack.status();
  }
  auto asset = register_subject(ledger, seed.asset, seed.location, first_identity_index + 3);
  if (!asset.has_value()) {
    return asset.status();
  }
  return seed;
}

fsl::Result<std::vector<std::uint64_t>> replay_sequences(const fsl::Ledger& ledger) {
  std::vector<std::uint64_t> sequences;
  fsl::ReplayRequest request;
  const fsl::Result<std::uint64_t> result =
      ledger.replay_each(request, [&sequences](const fsl::EventEnvelope& event) {
        sequences.push_back(event.sequence().value());
        return true;
      });
  if (!result.has_value()) {
    return result.status();
  }
  return sequences;
}

}  // namespace fsl_test

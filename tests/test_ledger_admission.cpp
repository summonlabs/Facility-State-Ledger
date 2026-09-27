// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_ledger_admission.cpp
/// The admission state machine: epoch lifecycle, generation advance, subject
/// lifecycle, referential integrity, and producer-incarnation fencing.

namespace {

using fsl_test::subject_ref;

[[nodiscard]] fsl::Result<fsl::Ledger> open_seeded(fsl_test::TempDirectory& temp) {
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  if (!created.has_value()) {
    return created.status();
  }
  fsl::Ledger ledger = std::move(created).value();
  auto seed = fsl_test::seed_facility(ledger);
  if (!seed.has_value()) {
    return seed.status();
  }
  return ledger;
}

FSL_TEST(facility_events_are_refused_while_no_epoch_is_open) {
  fsl_test::TempDirectory temp("admission_no_epoch");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kSubjectRegistered);
  builder.event_id(1, 1).subject(fsl::SubjectKind::kLocation, "site-a").payload(
      fsl::payload::encode(fsl::payload::SubjectRegistered{
          subject_ref(fsl::SubjectKind::kLocation, "site-a"), std::nullopt}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_ERROR(ledger.append(observation.value()), fsl::ErrorCode::kUnknownEpoch);
  FSL_CHECK_EQ(ledger.append(observation.value()).status().category(),
               fsl::ErrorCategory::kStaleEpoch);
}

FSL_TEST(epoch_lifecycle_is_enforced) {
  fsl_test::TempDirectory temp("admission_epochs");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();

  FSL_REQUIRE_OK(fsl_test::open_epoch(ledger, 1, 1, 1));
  // A second open of the same epoch, and opening a non-consecutive epoch, are
  // both lifecycle violations.
  FSL_REQUIRE_ERROR(fsl_test::open_epoch(ledger, 1, 1, 2), fsl::ErrorCode::kIllegalLifecycleTransition);
  FSL_REQUIRE_ERROR(fsl_test::open_epoch(ledger, 5, 1, 3), fsl::ErrorCode::kIllegalLifecycleTransition);
  FSL_REQUIRE_ERROR(fsl_test::close_epoch(ledger, 2, 4), fsl::ErrorCode::kStaleEpoch);

  FSL_CHECK(ledger.state().value().open_epoch.has_value());
  FSL_REQUIRE_OK(fsl_test::close_epoch(ledger, 1, 5));
  FSL_CHECK(!ledger.state().value().open_epoch.has_value());
  FSL_CHECK_EQ(ledger.state().value().latest_epoch->value(), std::uint64_t{1});

  // After the close, facility events are refused again.
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kSubjectRegistered);
  builder.event_id(1, 9).subject(fsl::SubjectKind::kLocation, "site-a").payload(
      fsl::payload::encode(fsl::payload::SubjectRegistered{
          subject_ref(fsl::SubjectKind::kLocation, "site-a"), std::nullopt}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_ERROR(ledger.append(observation.value()), fsl::ErrorCode::kUnknownEpoch);

  // The next epoch must be the successor, and it accepts events again.
  FSL_REQUIRE_OK(fsl_test::open_epoch(ledger, 2, 1, 10));
  FSL_CHECK_EQ(ledger.state().value().open_epoch->value(), std::uint64_t{2});
  FSL_REQUIRE_ERROR(fsl_test::close_epoch(ledger, 1, 11), fsl::ErrorCode::kStaleEpoch);
}

FSL_TEST(a_stale_epoch_envelope_is_rejected_after_the_epoch_closes) {
  fsl_test::TempDirectory temp("admission_stale_epoch");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  FSL_REQUIRE_OK(fsl_test::open_epoch(ledger, 1, 1, 1));
  FSL_REQUIRE_OK(fsl_test::close_epoch(ledger, 1, 2));
  FSL_REQUIRE_OK(fsl_test::open_epoch(ledger, 2, 1, 3));

  // An event that still names epoch 1 is stale even though epoch 2 is open.
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kSubjectRegistered);
  builder.event_id(1, 4)
      .epoch(1)
      .subject(fsl::SubjectKind::kLocation, "site-a")
      .payload(fsl::payload::encode(fsl::payload::SubjectRegistered{
          subject_ref(fsl::SubjectKind::kLocation, "site-a"), std::nullopt}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_ERROR(ledger.append(observation.value()), fsl::ErrorCode::kEpochClosed);
}

FSL_TEST(generation_advance_requires_a_closed_epoch_and_the_exact_predecessor) {
  fsl_test::TempDirectory temp("admission_generation");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();

  // No epoch is open on a fresh ledger, so the first advance is admissible.
  FSL_REQUIRE_OK(fsl_test::advance_generation(ledger, 1, 1));
  FSL_CHECK_EQ(ledger.state().value().facility_generation.value(), std::uint64_t{2});

  // A stale advance from the previous generation is refused.
  FSL_REQUIRE_ERROR(fsl_test::advance_generation(ledger, 1, 2), fsl::ErrorCode::kStaleFacilityGeneration);
  // An advance from a future generation is refused as well.
  // A generation ahead of the authoritative one is reported as future, which\n  // is a distinct and more precise outcome than stale.\n  FSL_REQUIRE_ERROR(fsl_test::advance_generation(ledger, 9, 3), fsl::ErrorCode::kFutureFacilityGeneration);

  // While an epoch is open the generation may not advance.
  FSL_REQUIRE_OK(fsl_test::open_epoch(ledger, 1, 2, 4));
  FSL_REQUIRE_ERROR(fsl_test::advance_generation(ledger, 2, 5), fsl::ErrorCode::kIllegalLifecycleTransition);
}

FSL_TEST(an_envelope_for_a_superseded_generation_is_rejected) {
  fsl_test::TempDirectory temp("admission_stale_generation");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  FSL_REQUIRE_OK(fsl_test::advance_generation(ledger, 1, 1));

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kEpochOpened);
  builder.event_id(1, 2)
      .generation(1)
      .epoch(1)
      .subject(fsl::SubjectKind::kLedger, "facility-state-ledger")
      .payload(fsl::payload::encode(fsl::payload::EpochOpened{fsl::FacilityEpoch(1),
                                                              fsl::FacilityGeneration(1)}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_ERROR(ledger.append(observation.value()), fsl::ErrorCode::kStaleFacilityGeneration);

  fsl_test::SubmissionBuilder future(fsl::EventKind::kEpochOpened);
  future.event_id(1, 3)
      .generation(9)
      .epoch(1)
      .subject(fsl::SubjectKind::kLedger, "facility-state-ledger")
      .payload(fsl::payload::encode(fsl::payload::EpochOpened{fsl::FacilityEpoch(1),
                                                              fsl::FacilityGeneration(9)}));
  auto future_observation = future.build();
  FSL_REQUIRE_OK(future_observation);
  FSL_REQUIRE_ERROR(ledger.append(future_observation.value()), fsl::ErrorCode::kFutureFacilityGeneration);
}

FSL_TEST(subject_registration_requires_a_location_for_physical_entities) {
  fsl_test::TempDirectory temp("admission_location");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  FSL_REQUIRE_OK(fsl_test::open_epoch(ledger, 1, 1, 1));

  // A rack without a location is refused.
  FSL_REQUIRE_ERROR(
      fsl_test::register_subject(ledger, subject_ref(fsl::SubjectKind::kRack, "rack-1"), std::nullopt, 2),
      fsl::ErrorCode::kMissingSubjectLocation);

  // A rack whose location is unknown is refused as a dangling reference.
  FSL_REQUIRE_ERROR(fsl_test::register_subject(ledger, subject_ref(fsl::SubjectKind::kRack, "rack-1"),
                                               subject_ref(fsl::SubjectKind::kLocation, "nowhere"), 3),
                    fsl::ErrorCode::kDanglingSubjectReference);

  // A domain subject needs no location.
  FSL_REQUIRE_OK(fsl_test::register_subject(ledger, subject_ref(fsl::SubjectKind::kPowerDomain, "feed-a"),
                                            std::nullopt, 4));

  // A location may be a domain subject of the wrong kind; the payload requires
  // an explicit location reference.
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kSubjectRegistered);
  builder.event_id(1, 5).subject(fsl::SubjectKind::kRack, "rack-2").payload(
      fsl::payload::encode(fsl::payload::SubjectRegistered{
          subject_ref(fsl::SubjectKind::kPowerDomain, "feed-a"), std::nullopt}));
  auto mismatch = builder.build();
  FSL_REQUIRE_OK(mismatch);
  FSL_REQUIRE_ERROR(ledger.append(mismatch.value()), fsl::ErrorCode::kSubjectKindMismatch);
}

FSL_TEST(subject_lifecycle_rejects_duplicate_and_missing_subjects) {
  fsl_test::TempDirectory temp("admission_subject_lifecycle");
  auto seeded = open_seeded(temp);
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  const fsl::SubjectRef rack = subject_ref(fsl::SubjectKind::kRack, "site-a.hall-1.row-1.rack-1");
  const fsl::SubjectRef location = subject_ref(fsl::SubjectKind::kLocation, "site-a.hall-1");

  FSL_REQUIRE_ERROR(fsl_test::register_subject(ledger, rack, location, 200),
                    fsl::ErrorCode::kSubjectAlreadyRegistered);

  const fsl::SubjectRef unknown = subject_ref(fsl::SubjectKind::kAsset, "site-a.asset-9");
  FSL_REQUIRE_ERROR(fsl_test::record_observation(ledger, unknown, "obs", 201),
                    fsl::ErrorCode::kSubjectNotRegistered);
  FSL_REQUIRE_ERROR(fsl_test::retire_subject(ledger, unknown, "gone", 202),
                    fsl::ErrorCode::kSubjectNotRegistered);

  FSL_REQUIRE_OK(fsl_test::retire_subject(ledger, rack, "decommissioned", 203));
  FSL_REQUIRE_ERROR(fsl_test::retire_subject(ledger, rack, "again", 204), fsl::ErrorCode::kSubjectRetired);
  FSL_REQUIRE_ERROR(fsl_test::record_observation(ledger, rack, "obs", 205), fsl::ErrorCode::kSubjectRetired);
  FSL_REQUIRE_ERROR(fsl_test::mutate_subject(ledger, rack, "mut", 206), fsl::ErrorCode::kSubjectRetired);

  const auto view = ledger.subject(rack);
  FSL_REQUIRE_OK(view);
  FSL_CHECK(view->registered);
  FSL_CHECK(view->retired);
  FSL_CHECK_EQ(view->event_count, std::uint64_t{2});
}

FSL_TEST(relationship_assertions_require_known_distinct_endpoints) {
  fsl_test::TempDirectory temp("admission_relationships");
  auto seeded = open_seeded(temp);
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  const fsl::SubjectRef rack = subject_ref(fsl::SubjectKind::kRack, "site-a.hall-1.row-1.rack-1");
  const fsl::SubjectRef power = subject_ref(fsl::SubjectKind::kPowerDomain, "feed-a");
  FSL_REQUIRE_OK(fsl_test::register_subject(ledger, power, std::nullopt, 300));

  FSL_REQUIRE_ERROR(fsl_test::assert_relationship(ledger, rack, rack, fsl::RelationshipKind::kDependsOn,
                                                  301),
                    fsl::ErrorCode::kSelfReference);
  FSL_REQUIRE_ERROR(
      fsl_test::assert_relationship(ledger, rack, subject_ref(fsl::SubjectKind::kAsset, "ghost"),
                                    fsl::RelationshipKind::kDependsOn, 302),
      fsl::ErrorCode::kDanglingSubjectReference);

  FSL_REQUIRE_OK(
      fsl_test::assert_relationship(ledger, rack, power, fsl::RelationshipKind::kPoweredBy, 303));
  FSL_REQUIRE_ERROR(
      fsl_test::assert_relationship(ledger, rack, power, fsl::RelationshipKind::kPoweredBy, 304),
      fsl::ErrorCode::kIllegalLifecycleTransition);
  FSL_REQUIRE_ERROR(
      fsl_test::retract_relationship(ledger, rack, power, fsl::RelationshipKind::kCooledBy, 305),
      fsl::ErrorCode::kIllegalLifecycleTransition);
  FSL_REQUIRE_OK(
      fsl_test::retract_relationship(ledger, rack, power, fsl::RelationshipKind::kPoweredBy, 306));
  FSL_REQUIRE_OK(
      fsl_test::assert_relationship(ledger, rack, power, fsl::RelationshipKind::kPoweredBy, 307));
}

FSL_TEST(corrections_require_an_existing_target_and_never_modify_it) {
  fsl_test::TempDirectory temp("admission_corrections");
  auto seeded = open_seeded(temp);
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  const fsl::SubjectRef asset = subject_ref(fsl::SubjectKind::kAsset, "site-a.hall-1.row-1.rack-1.asset-1");

  auto observation = fsl_test::record_observation(ledger, asset, "obs-1", 400);
  FSL_REQUIRE_OK(observation);
  const fsl::EventId target = observation->event.event_id();
  const fsl::LedgerSequence target_sequence = observation->event.sequence();
  const fsl::Digest target_integrity = observation->event.integrity();

  fsl_test::SubmissionBuilder missing(fsl::EventKind::kCorrectionRecorded);
  missing.event_id(1, 401)
      .subject(asset)
      .correction_target(0xDEADULL, 0xBEEFULL)
      .payload(fsl::payload::encode(fsl::payload::CorrectionRecorded{"wrong rack"}));
  auto bad = missing.build();
  FSL_REQUIRE_OK(bad);
  FSL_REQUIRE_ERROR(ledger.append(bad.value()), fsl::ErrorCode::kCorrectionTargetNotFound);

  fsl_test::SubmissionBuilder correcting(fsl::EventKind::kCorrectionRecorded);
  correcting.event_id(1, 402)
      .subject(asset)
      .correction_target(target.high(), target.low())
      .payload(fsl::payload::encode(fsl::payload::CorrectionRecorded{"the asset moved"}));
  auto correction = correcting.build();
  FSL_REQUIRE_OK(correction);
  FSL_REQUIRE_OK(ledger.append(correction.value()));

  // The corrected record is unchanged: its bytes, identity, sequence and
  // integrity digest are exactly what they were.
  auto reread = ledger.read(target_sequence);
  FSL_REQUIRE_OK(reread);
  FSL_CHECK(reread->event_id() == target);
  FSL_CHECK(reread->integrity() == target_integrity);
  FSL_CHECK(reread->kind() == fsl::EventKind::kObservationAccepted);
}

FSL_TEST(reconciliation_is_admitted_for_retired_subjects) {
  fsl_test::TempDirectory temp("admission_reconciliation");
  auto seeded = open_seeded(temp);
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  const fsl::SubjectRef asset = subject_ref(fsl::SubjectKind::kAsset, "site-a.hall-1.row-1.rack-1.asset-1");
  FSL_REQUIRE_OK(fsl_test::retire_subject(ledger, asset, "removed", 500));

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kReconciliationRecorded);
  builder.event_id(1, 501).subject(asset).payload(fsl::payload::encode(
      fsl::payload::ReconciliationRecorded{"recon-1", {1, 2, 3}}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_OK(ledger.append(observation.value()));

  // A relationship against a retired subject is still refused.
  FSL_REQUIRE_ERROR(fsl_test::assert_relationship(
                        ledger, asset, subject_ref(fsl::SubjectKind::kRack, "site-a.hall-1.row-1.rack-1"),
                        fsl::RelationshipKind::kContainedIn, 502),
                    fsl::ErrorCode::kSubjectRetired);
}

FSL_TEST(a_superseded_producer_incarnation_cannot_publish) {
  fsl_test::TempDirectory temp("admission_source");
  auto seeded = open_seeded(temp);
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  const fsl::SubjectRef asset = subject_ref(fsl::SubjectKind::kAsset, "site-a.hall-1.row-1.rack-1.asset-1");

  fsl_test::SubmissionBuilder first(fsl::EventKind::kObservationAccepted);
  first.event_id(1, 600)
      .subject(asset)
      .source("dccp.topology-agent")
      .source_generation(2)
      .source_sequence(10)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-a", {1}}));
  auto observation = first.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_OK(ledger.append(observation.value()));

  // The same source presenting an older incarnation is fenced out.
  fsl_test::SubmissionBuilder stale(fsl::EventKind::kObservationAccepted);
  stale.event_id(1, 601)
      .subject(asset)
      .source("dccp.topology-agent")
      .source_generation(1)
      .source_sequence(1)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-b", {2}}));
  auto stale_observation = stale.build();
  FSL_REQUIRE_OK(stale_observation);
  FSL_REQUIRE_ERROR(ledger.append(stale_observation.value()), fsl::ErrorCode::kStaleSourceGeneration);

  // Within one incarnation, a source sequence must strictly advance.
  fsl_test::SubmissionBuilder replayed(fsl::EventKind::kObservationAccepted);
  replayed.event_id(1, 602)
      .subject(asset)
      .source("dccp.topology-agent")
      .source_generation(2)
      .source_sequence(10)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-c", {3}}));
  auto replayed_observation = replayed.build();
  FSL_REQUIRE_OK(replayed_observation);
  FSL_REQUIRE_ERROR(ledger.append(replayed_observation.value()), fsl::ErrorCode::kSourceSequenceRegression);

  // A new incarnation restarts the source sequence freely.
  fsl_test::SubmissionBuilder restarted(fsl::EventKind::kObservationAccepted);
  restarted.event_id(1, 603)
      .subject(asset)
      .source("dccp.topology-agent")
      .source_generation(3)
      .source_sequence(1)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-d", {4}}));
  auto restarted_observation = restarted.build();
  FSL_REQUIRE_OK(restarted_observation);
  FSL_REQUIRE_OK(ledger.append(restarted_observation.value()));

  const auto sources = ledger.sources(16);
  FSL_REQUIRE_OK(sources);
  bool found = false;
  for (const fsl::SourceView& view : sources.value()) {
    if (view.source.str() == "dccp.topology-agent") {
      found = true;
      FSL_CHECK_EQ(view.generation.value(), std::uint64_t{3});
      FSL_CHECK(view.last_source_sequence.has_value());
      FSL_CHECK_EQ(view.last_source_sequence->value(), std::uint64_t{1});
    }
  }
  FSL_CHECK(found);
}

FSL_TEST(a_payload_larger_than_the_configured_bound_is_refused) {
  fsl_test::TempDirectory temp("admission_payload");
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  options.max_payload_bytes = 64;
  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  FSL_REQUIRE_OK(fsl_test::open_epoch(ledger, 1, 1, 1));
  FSL_REQUIRE_OK(fsl_test::register_subject(ledger, subject_ref(fsl::SubjectKind::kPowerDomain, "feed-a"),
                                            std::nullopt, 2));

  FSL_REQUIRE_OK(fsl_test::record_observation(ledger, subject_ref(fsl::SubjectKind::kPowerDomain, "feed-a"),
                                              "small", 3, 4));
  FSL_REQUIRE_ERROR(fsl_test::record_observation(ledger, subject_ref(fsl::SubjectKind::kPowerDomain,
                                                                     "feed-a"),
                                                 "large", 4, 4096),
                    fsl::ErrorCode::kSizeOutOfRange);
}

FSL_TEST(an_unsupported_payload_schema_is_refused) {
  fsl_test::TempDirectory temp("admission_schema");
  auto seeded = open_seeded(temp);
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  const fsl::SubjectRef asset = subject_ref(fsl::SubjectKind::kAsset, "site-a.hall-1.row-1.rack-1.asset-1");

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(1, 700)
      .subject(asset)
      .payload_schema("fsl.observation-accepted.v2")
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs", {1}}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  FSL_REQUIRE_ERROR(ledger.append(observation.value()), fsl::ErrorCode::kUnsupportedPayloadSchema);

  fsl_test::SubmissionBuilder wrong_kind(fsl::EventKind::kObservationAccepted);
  wrong_kind.event_id(1, 701)
      .subject(asset)
      .payload_schema("fsl.subject-retired.v1")
      .payload(fsl::payload::encode(fsl::payload::SubjectRetired{"reason"}));
  auto mismatched = wrong_kind.build();
  FSL_REQUIRE_OK(mismatched);
  FSL_REQUIRE_ERROR(ledger.append(mismatched.value()), fsl::ErrorCode::kUnsupportedPayloadSchema);
}

FSL_TEST(a_rejected_append_leaves_the_committed_prefix_untouched) {
  fsl_test::TempDirectory temp("admission_reject");
  auto seeded = open_seeded(temp);
  FSL_REQUIRE_OK(seeded);
  fsl::Ledger ledger = std::move(seeded).value();
  const std::uint64_t before = ledger.watermark().value().sequence->value();
  const fsl::Digest chain_before = ledger.watermark().value().chain;

  FSL_REQUIRE_ERROR(fsl_test::record_observation(
                        ledger, subject_ref(fsl::SubjectKind::kAsset, "site-a.ghost"), "obs", 800),
                    fsl::ErrorCode::kSubjectNotRegistered);
  FSL_REQUIRE_ERROR(fsl_test::register_subject(ledger, subject_ref(fsl::SubjectKind::kRack, "r-9"),
                                               std::nullopt, 801),
                    fsl::ErrorCode::kMissingSubjectLocation);

  const std::uint64_t after = ledger.watermark().value().sequence->value();
  FSL_CHECK_EQ(before, after);
  FSL_CHECK(ledger.watermark().value().chain == chain_before);
  FSL_CHECK_EQ(ledger.stats().rejected_appends, std::uint64_t{2});
  FSL_CHECK(ledger.verify().ok());
}

FSL_TEST(entity_capacity_bounds_are_enforced_before_mutation) {
  fsl_test::TempDirectory temp("admission_capacity");
  fsl::LedgerOptions options = fsl_test::deterministic_options();
  // The ledger itself and the test harness already occupy two source slots.
  options.max_sources = 3;
  auto created = fsl::Ledger::create(temp.path(), options);
  FSL_REQUIRE_OK(created);
  fsl::Ledger ledger = std::move(created).value();
  FSL_REQUIRE_OK(fsl_test::open_epoch(ledger, 1, 1, 1));
  FSL_REQUIRE_OK(fsl_test::register_subject(ledger, subject_ref(fsl::SubjectKind::kPowerDomain, "feed-a"),
                                            std::nullopt, 2));

  fsl_test::SubmissionBuilder first(fsl::EventKind::kObservationAccepted);
  first.event_id(1, 3).subject(fsl::SubjectKind::kPowerDomain, "feed-a").source("source-one").payload(
      fsl::payload::encode(fsl::payload::ObservationAccepted{"a", {1}}));
  auto one = first.build();
  FSL_REQUIRE_OK(one);
  FSL_REQUIRE_OK(ledger.append(one.value()));

  fsl_test::SubmissionBuilder second(fsl::EventKind::kObservationAccepted);
  second.event_id(1, 4).subject(fsl::SubjectKind::kPowerDomain, "feed-a").source("source-two").payload(
      fsl::payload::encode(fsl::payload::ObservationAccepted{"b", {2}}));
  auto two = second.build();
  FSL_REQUIRE_OK(two);
  FSL_REQUIRE_ERROR(ledger.append(two.value()), fsl::ErrorCode::kCapacityLimitExceeded);
}

}  // namespace

FSL_TEST_MAIN("test_ledger_admission")

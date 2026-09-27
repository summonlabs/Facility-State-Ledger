// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include "detail/codec.hpp"
#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_codec.cpp
/// Canonical encoding of submissions and committed envelopes, and the
/// deterministic rejection of anything that does not decode exactly.

namespace {

using fsl::detail::decode_envelope;
using fsl::detail::encode_envelope;
using fsl::detail::encode_submission;

[[nodiscard]] fsl::EventEnvelope make_envelope(const fsl::SubmittedObservation& observation,
                                               std::uint64_t sequence = 1,
                                               std::uint64_t tick = 1) {
  return fsl::EventEnvelope(fsl::LedgerSequence(sequence),
                            observation.event_id(),
                            observation.idempotency_token(),
                            observation.facility_generation(),
                            observation.epoch(),
                            observation.kind(),
                            observation.subject(),
                            observation.correction_target(),
                            observation.payload_schema(),
                            observation.payload_schema_version(),
                            observation.payload(),
                            observation.provenance(),
                            fsl::AcceptedAt{fsl::LogicalTick(tick), 12345},
                            observation.content_digest(),
                            fsl::Digest{});
}

FSL_TEST(submission_encoding_is_deterministic) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(1, 2)
      .subject(fsl::SubjectKind::kRack, "site-a.rack-1")
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-1", {1, 2, 3}}));
  auto first = builder.build();
  FSL_REQUIRE_OK(first);
  auto second = builder.build();
  FSL_REQUIRE_OK(second);
  FSL_CHECK(encode_submission(first.value()) == encode_submission(second.value()));
  FSL_CHECK(first->content_digest() == second->content_digest());
}

FSL_TEST(content_digest_ignores_the_retry_token) {
  // Two submissions that differ only in their idempotency token are the same
  // content, so a retry that carries a fresh token is still recognised.
  fsl_test::SubmissionBuilder first(fsl::EventKind::kObservationAccepted);
  first.event_id(9, 9).subject(fsl::SubjectKind::kRack, "r").token(1, 1);
  auto without_token = first.build();
  FSL_REQUIRE_OK(without_token);

  fsl_test::SubmissionBuilder second(fsl::EventKind::kObservationAccepted);
  second.event_id(9, 9).subject(fsl::SubjectKind::kRack, "r");
  auto plain = second.build();
  FSL_REQUIRE_OK(plain);

  FSL_CHECK(without_token->content_digest() == plain->content_digest());
  FSL_CHECK(encode_submission(without_token.value()) == encode_submission(plain.value()));
}

FSL_TEST(content_digest_changes_with_content) {
  fsl_test::SubmissionBuilder first(fsl::EventKind::kObservationAccepted);
  first.event_id(1, 1).subject(fsl::SubjectKind::kRack, "r");
  auto a = first.build();
  FSL_REQUIRE_OK(a);
  fsl_test::SubmissionBuilder second(fsl::EventKind::kObservationAccepted);
  second.event_id(1, 1).subject(fsl::SubjectKind::kRack, "s");
  auto b = second.build();
  FSL_REQUIRE_OK(b);
  FSL_CHECK(a->content_digest() != b->content_digest());
}

FSL_TEST(envelope_round_trips_exactly) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(0x1234, 0x5678)
      .subject(fsl::SubjectKind::kAsset, "site-a.rack-1.asset-7")
      .token(0xAAAA, 0xBBBB)
      .payload(fsl::payload::encode(fsl::payload::ObservationAccepted{"obs-42", {9, 8, 7, 6}}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);

  const fsl::EventEnvelope original = make_envelope(observation.value(), 17, 23);
  const std::vector<std::uint8_t> bytes = encode_envelope(original);
  auto decoded = decode_envelope(bytes);
  FSL_REQUIRE_OK(decoded);

  FSL_CHECK(decoded->sequence() == original.sequence());
  FSL_CHECK(decoded->event_id() == original.event_id());
  FSL_CHECK(decoded->idempotency_token() == original.idempotency_token());
  FSL_CHECK(decoded->facility_generation() == original.facility_generation());
  FSL_CHECK(decoded->epoch() == original.epoch());
  FSL_CHECK(decoded->kind() == original.kind());
  FSL_CHECK(decoded->subject() == original.subject());
  FSL_CHECK(decoded->payload_schema() == original.payload_schema());
  FSL_CHECK(decoded->payload_schema_version() == original.payload_schema_version());
  FSL_CHECK(decoded->payload() == original.payload());
  FSL_CHECK(decoded->provenance().source() == original.provenance().source());
  FSL_CHECK(decoded->provenance().source_generation() == original.provenance().source_generation());
  FSL_CHECK(decoded->accepted_at().logical_tick == original.accepted_at().logical_tick);
  FSL_CHECK_EQ(decoded->accepted_at().monotonic_nanoseconds,
               original.accepted_at().monotonic_nanoseconds);
  FSL_CHECK(decoded->content_digest() == original.content_digest());
  FSL_CHECK(decoded->integrity() == fsl::Sha256::hash(bytes.data(), bytes.size()));
}

FSL_TEST(envelope_rejects_a_flipped_content_byte) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(3, 4).subject(fsl::SubjectKind::kRack, "r");
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  std::vector<std::uint8_t> bytes = encode_envelope(make_envelope(observation.value()));
  // Flip a byte well past the fixed header so the payload is what changes.
  bytes[bytes.size() - 8] ^= 0x01;
  auto decoded = decode_envelope(bytes);
  FSL_REQUIRE_ERROR(decoded, fsl::ErrorCode::kDigestMismatch);
}

FSL_TEST(envelope_rejects_trailing_bytes) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(3, 4).subject(fsl::SubjectKind::kRack, "r");
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  std::vector<std::uint8_t> bytes = encode_envelope(make_envelope(observation.value()));
  bytes.push_back(0x00);
  FSL_REQUIRE_ERROR(decode_envelope(bytes), fsl::ErrorCode::kUnexpectedTrailingBytes);
}

FSL_TEST(envelope_rejects_a_truncated_body) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(3, 4).subject(fsl::SubjectKind::kRack, "r");
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  const std::vector<std::uint8_t> bytes = encode_envelope(make_envelope(observation.value()));
  for (std::size_t cut = 1; cut < bytes.size() && cut < 40; ++cut) {
    const std::span<const std::uint8_t> view(bytes.data(), cut);
    auto decoded = decode_envelope(view);
    FSL_CHECK(!decoded.has_value());
  }
}

FSL_TEST(envelope_rejects_an_unsupported_version) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(3, 4).subject(fsl::SubjectKind::kRack, "r");
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  std::vector<std::uint8_t> bytes = encode_envelope(make_envelope(observation.value()));
  bytes[0] = 0x7F;  // envelope version
  FSL_REQUIRE_ERROR(decode_envelope(bytes), fsl::ErrorCode::kUnsupportedFormatVersion);
}

FSL_TEST(envelope_rejects_a_zero_sequence_or_tick) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(3, 4).subject(fsl::SubjectKind::kRack, "r");
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);
  std::vector<std::uint8_t> bytes = encode_envelope(make_envelope(observation.value()));
  for (std::size_t i = 4; i < 12; ++i) {
    bytes[i] = 0;  // zero the committed sequence
  }
  FSL_REQUIRE_ERROR(decode_envelope(bytes), fsl::ErrorCode::kHeaderFieldInvalid);
}

FSL_TEST(provenance_attributes_are_canonical_and_bounded) {
  auto source = fsl::SourceComponentId::parse("test.harness").value();
  std::vector<fsl::ProvenanceAttribute> attributes{{"zulu", "last"}, {"alpha", "first"}};
  auto record = fsl::ProvenanceRecord::create(std::move(source), fsl::SourceGeneration(2),
                                              fsl::SourceSequence(11), 1234, std::move(attributes));
  FSL_REQUIRE_OK(record);
  FSL_REQUIRE(record->attributes().size() == 2);
  FSL_CHECK_EQ(record->attributes()[0].key, std::string("alpha"));
  FSL_CHECK_EQ(record->attributes()[1].key, std::string("zulu"));
  FSL_CHECK(record->source_sequence().has_value());
  FSL_CHECK_EQ(record->source_sequence()->value(), std::uint64_t{11});
  FSL_CHECK(record->source_wall_clock_unix_nanos().has_value());

  const std::string* found = record->find_attribute("alpha");
  FSL_REQUIRE(found != nullptr);
  FSL_CHECK_EQ(*found, std::string("first"));
  FSL_CHECK(record->find_attribute("missing") == nullptr);
}

FSL_TEST(provenance_rejects_duplicate_and_malformed_attributes) {
  auto source = fsl::SourceComponentId::parse("test.harness").value();
  std::vector<fsl::ProvenanceAttribute> duplicates{{"k", "1"}, {"k", "2"}};
  FSL_REQUIRE_ERROR(
      fsl::ProvenanceRecord::create(source, fsl::SourceGeneration(1), std::nullopt, std::nullopt,
                                    std::move(duplicates)),
      fsl::ErrorCode::kDuplicateKey);

  std::vector<fsl::ProvenanceAttribute> bad_key{{"UPPER", "v"}};
  FSL_REQUIRE_ERROR(
      fsl::ProvenanceRecord::create(source, fsl::SourceGeneration(1), std::nullopt, std::nullopt,
                                    std::move(bad_key)),
      fsl::ErrorCode::kSyntaxInvalid);

  std::vector<fsl::ProvenanceAttribute> bad_value{{"k", std::string("\xFF\xFE", 2)}};
  FSL_REQUIRE_ERROR(
      fsl::ProvenanceRecord::create(source, fsl::SourceGeneration(1), std::nullopt, std::nullopt,
                                    std::move(bad_value)),
      fsl::ErrorCode::kInvalidUtf8);

  std::vector<fsl::ProvenanceAttribute> too_many;
  for (std::size_t i = 0; i <= fsl::ProvenanceRecord::kMaxAttributes; ++i) {
    too_many.push_back(fsl::ProvenanceAttribute{"k" + std::to_string(i), "v"});
  }
  FSL_REQUIRE_ERROR(
      fsl::ProvenanceRecord::create(source, fsl::SourceGeneration(1), std::nullopt, std::nullopt,
                                    std::move(too_many)),
      fsl::ErrorCode::kCountOutOfRange);
}

FSL_TEST(provenance_rejects_a_zero_incarnation) {
  auto source = fsl::SourceComponentId::parse("test.harness").value();
  FSL_REQUIRE_ERROR(fsl::ProvenanceRecord::create(source, fsl::SourceGeneration(0), std::nullopt,
                                                  std::nullopt, {}),
                    fsl::ErrorCode::kInvalidEnumValue);
}

FSL_TEST(submission_validation_reports_missing_fields) {
  fsl::SubmittedObservationFields empty;
  FSL_REQUIRE_ERROR(fsl::SubmittedObservation::create(empty), fsl::ErrorCode::kMissingRequiredAttribute);

  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  auto fields = builder.fields();
  fields.event_id.reset();
  FSL_REQUIRE_ERROR(fsl::SubmittedObservation::create(std::move(fields)),
                    fsl::ErrorCode::kMissingRequiredAttribute);

  auto fields2 = builder.fields();
  fields2.subject.reset();
  FSL_REQUIRE_ERROR(fsl::SubmittedObservation::create(std::move(fields2)),
                    fsl::ErrorCode::kMissingRequiredAttribute);

  auto fields3 = builder.fields();
  fields3.provenance.reset();
  FSL_REQUIRE_ERROR(fsl::SubmittedObservation::create(std::move(fields3)),
                    fsl::ErrorCode::kMissingRequiredAttribute);
}

FSL_TEST(submission_rejects_nil_identities_and_self_correction) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  auto fields = builder.fields();
  fields.event_id = fsl::EventId{};
  FSL_REQUIRE_ERROR(fsl::SubmittedObservation::create(std::move(fields)),
                    fsl::ErrorCode::kHeaderFieldInvalid);

  fsl_test::SubmissionBuilder correcting(fsl::EventKind::kCorrectionRecorded);
  correcting.event_id(5, 5).correction_target(5, 5).payload(
      fsl::payload::encode(fsl::payload::CorrectionRecorded{"because"}));
  FSL_REQUIRE_ERROR(correcting.build(), fsl::ErrorCode::kSelfReference);
}

FSL_TEST(a_correction_target_is_only_valid_on_a_correction) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  builder.event_id(1, 1).correction_target(2, 2);
  FSL_REQUIRE_ERROR(builder.build(), fsl::ErrorCode::kCorrectionTargetUnexpected);
}

FSL_TEST(payload_schema_version_is_required_and_non_zero) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kObservationAccepted);
  auto fields = builder.fields();
  fields.payload_schema_version.reset();
  FSL_REQUIRE_ERROR(fsl::SubmittedObservation::create(std::move(fields)),
                    fsl::ErrorCode::kMissingRequiredAttribute);

  auto fields2 = builder.fields();
  fields2.payload_schema_version = fsl::SchemaVersion(0);
  FSL_REQUIRE_ERROR(fsl::SubmittedObservation::create(std::move(fields2)),
                    fsl::ErrorCode::kInvalidEnumValue);
}

FSL_TEST(payload_validation_rejects_schema_and_content_mismatches) {
  fsl_test::SubmissionBuilder builder(fsl::EventKind::kEpochOpened);
  builder.event_id(1, 1).payload(fsl::payload::encode(fsl::payload::EpochOpened{
      fsl::FacilityEpoch(1), fsl::FacilityGeneration(1)}));
  auto observation = builder.build();
  FSL_REQUIRE_OK(observation);

  FSL_CHECK(fsl::payload::validate(observation->kind(), observation->payload_schema(),
                                   observation->payload_schema_version(), observation->payload())
                .is_ok());

  // Wrong schema identifier for the kind.
  auto wrong_schema = fsl::SchemaId::parse("fsl.subject-retired.v1").value();
  FSL_CHECK_EQ(fsl::payload::validate(observation->kind(), wrong_schema,
                                      observation->payload_schema_version(), observation->payload())
                   .code(),
               fsl::ErrorCode::kUnsupportedPayloadSchema);

  // A payload that is not the schema it claims.
  const std::vector<std::uint8_t> garbage{0xFF, 0xFF};
  FSL_CHECK(fsl::payload::validate(observation->kind(), observation->payload_schema(),
                                   observation->payload_schema_version(), garbage)
                .is_error());
}

FSL_TEST(every_payload_schema_round_trips) {
  const fsl::payload::EpochOpened epoch_opened{fsl::FacilityEpoch(3), fsl::FacilityGeneration(2)};
  auto decoded_epoch =
      fsl::payload::decode_epoch_opened(fsl::payload::encode(epoch_opened), fsl::SchemaVersion(1));
  FSL_REQUIRE_OK(decoded_epoch);
  FSL_CHECK(decoded_epoch->epoch == epoch_opened.epoch);
  FSL_CHECK(decoded_epoch->generation == epoch_opened.generation);

  const fsl::payload::SubjectRegistered registered{
      fsl_test::subject_ref(fsl::SubjectKind::kRack, "site-a.rack-1"),
      fsl_test::subject_ref(fsl::SubjectKind::kLocation, "site-a")};
  auto decoded_subject =
      fsl::payload::decode_subject_registered(fsl::payload::encode(registered), fsl::SchemaVersion(1));
  FSL_REQUIRE_OK(decoded_subject);
  FSL_CHECK(decoded_subject->subject == registered.subject);
  FSL_REQUIRE(decoded_subject->location.has_value());
  FSL_CHECK(*decoded_subject->location == *registered.location);

  const fsl::payload::ObservationAccepted observation{"obs-1", {1, 2, 3, 4}};
  auto decoded_observation = fsl::payload::decode_observation_accepted(
      fsl::payload::encode(observation), fsl::SchemaVersion(1));
  FSL_REQUIRE_OK(decoded_observation);
  FSL_CHECK_EQ(decoded_observation->observation_ref, observation.observation_ref);
  FSL_CHECK(decoded_observation->body == observation.body);

  const fsl::payload::Relationship relationship{
      fsl_test::subject_ref(fsl::SubjectKind::kPowerDomain, "feed-a"),
      fsl::RelationshipKind::kPoweredBy};
  auto decoded_relationship =
      fsl::payload::decode_relationship(fsl::payload::encode(relationship), fsl::SchemaVersion(1));
  FSL_REQUIRE_OK(decoded_relationship);
  FSL_CHECK(decoded_relationship->related == relationship.related);
  FSL_CHECK(decoded_relationship->kind == relationship.kind);

  const fsl::payload::CorrectionRecorded correction{"the earlier record named the wrong rack"};
  auto decoded_correction = fsl::payload::decode_correction_recorded(
      fsl::payload::encode(correction), fsl::SchemaVersion(1));
  FSL_REQUIRE_OK(decoded_correction);
  FSL_CHECK_EQ(decoded_correction->rationale, correction.rationale);
}

FSL_TEST(structured_payload_decoders_reject_versions_trailing_bytes_and_truncation) {
  const std::vector<std::uint8_t> bytes =
      fsl::payload::encode(fsl::payload::EpochOpened{fsl::FacilityEpoch(1), fsl::FacilityGeneration(1)});

  std::vector<std::uint8_t> extended = bytes;
  extended.push_back(0x00);
  FSL_REQUIRE_ERROR(fsl::payload::decode_epoch_opened(extended, fsl::SchemaVersion(1)),
                    fsl::ErrorCode::kUnexpectedTrailingBytes);

  FSL_REQUIRE_ERROR(fsl::payload::decode_epoch_opened(bytes, fsl::SchemaVersion(9)),
                    fsl::ErrorCode::kUnsupportedPayloadSchema);

  std::vector<std::uint8_t> truncated = bytes;
  truncated.pop_back();
  FSL_REQUIRE_ERROR(fsl::payload::decode_epoch_opened(truncated, fsl::SchemaVersion(1)),
                    fsl::ErrorCode::kTruncatedInput);
}

FSL_TEST(payload_decoders_reject_zero_valued_and_out_of_step_fields) {
  const std::vector<std::uint8_t> zero_epoch(16, 0);
  FSL_REQUIRE_ERROR(fsl::payload::decode_epoch_opened(zero_epoch, fsl::SchemaVersion(1)),
                    fsl::ErrorCode::kHeaderFieldInvalid);

  const std::vector<std::uint8_t> bad_advance = fsl::payload::encode(
      fsl::payload::GenerationAdvanced{fsl::FacilityGeneration(2), fsl::FacilityGeneration(4)});
  FSL_REQUIRE_ERROR(fsl::payload::decode_generation_advanced(bad_advance, fsl::SchemaVersion(1)),
                    fsl::ErrorCode::kHeaderFieldInvalid);
}

FSL_TEST(relationship_payload_rejects_unknown_kinds) {
  std::vector<std::uint8_t> bytes =
      fsl::payload::encode(fsl::payload::Relationship{fsl_test::subject_ref(fsl::SubjectKind::kRack, "r"),
                                                      fsl::RelationshipKind::kContainedIn});
  bytes.back() = 0x7F;
  const auto decoded = fsl::payload::decode_relationship(bytes, fsl::SchemaVersion(1));
  FSL_REQUIRE_ERROR(decoded, fsl::ErrorCode::kInvalidEnumValue);
}

}  // namespace

FSL_TEST_MAIN("test_codec")

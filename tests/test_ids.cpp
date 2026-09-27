// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <limits>
#include <string>
#include <type_traits>

#include "fsl/counter.hpp"
#include "fsl/ids.hpp"
#include "fsl/subject.hpp"
#include "support/test_support.hpp"

/// \file test_ids.cpp
/// Identity and counter semantics, including the compile-time guarantee that
/// unrelated identities and counters never convert into one another.

namespace {

FSL_TEST(identity_hex_round_trip) {
  const fsl::EventId id = fsl::EventId::from_words(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL);
  FSL_CHECK_EQ(id.to_hex(), std::string("0123456789abcdeffedcba9876543210"));
  auto parsed = fsl::EventId::from_hex(id.to_hex());
  FSL_REQUIRE(parsed.has_value());
  FSL_CHECK(parsed.value() == id);
  auto upper = fsl::EventId::from_hex("0123456789ABCDEFFEDCBA9876543210");
  FSL_REQUIRE(upper.has_value());
  FSL_CHECK(upper.value() == id);
}

FSL_TEST(identity_rejects_malformed_text) {
  FSL_CHECK(!fsl::EventId::from_hex("").has_value());
  FSL_CHECK(!fsl::EventId::from_hex("00").has_value());
  FSL_CHECK(!fsl::EventId::from_hex(std::string(31, 'a')).has_value());
  FSL_CHECK(!fsl::EventId::from_hex(std::string(33, 'a')).has_value());
  FSL_CHECK(!fsl::EventId::from_hex(std::string(32, 'z')).has_value());
  FSL_CHECK(!fsl::EventId::from_hex(std::string(32, ' ')).has_value());
}

FSL_TEST(nil_identity_is_recognised_and_never_confused_with_content) {
  const fsl::EventId nil;
  FSL_CHECK(nil.is_nil());
  const fsl::EventId other = fsl::EventId::from_words(0, 1);
  FSL_CHECK(!other.is_nil());
  FSL_CHECK(!(nil == other));
  // A digest very close to zero is still not the nil identity.
  const fsl::EventId low = fsl::EventId::from_words(0, 0);
  FSL_CHECK(low.is_nil());
}

FSL_TEST(identity_hashing_is_stable_within_a_run) {
  const fsl::EventId id = fsl::EventId::from_words(1, 2);
  const std::hash<fsl::EventId> hasher;
  FSL_CHECK_EQ(hasher(id), hasher(id));
  const fsl::EventId equal = fsl::EventId::from_words(1, 2);
  FSL_CHECK_EQ(hasher(id), hasher(equal));
  const fsl::EventId different = fsl::EventId::from_words(1, 3);
  FSL_CHECK(hasher(id) != hasher(different));
}

FSL_TEST(identity_types_are_distinct) {
  static_assert(!std::is_convertible_v<fsl::EventId, fsl::LedgerId>);
  static_assert(!std::is_convertible_v<fsl::LedgerId, fsl::EventId>);
  static_assert(!std::is_convertible_v<fsl::EventId, fsl::IdempotencyToken>);
  static_assert(!std::is_convertible_v<fsl::IdempotencyToken, fsl::EventId>);
  static_assert(!std::is_same_v<fsl::EventId, fsl::LedgerId>);
  FSL_CHECK(true);
}

FSL_TEST(counter_types_are_distinct_and_do_not_convert) {
  static_assert(!std::is_convertible_v<fsl::FacilityGeneration, fsl::FacilityEpoch>);
  static_assert(!std::is_convertible_v<fsl::FacilityEpoch, fsl::SourceGeneration>);
  static_assert(!std::is_convertible_v<fsl::LedgerSequence, fsl::FacilityGeneration>);
  static_assert(!std::is_convertible_v<fsl::SegmentIndex, fsl::LedgerSequence>);
  static_assert(!std::is_convertible_v<fsl::ManifestGeneration, fsl::WriterIncarnation>);
  static_assert(!std::is_default_constructible_v<fsl::LedgerSequence>);
  static_assert(!std::is_default_constructible_v<fsl::FacilityEpoch>);
  FSL_CHECK(true);
}

FSL_TEST(counter_arithmetic_is_checked) {
  const fsl::LedgerSequence first = fsl::LedgerSequence::first();
  FSL_CHECK_EQ(first.value(), std::uint64_t{1});
  FSL_CHECK(!first.is_zero());
  FSL_CHECK(first.can_advance());
  const auto second = first.try_next();
  FSL_REQUIRE(second.has_value());
  FSL_CHECK_EQ(second->value(), std::uint64_t{2});
  FSL_CHECK_EQ(second->distance_from(first), std::uint64_t{1});

  const auto maximum = fsl::LedgerSequence(std::numeric_limits<std::uint64_t>::max());
  FSL_CHECK(!maximum.can_advance());
  FSL_CHECK(!maximum.try_next().has_value());
}

FSL_TEST(source_component_identifier_validation) {
  auto good = fsl::SourceComponentId::parse("dccp.facility-topology");
  FSL_REQUIRE(good.has_value());
  FSL_CHECK_EQ(good->str(), std::string("dccp.facility-topology"));

  FSL_CHECK(!fsl::SourceComponentId::parse("").has_value());
  FSL_CHECK(!fsl::SourceComponentId::parse("Uppercase").has_value());
  FSL_CHECK(!fsl::SourceComponentId::parse("-leading").has_value());
  FSL_CHECK(!fsl::SourceComponentId::parse(std::string(97, 'a')).has_value());
  FSL_CHECK(fsl::SourceComponentId::parse(std::string(96, 'a')).has_value());
  FSL_CHECK_EQ(fsl::SourceComponentId::parse("bad id").status().code(), fsl::ErrorCode::kSyntaxInvalid);
  FSL_CHECK_EQ(fsl::SourceComponentId::parse("").status().code(), fsl::ErrorCode::kValueEmpty);
}

FSL_TEST(schema_identifier_validation) {
  FSL_CHECK(fsl::SchemaId::parse("fsl.epoch-opened.v1").has_value());
  FSL_CHECK(!fsl::SchemaId::parse("FSL.Epoch").has_value());
  FSL_CHECK(!fsl::SchemaId::parse(std::string(65, 'a')).has_value());
}

FSL_TEST(subject_reference_round_trip) {
  auto reference = fsl::SubjectRef::create(fsl::SubjectKind::kRack, "site-a/row-3/rack-1");
  FSL_REQUIRE(reference.has_value());
  FSL_CHECK_EQ(reference->to_string(), std::string("rack/site-a/row-3/rack-1"));

  auto parsed = fsl::SubjectRef::parse("rack/site-a/row-3/rack-1");
  FSL_REQUIRE(parsed.has_value());
  FSL_CHECK(parsed.value() == reference.value());
}

FSL_TEST(subject_reference_rejects_unknown_kinds_and_bad_keys) {
  FSL_CHECK(!fsl::SubjectRef::parse("nosuchkind/key").has_value());
  FSL_CHECK(!fsl::SubjectRef::parse("rack").has_value());
  FSL_CHECK(!fsl::SubjectRef::parse("/key").has_value());
  FSL_CHECK(!fsl::SubjectRef::parse("rack/..").has_value());
  FSL_CHECK_EQ(fsl::SubjectRef::parse("nosuchkind/key").status().code(), fsl::ErrorCode::kInvalidEnumValue);
  FSL_CHECK_EQ(fsl::SubjectRef::create(fsl::SubjectKind::kRack, "..").status().code(),
               fsl::ErrorCode::kSyntaxInvalid);
}

FSL_TEST(subject_reference_order_is_total_and_kind_first) {
  const auto asset = fsl::SubjectRef::create(fsl::SubjectKind::kAsset, "a").value();
  const auto rack_a = fsl::SubjectRef::create(fsl::SubjectKind::kRack, "a").value();
  const auto rack_b = fsl::SubjectRef::create(fsl::SubjectKind::kRack, "b").value();
  FSL_CHECK(rack_a < asset);
  FSL_CHECK(rack_a < rack_b);
  FSL_CHECK(!(rack_b < rack_a));
}

FSL_TEST(location_requirement_is_declared_per_kind) {
  FSL_CHECK(fsl::subject_kind_requires_location(fsl::SubjectKind::kRack));
  FSL_CHECK(fsl::subject_kind_requires_location(fsl::SubjectKind::kAsset));
  FSL_CHECK(fsl::subject_kind_requires_location(fsl::SubjectKind::kFacilityNode));
  FSL_CHECK(!fsl::subject_kind_requires_location(fsl::SubjectKind::kLocation));
  FSL_CHECK(!fsl::subject_kind_requires_location(fsl::SubjectKind::kPowerDomain));
  FSL_CHECK(!fsl::subject_kind_requires_location(fsl::SubjectKind::kExternalAsi));
  FSL_CHECK(!fsl::subject_kind_requires_location(fsl::SubjectKind::kExternalDfi));
}

FSL_TEST(subject_kind_names_round_trip) {
  const fsl::SubjectKind kinds[] = {
      fsl::SubjectKind::kRack,         fsl::SubjectKind::kAsset,
      fsl::SubjectKind::kLocation,     fsl::SubjectKind::kFacilityNode,
      fsl::SubjectKind::kPowerDomain,  fsl::SubjectKind::kCoolingDomain,
      fsl::SubjectKind::kExternalAsi,  fsl::SubjectKind::kExternalDfi,
      fsl::SubjectKind::kLedger,
  };
  for (const fsl::SubjectKind kind : kinds) {
    const auto parsed = fsl::subject_kind_from_string(fsl::to_string(kind));
    FSL_CHECK(parsed.has_value());
    FSL_CHECK(parsed.value() == kind);
  }
}

FSL_TEST(event_kind_names_round_trip) {
  const fsl::EventKind kinds[] = {
      fsl::EventKind::kLedgerOpened,         fsl::EventKind::kEpochOpened,
      fsl::EventKind::kEpochClosed,          fsl::EventKind::kGenerationAdvanced,
      fsl::EventKind::kSubjectRegistered,    fsl::EventKind::kObservationAccepted,
      fsl::EventKind::kSubjectMutated,       fsl::EventKind::kSubjectRetired,
      fsl::EventKind::kRelationshipAsserted, fsl::EventKind::kRelationshipRetracted,
      fsl::EventKind::kReconciliationRecorded, fsl::EventKind::kCorrectionRecorded,
      fsl::EventKind::kPolicyAttested,
  };
  for (const fsl::EventKind kind : kinds) {
    const auto parsed = fsl::event_kind_from_string(fsl::to_string(kind));
    FSL_CHECK(parsed.has_value());
    FSL_CHECK(parsed.value() == kind);
  }
  FSL_CHECK(!fsl::event_kind_from_string("no-such-kind").has_value());
}

}  // namespace

FSL_TEST_MAIN("test_ids")

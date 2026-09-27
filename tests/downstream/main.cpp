// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <fsl/ledger.hpp>
#include <fsl/payload.hpp>

/// \file main.cpp
/// An independent consumer of the installed FacilityStateLedger package.
///
/// It links the exported target, opens a ledger it creates, commits an epoch
/// and a subject, reads the record back, verifies integrity, exports one
/// canonical audit record and reopens the ledger from a fresh handle. It fails
/// loudly if any step does not behave as the installed headers promise.

namespace {

int fail(const char* step, const fsl::Status& status) {
  std::fprintf(stderr, "downstream %s failed: %s\n", step, status.to_string().c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path directory =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() / "fsl_downstream_consumer";
  std::error_code error;
  std::filesystem::remove_all(directory, error);

  auto created = fsl::Ledger::create(directory);
  if (!created.has_value()) {
    return fail("create", created.status());
  }
  fsl::Ledger ledger = std::move(created).value();

  const fsl::SubjectRef journal =
      std::move(fsl::SubjectRef::create(fsl::SubjectKind::kLedger, "facility-state-ledger").value());

  const auto submit = [&ledger, &journal](std::uint64_t index, fsl::EventKind kind,
                                          std::vector<std::uint8_t> payload,
                                          fsl::FacilityEpoch epoch) {
    fsl::SubmittedObservationFields fields;
    fields.kind = kind;
    fields.event_id = fsl::EventId::from_words(0xD0A5ULL, index);
    fields.facility_generation = fsl::FacilityGeneration(1);
    fields.epoch = epoch;
    fields.subject = journal;
    fields.payload_schema = fsl::payload::required_schema(kind);
    fields.payload_schema_version = fsl::payload::required_schema_version(kind);
    fields.payload = std::move(payload);
    auto source = fsl::SourceComponentId::parse("downstream.consumer").value();
    auto provenance = fsl::ProvenanceRecord::create(std::move(source), fsl::SourceGeneration::first(),
                                                    std::nullopt, std::nullopt, {});
    fields.provenance = std::move(provenance).value();
    auto observation = fsl::SubmittedObservation::create(std::move(fields));
    if (!observation.has_value()) {
      return fsl::Result<fsl::AppendOutcome>(observation.status());
    }
    return ledger.append(observation.value());
  };

  auto epoch = submit(1, fsl::EventKind::kEpochOpened,
                      fsl::payload::encode(fsl::payload::EpochOpened{fsl::FacilityEpoch(1),
                                                                     fsl::FacilityGeneration(1)}),
                      fsl::FacilityEpoch(1));
  if (!epoch.has_value()) {
    return fail("epoch-opened", epoch.status());
  }

  fsl::SubmittedObservationFields fields;
  fields.kind = fsl::EventKind::kSubjectRegistered;
  fields.event_id = fsl::EventId::from_words(0xD0A5ULL, 3);
  fields.facility_generation = fsl::FacilityGeneration(1);
  fields.epoch = fsl::FacilityEpoch(1);
  fields.subject = fsl::SubjectRef::create(fsl::SubjectKind::kLocation, "downstream/site").value();
  fields.payload_schema = fsl::payload::required_schema(fsl::EventKind::kSubjectRegistered);
  fields.payload_schema_version = fsl::payload::required_schema_version(fsl::EventKind::kSubjectRegistered);
  fields.payload = fsl::payload::encode(
      fsl::payload::SubjectRegistered{*fields.subject, std::nullopt});
  auto source = fsl::SourceComponentId::parse("downstream.consumer").value();
  fields.provenance = fsl::ProvenanceRecord::create(std::move(source), fsl::SourceGeneration::first(),
                                                    std::nullopt, std::nullopt, {})
                          .value();
  auto registration = fsl::SubmittedObservation::create(std::move(fields));
  if (!registration.has_value()) {
    return fail("subject-registered", registration.status());
  }
  auto registered = ledger.append(registration.value());
  if (!registered.has_value()) {
    return fail("subject-registered", registered.status());
  }

  auto read_back = ledger.read(registered->event.sequence());
  if (!read_back.has_value()) {
    return fail("read", read_back.status());
  }
  if (read_back->kind() != fsl::EventKind::kSubjectRegistered) {
    std::fprintf(stderr, "downstream read returned the wrong kind\n");
    return 1;
  }

  const fsl::VerifyReport report = ledger.verify();
  if (!report.ok()) {
    return fail("verify", report.status);
  }

  auto audit = ledger.export_audit_json();
  if (!audit.has_value()) {
    return fail("export", audit.status());
  }
  if (audit->find("\"format\":\"fsl.audit.v1\"") == std::string::npos) {
    std::fprintf(stderr, "downstream export did not produce canonical audit records\n");
    return 1;
  }

  const auto watermark = ledger.watermark();
  if (!watermark.has_value()) {
    return fail("watermark", watermark.status());
  }
  const std::uint64_t committed = watermark->sequence->value();

  const auto closed = ledger.close();
  if (!closed.has_value()) {
    return fail("close", closed);
  }

  auto reopened = fsl::Ledger::open(directory, fsl::OpenMode::kReadOnly);
  if (!reopened.has_value()) {
    return fail("reopen", reopened.status());
  }
  fsl::Ledger reader = std::move(reopened).value();
  const auto reread = reader.watermark();
  if (!reread.has_value()) {
    return fail("watermark", reread.status());
  }
  if (reread->sequence->value() != committed) {
    std::fprintf(stderr, "downstream reopen lost committed records\n");
    return 1;
  }
  const auto closed_reader = reader.close();
  if (!closed_reader.has_value()) {
    return fail("close", closed_reader);
  }

  std::printf("downstream consumer ok: ledger %s, committed %llu, verified %llu records\n",
              directory.string().c_str(), static_cast<unsigned long long>(committed),
              static_cast<unsigned long long>(report.records_verified));

  std::filesystem::remove_all(directory, error);
  return 0;
}

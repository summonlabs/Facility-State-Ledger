// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "fsl/fault.hpp"
#include "fsl/ledger.hpp"
#include "fsl/payload.hpp"

/// \file crash_writer.cpp
/// A writer that terminates abruptly at a named durability boundary.
///
/// This program exists so that durability can be proven rather than asserted:
/// a supervisor starts it, it appends deterministically identified events, and
/// the injected failure terminates the process at a chosen point on the commit
/// path. The supervisor then reopens the ledger from a fresh process and checks
/// that exactly the acknowledged prefix survived.
///
/// Usage:
///   fsl_example_crash_writer --dir D --events N [--batch M] [--boundary NAME]
///
/// Exits 0 and prints "committed <n>" when every event was committed. Exits 97
/// when the boundary terminated the process.

namespace {

constexpr int kExitInjected = 97;

/// Terminates the process at a named durability boundary.
///
/// The injector starts disarmed so that opening the ledger and preparing the
/// subject pool are never the thing that dies; the run arms it immediately
/// before the final append, which is the operation whose durability is under
/// test. That makes the crash point deterministic regardless of how many
/// preparatory commits the run needs.
class TerminatingInjector final : public fsl::IFaultInjector {
 public:
  explicit TerminatingInjector(fsl::Boundary boundary) : boundary_(boundary) {}

  void arm() noexcept { armed_ = true; }

  void on_boundary(fsl::Boundary boundary) override {
    if (armed_ && boundary == boundary_) {
      // Terminate immediately: no destructors, no flush, no clean shutdown.
      std::_Exit(kExitInjected);
    }
  }

 private:
  fsl::Boundary boundary_;
  bool armed_ = false;
};

struct Arguments {
  std::filesystem::path directory;
  std::uint64_t events = 1;
  std::uint64_t batch = 1;
  std::uint64_t identity_base = 100;
  std::optional<fsl::Boundary> boundary;
};

[[nodiscard]] bool parse_number(const char* text, std::uint64_t& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  std::uint64_t value = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
    value = value * 10 + static_cast<std::uint64_t>(*cursor - '0');
  }
  out = value;
  return true;
}

[[nodiscard]] bool parse(int argc, char** argv, Arguments& out) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&]() -> const char* {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", argument.c_str());
        return nullptr;
      }
      return argv[++index];
    };
    if (argument == "--dir") {
      const char* value = next();
      if (value == nullptr) {
        return false;
      }
      out.directory = value;
    } else if (argument == "--events") {
      const char* value = next();
      if (value == nullptr || !parse_number(value, out.events) || out.events == 0) {
        std::fprintf(stderr, "--events requires a positive integer\n");
        return false;
      }
    } else if (argument == "--batch") {
      const char* value = next();
      if (value == nullptr || !parse_number(value, out.batch) || out.batch == 0) {
        std::fprintf(stderr, "--batch requires a positive integer\n");
        return false;
      }
    } else if (argument == "--identity-base") {
      const char* value = next();
      if (value == nullptr || !parse_number(value, out.identity_base)) {
        std::fprintf(stderr, "--identity-base requires a non-negative integer\n");
        return false;
      }
    } else if (argument == "--boundary") {
      const char* value = next();
      if (value == nullptr) {
        return false;
      }
      const std::string name = value;
      if (name == "none") {
        out.boundary.reset();
        continue;
      }
      const fsl::Boundary boundaries[] = {
          fsl::Boundary::kAfterEncode,         fsl::Boundary::kAfterSegmentWrite,
          fsl::Boundary::kAfterSegmentFlush,   fsl::Boundary::kBeforeManifestPublish,
          fsl::Boundary::kAfterManifestPublish, fsl::Boundary::kAfterCommitAck,
          fsl::Boundary::kAfterRotateSealWrite, fsl::Boundary::kAfterRecoveryScan,
      };
      bool matched = false;
      for (const fsl::Boundary boundary : boundaries) {
        if (fsl::to_string(boundary) == name) {
          out.boundary = boundary;
          matched = true;
          break;
        }
      }
      if (!matched) {
        std::fprintf(stderr, "unknown boundary '%s'\n", name.c_str());
        return false;
      }
    } else {
      std::fprintf(stderr, "unknown option '%s'\n", argument.c_str());
      return false;
    }
  }
  if (out.directory.empty()) {
    std::fprintf(stderr, "--dir is required\n");
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Arguments arguments;
  if (!parse(argc, argv, arguments)) {
    return 1;
  }

  fsl::LedgerOptions options;
  options.segment_timestamp_unix_nanos = 1767225600000000000ULL;
  std::optional<TerminatingInjector> injector;
  if (arguments.boundary.has_value()) {
    injector.emplace(*arguments.boundary);
    options.fault_injector = &injector.value();
  }

  std::error_code error;
  const bool exists = std::filesystem::exists(arguments.directory / "ledger.manifest", error) && !error;
  auto opened = exists ? fsl::Ledger::open(arguments.directory, fsl::OpenMode::kReadWrite, options)
                       : fsl::Ledger::create(arguments.directory, options);
  if (!opened.has_value()) {
    std::fprintf(stderr, "open failed: %s\n", opened.status().to_string().c_str());
    return 1;
  }
  fsl::Ledger ledger = std::move(opened).value();

  const auto state = ledger.state();
  if (!state.has_value()) {
    std::fprintf(stderr, "state failed: %s\n", state.status().to_string().c_str());
    return 1;
  }

  const fsl::SubjectRef journal = fsl::SubjectRef::create(fsl::SubjectKind::kLedger, "facility-state-ledger").value();
  const fsl::SubjectRef location =
      fsl::SubjectRef::create(fsl::SubjectKind::kLocation, "crash/hall-1").value();
  const fsl::SubjectRef rack = fsl::SubjectRef::create(fsl::SubjectKind::kRack, "crash/rack-1").value();

  const auto append_one = [&ledger](std::uint64_t index, const fsl::SubjectRef& target,
                                    fsl::EventKind kind, std::vector<std::uint8_t> payload) {
    auto source = fsl::SourceComponentId::parse("example.crash-writer").value();
    fsl::SubmittedObservationFields fields;
    fields.kind = kind;
    fields.event_id = fsl::EventId::from_words(0xC2A5'0000ULL, index);
    fields.facility_generation = fsl::FacilityGeneration(1);
    fields.epoch = fsl::FacilityEpoch(1);
    fields.subject = target;
    fields.payload_schema = fsl::payload::required_schema(kind);
    fields.payload_schema_version = fsl::payload::required_schema_version(kind);
    fields.payload = std::move(payload);
    auto provenance = fsl::ProvenanceRecord::create(std::move(source), fsl::SourceGeneration::first(),
                                                    std::nullopt, std::nullopt, {});
    fields.provenance = std::move(provenance).value();
    auto observation = fsl::SubmittedObservation::create(std::move(fields));
    if (!observation.has_value()) {
      return fsl::Result<fsl::AppendOutcome>(observation.status());
    }
    return ledger.append(observation.value());
  };

  // Prepare whatever the ledger still needs, so that the run works against both
  // a fresh directory and one a previous run already prepared. Every step is
  // skipped when the ledger already satisfies it, which is what makes repeated
  // runs against the same directory idempotent.
  const auto is_live = [&ledger](const fsl::SubjectRef& reference) {
    const auto view = ledger.subject(reference);
    return view.has_value() && view->registered && !view->retired;
  };

  if (!state->open_epoch.has_value()) {
    auto epoch = append_one(1, journal, fsl::EventKind::kEpochOpened,
                            fsl::payload::encode(fsl::payload::EpochOpened{fsl::FacilityEpoch(1),
                                                                           fsl::FacilityGeneration(1)}));
    if (!epoch.has_value()) {
      std::fprintf(stderr, "epoch failed: %s\n", epoch.status().to_string().c_str());
      return 1;
    }
  }
  if (!is_live(location)) {
    auto registered_location =
        append_one(2, location, fsl::EventKind::kSubjectRegistered,
                   fsl::payload::encode(fsl::payload::SubjectRegistered{location, std::nullopt}));
    if (!registered_location.has_value()) {
      std::fprintf(stderr, "location registration failed: %s\n",
                   registered_location.status().to_string().c_str());
      return 1;
    }
  }
  if (!is_live(rack)) {
    auto registered = append_one(3, rack, fsl::EventKind::kSubjectRegistered,
                                 fsl::payload::encode(fsl::payload::SubjectRegistered{rack, location}));
    if (!registered.has_value()) {
      std::fprintf(stderr, "registration failed: %s\n", registered.status().to_string().c_str());
      return 1;
    }
  }

  std::uint64_t committed = 0;
  std::uint64_t index = arguments.identity_base;
  while (committed < arguments.events) {
    const std::uint64_t size = std::min(arguments.batch, arguments.events - committed);
    if (injector.has_value() && committed + size >= arguments.events) {
      injector->arm();
    }
    std::vector<fsl::SubmittedObservation> batch;
    for (std::uint64_t i = 0; i < size; ++i) {
      fsl::SubmittedObservationFields fields;
      auto source = fsl::SourceComponentId::parse("example.crash-writer").value();
      fields.kind = fsl::EventKind::kObservationAccepted;
      // Data events live in their own identity space, so the preparation run and
      // the measured runs can never collide even when they share an index base.
      fields.event_id = fsl::EventId::from_words(0xC2A5'0001ULL, index + i);
      fields.facility_generation = fsl::FacilityGeneration(1);
      fields.epoch = state->open_epoch.value_or(fsl::FacilityEpoch(1));
      fields.subject = rack;
      fields.payload_schema = fsl::payload::required_schema(fsl::EventKind::kObservationAccepted);
      fields.payload_schema_version =
          fsl::payload::required_schema_version(fsl::EventKind::kObservationAccepted);
      fsl::payload::ObservationAccepted observation;
      observation.observation_ref = "crash-" + std::to_string(index + i);
      observation.body.assign(16, static_cast<std::uint8_t>(i));
      fields.payload = fsl::payload::encode(observation);
      auto provenance = fsl::ProvenanceRecord::create(std::move(source), fsl::SourceGeneration::first(),
                                                      std::nullopt, std::nullopt, {});
      fields.provenance = std::move(provenance).value();
      auto built = fsl::SubmittedObservation::create(std::move(fields));
      if (!built.has_value()) {
        std::fprintf(stderr, "submission failed: %s\n", built.status().to_string().c_str());
        return 1;
      }
      batch.push_back(std::move(built).value());
    }

    auto outcome = ledger.append_batch(batch);
    if (!outcome.has_value()) {
      std::fprintf(stderr, "append failed: %s\n", outcome.status().to_string().c_str());
      return 1;
    }
    committed += size;
    index += size;
  }

  std::printf("committed %llu\n", static_cast<unsigned long long>(committed));
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    std::fprintf(stderr, "close failed: %s\n", closed.status().to_string().c_str());
    return 1;
  }
  return 0;
}

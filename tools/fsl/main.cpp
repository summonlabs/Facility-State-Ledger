// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fsl/clock.hpp"
#include "fsl/hash.hpp"
#include "fsl/ledger.hpp"
#include "fsl/payload.hpp"
#include "fsl/text.hpp"

/// \file main.cpp
/// The `fsl` inspection CLI.
///
/// Read-only commands open the ledger with OpenMode::kReadOnly and go through
/// exactly the same public API a consumer uses: no command bypasses an
/// authority, generation or integrity check. Mutating commands are limited to
/// the narrow set needed to operate and exercise the library, and every one of
/// them is a documented public operation rather than a back door.
///
/// Exit codes: 0 success, 1 usage error, 2 operation failure.

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailure = 2;

struct Options {
  std::string command;
  std::filesystem::path directory;
  bool has_directory = false;
  std::optional<std::uint64_t> sequence;
  std::optional<std::uint64_t> sequence_from;
  std::optional<std::uint64_t> sequence_to;
  std::optional<std::uint64_t> after_sequence;
  std::optional<std::uint64_t> limit;
  std::optional<std::uint64_t> epoch;
  std::optional<std::uint64_t> generation;
  std::optional<std::uint64_t> to_generation;
  std::optional<std::uint64_t> repeat;
  std::optional<std::string> event_id;
  std::optional<std::string> token;
  std::optional<std::string> payload_hex;
  std::optional<std::string> body_hex;
  std::optional<std::string> schema;
  std::optional<std::uint32_t> schema_version;
  std::optional<std::string> kind;
  std::optional<std::string> subject;
  std::optional<std::string> subject_kind;
  std::optional<std::string> subject_key;
  std::optional<std::string> location;
  std::optional<std::string> related;
  std::optional<std::string> relationship;
  std::optional<std::string> source;
  std::optional<std::string> reference;
  std::optional<std::string> reason;
  std::optional<std::string> scope;
  std::optional<std::uint64_t> checkpoint;
  std::optional<std::uint64_t> pin_clock;
  std::optional<std::uint64_t> pin_segment_timestamp;
  bool verify_index = true;
  bool dry_run = false;
  bool rebuild_index = false;
  bool help = false;
};

[[noreturn]] void usage_error(const std::string& message) {
  std::fprintf(stderr, "fsl: %s\n", message.c_str());
  std::fprintf(stderr, "run 'fsl --help' for usage\n");
  std::exit(kExitUsage);
}

[[noreturn]] void fail(const fsl::Status& status) {
  std::fprintf(stderr, "%s\n", status.to_string().c_str());
  std::exit(kExitFailure);
}

[[nodiscard]] std::uint64_t parse_u64(const std::string& text, const std::string& option) {
  if (text.empty()) {
    usage_error("option " + option + " requires a value");
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      usage_error("option " + option + " requires a non-negative integer, got '" + text + "'");
    }
    if (value > (UINT64_MAX - static_cast<std::uint64_t>(c - '0')) / 10) {
      usage_error("option " + option + " overflows a 64-bit integer");
    }
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return value;
}

[[nodiscard]] std::string require_value(int argc, char** argv, int& index, const std::string& option) {
  if (index + 1 >= argc) {
    usage_error("option " + option + " requires a value");
  }
  ++index;
  return argv[index];
}

void print_help() {
  std::printf(
      "fsl - Facility State Ledger inspection tool\n"
      "\n"
      "usage: fsl <command> --dir <path> [options]\n"
      "\n"
      "read-only commands (never mutate the ledger)\n"
      "  inspect                          ledger identity, watermark and admission state\n"
      "  segments                         segment metadata\n"
      "  read      --sequence N | --event-id HEX\n"
      "  find      --event-id HEX | --token HEX\n"
      "  query     [--sequence-from N] [--sequence-to N] [--after N]\n"
      "            [--subject T/K] [--source S] [--kind K] [--epoch N]\n"
      "            [--generation N] --limit N\n"
      "  replay    [--from N] [--to N] [--kind K] [--limit N]\n"
      "  verify    [--scope manifest-tail|full|from-checkpoint] [--checkpoint N] [--no-index]\n"
      "  export    [--from N] [--to N] [--subject T/K] [--max-events N]\n"
      "  checkpoints\n"
      "  subjects  --limit N [--after T/K]\n"
      "  sources   --limit N\n"
      "\n"
      "mutating commands\n"
      "  init\n"
      "  append    --kind K --subject T/K --source S --generation N [options]\n"
      "  checkpoint\n"
      "  rotate\n"
      "  recover   [--dry-run] [--rebuild-index]\n"
      "\n"
      "common options\n"
      "  --dir <path>                 ledger directory\n"
      "  --pin-clock-nanos N          deterministic monotonic readings\n"
      "  --pin-segment-timestamp N    deterministic segment header timestamp\n");
}

struct Parsed {
  Options options;
};

[[nodiscard]] Parsed parse(int argc, char** argv) {
  Parsed parsed;
  if (argc < 2) {
    usage_error("no command given");
  }
  parsed.options.command = argv[1];
  if (parsed.options.command == "--help" || parsed.options.command == "-h" ||
      parsed.options.command == "help") {
    parsed.options.help = true;
    return parsed;
  }

  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      parsed.options.help = true;
      continue;
    }
    const auto value = [&](const std::string& option) { return require_value(argc, argv, index, option); };

    if (argument == "--dir") {
      parsed.options.directory = value(argument);
      parsed.options.has_directory = true;
    } else if (argument == "--sequence") {
      parsed.options.sequence = parse_u64(value(argument), argument);
    } else if (argument == "--from" || argument == "--sequence-from") {
      parsed.options.sequence_from = parse_u64(value(argument), argument);
    } else if (argument == "--to" || argument == "--sequence-to") {
      parsed.options.sequence_to = parse_u64(value(argument), argument);
    } else if (argument == "--after") {
      parsed.options.after_sequence = parse_u64(value(argument), argument);
    } else if (argument == "--limit" || argument == "--max-events") {
      parsed.options.limit = parse_u64(value(argument), argument);
    } else if (argument == "--epoch") {
      parsed.options.epoch = parse_u64(value(argument), argument);
    } else if (argument == "--generation") {
      parsed.options.generation = parse_u64(value(argument), argument);
    } else if (argument == "--to-generation") {
      parsed.options.to_generation = parse_u64(value(argument), argument);
    } else if (argument == "--repeat") {
      parsed.options.repeat = parse_u64(value(argument), argument);
    } else if (argument == "--event-id") {
      parsed.options.event_id = value(argument);
    } else if (argument == "--token") {
      parsed.options.token = value(argument);
    } else if (argument == "--payload-hex") {
      parsed.options.payload_hex = value(argument);
    } else if (argument == "--body-hex") {
      parsed.options.body_hex = value(argument);
    } else if (argument == "--schema") {
      parsed.options.schema = value(argument);
    } else if (argument == "--schema-version") {
      parsed.options.schema_version = static_cast<std::uint32_t>(parse_u64(value(argument), argument));
    } else if (argument == "--kind") {
      parsed.options.kind = value(argument);
    } else if (argument == "--subject") {
      parsed.options.subject = value(argument);
    } else if (argument == "--subject-kind") {
      parsed.options.subject_kind = value(argument);
    } else if (argument == "--subject-key") {
      parsed.options.subject_key = value(argument);
    } else if (argument == "--location") {
      parsed.options.location = value(argument);
    } else if (argument == "--related") {
      parsed.options.related = value(argument);
    } else if (argument == "--relationship") {
      parsed.options.relationship = value(argument);
    } else if (argument == "--source") {
      parsed.options.source = value(argument);
    } else if (argument == "--ref") {
      parsed.options.reference = value(argument);
    } else if (argument == "--reason") {
      parsed.options.reason = value(argument);
    } else if (argument == "--scope") {
      parsed.options.scope = value(argument);
    } else if (argument == "--checkpoint") {
      parsed.options.checkpoint = parse_u64(value(argument), argument);
    } else if (argument == "--pin-clock-nanos") {
      parsed.options.pin_clock = parse_u64(value(argument), argument);
    } else if (argument == "--pin-segment-timestamp") {
      parsed.options.pin_segment_timestamp = parse_u64(value(argument), argument);
    } else if (argument == "--no-index") {
      parsed.options.verify_index = false;
    } else if (argument == "--dry-run") {
      parsed.options.dry_run = true;
    } else if (argument == "--rebuild-index") {
      parsed.options.rebuild_index = true;
    } else {
      usage_error("unknown option '" + argument + "'");
    }
  }
  return parsed;
}

[[nodiscard]] fsl::LedgerOptions make_options(const Options& options, bool writable) {
  fsl::LedgerOptions ledger_options;
  ledger_options.segment_timestamp_unix_nanos = options.pin_segment_timestamp;
  if (writable && options.pin_clock.has_value()) {
    // A pinned clock must outlive the handle; the CLI keeps it in a static so
    // that its lifetime spans the whole process.
    static fsl::ManualClock clock;
    clock.set_unix_nanoseconds(*options.pin_segment_timestamp
                                   ? *options.pin_segment_timestamp
                                   : 1767225600000000000ULL);
    clock.advance(*options.pin_clock);
    ledger_options.clock = &clock;
  }
  return ledger_options;
}

[[nodiscard]] std::filesystem::path require_directory(const Options& options) {
  if (!options.has_directory) {
    usage_error("this command requires --dir");
  }
  return options.directory;
}

[[nodiscard]] fsl::Ledger open_read_only(const Options& options) {
  auto opened = fsl::Ledger::open(require_directory(options), fsl::OpenMode::kReadOnly,
                                  make_options(options, false));
  if (!opened.has_value()) {
    fail(opened.status());
  }
  return std::move(opened).value();
}

[[nodiscard]] fsl::Ledger open_read_write(const Options& options) {
  auto opened = fsl::Ledger::open(require_directory(options), fsl::OpenMode::kReadWrite,
                                  make_options(options, true));
  if (!opened.has_value()) {
    fail(opened.status());
  }
  return std::move(opened).value();
}

[[nodiscard]] fsl::EventId parse_event_id(const std::string& text) {
  auto parsed = fsl::EventId::from_hex(text);
  if (!parsed.has_value()) {
    usage_error("'" + text + "' is not a 32-character hexadecimal event identity");
  }
  return parsed.value();
}

[[nodiscard]] fsl::IdempotencyToken parse_token(const std::string& text) {
  auto parsed = fsl::IdempotencyToken::from_hex(text);
  if (!parsed.has_value()) {
    usage_error("'" + text + "' is not a 32-character hexadecimal idempotency token");
  }
  return parsed.value();
}

[[nodiscard]] fsl::SubjectRef parse_subject(const std::string& text) {
  auto parsed = fsl::SubjectRef::parse(text);
  if (!parsed.has_value()) {
    usage_error(parsed.status().message());
  }
  return std::move(parsed).value();
}

[[nodiscard]] std::vector<std::uint8_t> parse_hex(const std::string& text, const std::string& option) {
  auto parsed = fsl::from_hex(text, 1u << 20);
  if (!parsed.has_value()) {
    usage_error("option " + option + " is not valid hexadecimal");
  }
  return std::move(parsed).value();
}

/// Deterministic identity for a command line, so that repeating the same append
/// is an idempotent retry rather than a second event.
[[nodiscard]] fsl::EventId derive_identity(const std::string& command_line) {
  const fsl::Digest digest = fsl::Sha256::hash(command_line);
  return fsl::EventId::from_bytes(digest.data());
}

[[nodiscard]] std::string canonical_command_line(const Options& options) {
  std::string text = options.command;
  const auto append = [&text](const char* name, const std::string& value) {
    text.push_back('|');
    text.append(name);
    text.push_back('=');
    text.append(value);
  };
  if (options.kind.has_value()) {
    append("kind", *options.kind);
  }
  if (options.subject.has_value()) {
    append("subject", *options.subject);
  }
  if (options.source.has_value()) {
    append("source", *options.source);
  }
  if (options.reference.has_value()) {
    append("ref", *options.reference);
  }
  if (options.reason.has_value()) {
    append("reason", *options.reason);
  }
  if (options.location.has_value()) {
    append("location", *options.location);
  }
  if (options.related.has_value()) {
    append("related", *options.related);
  }
  if (options.relationship.has_value()) {
    append("relationship", *options.relationship);
  }
  if (options.payload_hex.has_value()) {
    append("payload", *options.payload_hex);
  }
  if (options.body_hex.has_value()) {
    append("body", *options.body_hex);
  }
  const auto append_number = [&append](const char* name, const std::optional<std::uint64_t>& value) {
    if (value.has_value()) {
      append(name, std::to_string(*value));
    }
  };
  append_number("generation", options.generation);
  append_number("epoch", options.epoch);
  append_number("to-generation", options.to_generation);
  return text;
}

[[nodiscard]] fsl::SubmittedObservation build_append_observation(const Options& options) {
  if (!options.kind.has_value()) {
    usage_error("append requires --kind");
  }
  const auto kind = fsl::event_kind_from_string(*options.kind);
  if (!kind.has_value()) {
    usage_error("unknown event kind '" + *options.kind + "'");
  }

  fsl::SubmittedObservationFields fields;
  fields.kind = kind;
  fields.event_id = options.event_id.has_value() ? parse_event_id(*options.event_id)
                                                 : derive_identity(canonical_command_line(options));
  if (options.token.has_value()) {
    fields.idempotency_token = parse_token(*options.token);
  }
  fields.facility_generation = fsl::FacilityGeneration(options.generation.value_or(1));
  if (*kind != fsl::EventKind::kLedgerOpened && *kind != fsl::EventKind::kGenerationAdvanced) {
    fields.epoch = fsl::FacilityEpoch(options.epoch.value_or(1));
  }

  if (options.subject.has_value()) {
    fields.subject = parse_subject(*options.subject);
  } else if (options.subject_kind.has_value() && options.subject_key.has_value()) {
    const auto subject_kind = fsl::subject_kind_from_string(*options.subject_kind);
    if (!subject_kind.has_value()) {
      usage_error("unknown subject kind '" + *options.subject_kind + "'");
    }
    auto reference = fsl::SubjectRef::create(*subject_kind, *options.subject_key);
    if (!reference.has_value()) {
      usage_error(reference.status().message());
    }
    fields.subject = std::move(reference).value();
  } else {
    fields.subject = parse_subject("ledger/facility-state-ledger");
  }

  auto source = fsl::SourceComponentId::parse(options.source.value_or("fsl-cli"));
  if (!source.has_value()) {
    usage_error(source.status().message());
  }
  auto provenance = fsl::ProvenanceRecord::create(std::move(source).value(), fsl::SourceGeneration::first(),
                                                  std::nullopt, std::nullopt, {});
  if (!provenance.has_value()) {
    fail(provenance.status());
  }
  fields.provenance = std::move(provenance).value();

  const fsl::SchemaId required = fsl::payload::required_schema(*kind);
  if (options.schema.has_value()) {
    auto schema = fsl::SchemaId::parse(*options.schema);
    if (!schema.has_value()) {
      usage_error(schema.status().message());
    }
    fields.payload_schema = std::move(schema).value();
  } else {
    fields.payload_schema = required;
  }
  fields.payload_schema_version =
      fsl::SchemaVersion(options.schema_version.value_or(fsl::payload::required_schema_version(*kind).value()));

  const std::vector<std::uint8_t> body =
      options.body_hex.has_value() ? parse_hex(*options.body_hex, "--body-hex") : std::vector<std::uint8_t>{};

  switch (*kind) {
    case fsl::EventKind::kLedgerOpened: {
      fsl::payload::LedgerOpened payload;
      payload.ledger_id = fsl::LedgerId::from_words(0, 0);
      payload.segment_format_version = 1;
      payload.manifest_generation = 1;
      fields.payload = fsl::payload::encode(payload);
      break;
    }
    case fsl::EventKind::kEpochOpened:
      fields.payload = fsl::payload::encode(fsl::payload::EpochOpened{
          fsl::FacilityEpoch(options.epoch.value_or(1)), fields.facility_generation.value()});
      break;
    case fsl::EventKind::kEpochClosed:
      fields.payload =
          fsl::payload::encode(fsl::payload::EpochClosed{fsl::FacilityEpoch(options.epoch.value_or(1))});
      break;
    case fsl::EventKind::kGenerationAdvanced:
      fields.payload = fsl::payload::encode(fsl::payload::GenerationAdvanced{
          fields.facility_generation.value(),
          fsl::FacilityGeneration(options.to_generation.value_or(fields.facility_generation->value() + 1))});
      break;
    case fsl::EventKind::kSubjectRegistered: {
      std::optional<fsl::SubjectRef> location;
      if (options.location.has_value()) {
        location = parse_subject(*options.location);
      }
      fields.payload = fsl::payload::encode(
          fsl::payload::SubjectRegistered{*fields.subject, std::move(location)});
      break;
    }
    case fsl::EventKind::kSubjectRetired:
      fields.payload = fsl::payload::encode(
          fsl::payload::SubjectRetired{options.reason.value_or("retired by operator")});
      break;
    case fsl::EventKind::kRelationshipAsserted:
    case fsl::EventKind::kRelationshipRetracted: {
      if (!options.related.has_value() || !options.relationship.has_value()) {
        usage_error("this kind requires --related T/K and --relationship KIND");
      }
      const auto relationship = fsl::relationship_kind_from_string(*options.relationship);
      if (!relationship.has_value()) {
        usage_error("unknown relationship kind '" + *options.relationship + "'");
      }
      fields.payload =
          fsl::payload::encode(fsl::payload::Relationship{parse_subject(*options.related), *relationship});
      break;
    }
    case fsl::EventKind::kCorrectionRecorded:
      if (!options.event_id.has_value() && !options.after_sequence.has_value()) {
        // The correction target is supplied through --after, which names the
        // sequence being corrected; its identity is read from the ledger.
      }
      fields.payload = fsl::payload::encode(
          fsl::payload::CorrectionRecorded{options.reason.value_or("corrected by operator")});
      break;
    case fsl::EventKind::kObservationAccepted:
      fields.payload = fsl::payload::encode(fsl::payload::ObservationAccepted{
          options.reference.value_or("observation"), body});
      break;
    case fsl::EventKind::kSubjectMutated:
      fields.payload = fsl::payload::encode(
          fsl::payload::SubjectMutated{options.reference.value_or("mutation"), body});
      break;
    case fsl::EventKind::kReconciliationRecorded:
      fields.payload = fsl::payload::encode(
          fsl::payload::ReconciliationRecorded{options.reference.value_or("reconciliation"), body});
      break;
    case fsl::EventKind::kPolicyAttested:
      fields.payload = fsl::payload::encode(
          fsl::payload::PolicyAttested{options.reference.value_or("policy"), body});
      break;
  }

  if (options.payload_hex.has_value()) {
    fields.payload = parse_hex(*options.payload_hex, "--payload-hex");
  }

  auto observation = fsl::SubmittedObservation::create(std::move(fields));
  if (!observation.has_value()) {
    fail(observation.status());
  }
  return std::move(observation).value();
}

// -- commands -----------------------------------------------------------------

int command_init(const Options& options) {
  auto created = fsl::Ledger::create(require_directory(options), make_options(options, true));
  if (!created.has_value()) {
    fail(created.status());
  }
  fsl::Ledger ledger = std::move(created).value();
  const auto identity = ledger.identity();
  if (!identity.has_value()) {
    fail(identity.status());
  }
  const auto watermark = ledger.watermark();
  if (!watermark.has_value()) {
    fail(watermark.status());
  }
  std::printf("ledger_id=%s\ncommitted=%s\ncreated=%s\n", identity->id.to_hex().c_str(),
              watermark->sequence.has_value() ? std::to_string(watermark->sequence->value()).c_str() : "none",
              ledger.open_report().created ? "yes" : "no");
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_append(const Options& options) {
  const std::filesystem::path directory = require_directory(options);
  fsl::Ledger ledger = open_read_write(options);

  if (!options.kind.has_value()) {
    usage_error("append requires --kind");
  }
  const auto parsed_kind = fsl::event_kind_from_string(*options.kind);
  if (!parsed_kind.has_value()) {
    usage_error("unknown event kind '" + *options.kind + "'");
  }

  if (*parsed_kind == fsl::EventKind::kCorrectionRecorded) {
    if (!options.after_sequence.has_value()) {
      usage_error("correction-recorded requires --after <sequence> naming the corrected record");
    }
    auto target = ledger.read(fsl::LedgerSequence(*options.after_sequence));
    if (!target.has_value()) {
      fail(target.status());
    }

    fsl::SubmittedObservationFields fields;
    fields.kind = fsl::EventKind::kCorrectionRecorded;
    fields.event_id = options.event_id.has_value() ? parse_event_id(*options.event_id)
                                                   : derive_identity(canonical_command_line(options));
    if (options.token.has_value()) {
      fields.idempotency_token = parse_token(*options.token);
    }
    fields.facility_generation = fsl::FacilityGeneration(options.generation.value_or(1));
    fields.epoch = fsl::FacilityEpoch(options.epoch.value_or(1));
    fields.subject = options.subject.has_value() ? parse_subject(*options.subject) : target->subject();
    fields.correction_target = target->event_id();
    auto source = fsl::SourceComponentId::parse(options.source.value_or("fsl-cli"));
    if (!source.has_value()) {
      usage_error(source.status().message());
    }
    auto provenance = fsl::ProvenanceRecord::create(std::move(source).value(),
                                                    fsl::SourceGeneration::first(), std::nullopt,
                                                    std::nullopt, {});
    if (!provenance.has_value()) {
      fail(provenance.status());
    }
    fields.provenance = std::move(provenance).value();
    fields.payload_schema = fsl::payload::required_schema(fsl::EventKind::kCorrectionRecorded);
    fields.payload_schema_version =
        fsl::payload::required_schema_version(fsl::EventKind::kCorrectionRecorded);
    fields.payload = fsl::payload::encode(
        fsl::payload::CorrectionRecorded{options.reason.value_or("corrected by operator")});

    auto observation = fsl::SubmittedObservation::create(std::move(fields));
    if (!observation.has_value()) {
      fail(observation.status());
    }
    auto outcome = ledger.append(observation.value());
    if (!outcome.has_value()) {
      fail(outcome.status());
    }
    std::printf("sequence=%llu duplicate=%s event_id=%s directory=%s\n",
                static_cast<unsigned long long>(outcome->event.sequence().value()),
                outcome->duplicate ? "yes" : "no", outcome->event.event_id().to_hex().c_str(),
                directory.string().c_str());
    const auto closed = ledger.close();
    if (!closed.has_value()) {
      fail(closed);
    }
    return kExitOk;
  }
  const fsl::SubmittedObservation observation = build_append_observation(options);
  const std::uint64_t repeats = options.repeat.value_or(1);
  for (std::uint64_t i = 0; i < repeats; ++i) {
    auto outcome = ledger.append(observation);
    if (!outcome.has_value()) {
      fail(outcome.status());
    }
    std::printf("sequence=%llu duplicate=%s event_id=%s directory=%s\n",
                static_cast<unsigned long long>(outcome->event.sequence().value()),
                outcome->duplicate ? "yes" : "no", outcome->event.event_id().to_hex().c_str(),
                directory.string().c_str());
  }
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

void print_watermark_fields(const fsl::LedgerState& state) {
  std::printf("ledger_id=%s\n", state.identity.id.to_hex().c_str());
  std::printf("segment_format_version=%llu\n",
              static_cast<unsigned long long>(state.identity.segment_format_version));
  std::printf("manifest_format_version=%llu\n",
              static_cast<unsigned long long>(state.identity.manifest_format_version));
  std::printf("committed_sequence=%s\n",
              state.watermark.sequence.has_value()
                  ? std::to_string(state.watermark.sequence->value()).c_str()
                  : "none");
  std::printf("committed_event_count=%llu\n",
              static_cast<unsigned long long>(state.watermark.event_count));
  std::printf("committed_segment=%s\n",
              state.watermark.segment_index.has_value()
                  ? std::to_string(state.watermark.segment_index->value()).c_str()
                  : "none");
  std::printf("committed_offset=%llu\n", static_cast<unsigned long long>(state.watermark.offset));
  std::printf("committed_chain=%s\n", state.watermark.chain.to_hex().c_str());
  std::printf("manifest_generation=%llu\n",
              static_cast<unsigned long long>(state.watermark.manifest_generation.value()));
  std::printf("facility_generation=%llu\n",
              static_cast<unsigned long long>(state.facility_generation.value()));
  std::printf("open_epoch=%s\n",
              state.open_epoch.has_value() ? std::to_string(state.open_epoch->value()).c_str() : "none");
  std::printf("latest_epoch=%s\n",
              state.latest_epoch.has_value() ? std::to_string(state.latest_epoch->value()).c_str() : "none");
  std::printf("active_segment=%llu\n", static_cast<unsigned long long>(state.active_segment.value()));
  std::printf("segment_count=%llu\n", static_cast<unsigned long long>(state.segment_count));
  std::printf("writer_incarnation=%llu\n",
              static_cast<unsigned long long>(state.writer_incarnation.value()));
  std::printf("writable=%s\n", state.writable ? "yes" : "no");
  std::printf("durable=%s\n", state.durable ? "yes" : "no");
  std::printf("closed=%s\n", state.closed ? "yes" : "no");
}

int command_inspect(const Options& options) {
  fsl::Ledger ledger = open_read_only(options);
  const auto state = ledger.state();
  if (!state.has_value()) {
    fail(state.status());
  }
  print_watermark_fields(state.value());

  const fsl::LedgerStats stats = ledger.stats();
  std::printf("committed_frames=%llu\n", static_cast<unsigned long long>(stats.committed_frames));
  std::printf("index_postings=%llu\n", static_cast<unsigned long long>(stats.index_postings));
  std::printf("index_postings_complete=%s\n", stats.index_postings_complete ? "yes" : "no");
  std::printf("dedupe_entries=%llu\n", static_cast<unsigned long long>(stats.dedupe_entries));
  std::printf("subjects_known=%llu\n", static_cast<unsigned long long>(stats.subjects_known));
  std::printf("sources_tracked=%llu\n", static_cast<unsigned long long>(stats.sources_tracked));

  const auto checkpoints = ledger.checkpoints();
  if (checkpoints.has_value()) {
    std::printf("checkpoints=%llu\n", static_cast<unsigned long long>(checkpoints->size()));
  }
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_segments(const Options& options) {
  fsl::Ledger ledger = open_read_only(options);
  const auto segments = ledger.segments();
  if (!segments.has_value()) {
    fail(segments.status());
  }
  for (const fsl::SegmentInfo& info : segments.value()) {
    std::printf("segment=%llu file=%s first=%s last=%s events=%llu frames=%llu bytes=%llu sealed=%s\n",
                static_cast<unsigned long long>(info.index.value()), info.file_name.c_str(),
                info.first_sequence.has_value()
                    ? std::to_string(info.first_sequence->value()).c_str()
                    : "none",
                info.last_sequence.has_value() ? std::to_string(info.last_sequence->value()).c_str() : "none",
                static_cast<unsigned long long>(info.event_count),
                static_cast<unsigned long long>(info.frame_count),
                static_cast<unsigned long long>(info.byte_size), info.sealed ? "yes" : "no");
  }
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_read(const Options& options) {
  fsl::Ledger ledger = open_read_only(options);
  fsl::LedgerSequence sequence{1};
  if (options.sequence.has_value()) {
    sequence = fsl::LedgerSequence(*options.sequence);
  } else if (options.event_id.has_value()) {
    auto found = ledger.find_event(parse_event_id(*options.event_id));
    if (!found.has_value()) {
      fail(found.status());
    }
    sequence = found->sequence();
  } else {
    usage_error("read requires --sequence or --event-id");
  }
  fsl::AuditExportRequest request;
  request.from = sequence;
  request.to = sequence;
  request.max_events = 1;
  auto text = ledger.export_audit_json(request);
  if (!text.has_value()) {
    fail(text.status());
  }
  std::fputs(text->c_str(), stdout);
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_find(const Options& options) {
  fsl::Ledger ledger = open_read_only(options);
  fsl::Result<fsl::EventEnvelope> found =
      options.event_id.has_value()
          ? ledger.find_event(parse_event_id(*options.event_id))
          : (options.token.has_value() ? ledger.find_idempotency(parse_token(*options.token))
                                       : fsl::Result<fsl::EventEnvelope>(
                                             fsl::invalid_argument(fsl::ErrorCode::kMissingRequiredAttribute,
                                                                   "find requires --event-id or --token")));
  if (!found.has_value()) {
    fail(found.status());
  }
  fsl::AuditExportRequest request;
  request.from = found->sequence();
  request.to = found->sequence();
  request.max_events = 1;
  auto text = ledger.export_audit_json(request);
  if (!text.has_value()) {
    fail(text.status());
  }
  std::fputs(text->c_str(), stdout);
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_query(const Options& options) {
  fsl::Ledger ledger = open_read_only(options);
  if (!options.limit.has_value()) {
    usage_error("query requires --limit");
  }
  fsl::Query query;
  query.limit = static_cast<std::size_t>(*options.limit);
  if (options.sequence_from.has_value()) {
    query.from = fsl::LedgerSequence(*options.sequence_from);
  }
  if (options.sequence_to.has_value()) {
    query.to = fsl::LedgerSequence(*options.sequence_to);
  }
  if (options.after_sequence.has_value()) {
    query.after = fsl::LedgerSequence(*options.after_sequence);
  }
  if (options.subject.has_value()) {
    query.subject = parse_subject(*options.subject);
  }
  if (options.source.has_value()) {
    auto source = fsl::SourceComponentId::parse(*options.source);
    if (!source.has_value()) {
      usage_error(source.status().message());
    }
    query.source = std::move(source).value();
  }
  if (options.kind.has_value()) {
    const auto kind = fsl::event_kind_from_string(*options.kind);
    if (!kind.has_value()) {
      usage_error("unknown event kind '" + *options.kind + "'");
    }
    query.kind = kind;
  }
  if (options.epoch.has_value()) {
    query.epoch = fsl::FacilityEpoch(*options.epoch);
  }
  if (options.generation.has_value()) {
    query.facility_generation = fsl::FacilityGeneration(*options.generation);
  }

  auto page = ledger.query(query);
  if (!page.has_value()) {
    fail(page.status());
  }
  for (const fsl::EventEnvelope& event : page->events) {
    fsl::AuditExportRequest request;
    request.from = event.sequence();
    request.to = event.sequence();
    request.max_events = 1;
    auto text = ledger.export_audit_json(request);
    if (!text.has_value()) {
      fail(text.status());
    }
    std::fputs(text->c_str(), stdout);
  }
  std::fprintf(stderr, "source=%s examined=%llu returned=%llu exhausted=%s\n",
               std::string(fsl::to_string(page->source)).c_str(),
               static_cast<unsigned long long>(page->records_examined),
               static_cast<unsigned long long>(page->events.size()), page->exhausted ? "yes" : "no");
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_replay(const Options& options) {
  fsl::Ledger ledger = open_read_only(options);
  fsl::ReplayRequest request;
  if (options.sequence_from.has_value()) {
    request.from = fsl::LedgerSequence(*options.sequence_from);
  }
  if (options.sequence_to.has_value()) {
    request.to = fsl::LedgerSequence(*options.sequence_to);
  }
  if (options.kind.has_value()) {
    const auto kind = fsl::event_kind_from_string(*options.kind);
    if (!kind.has_value()) {
      usage_error("unknown event kind '" + *options.kind + "'");
    }
    request.kind = kind;
  }
  if (options.limit.has_value()) {
    request.max_events = static_cast<std::size_t>(*options.limit);
  }

  std::vector<fsl::LedgerSequence> sequences;
  const fsl::Result<std::uint64_t> replay_result =
      ledger.replay_each(request, [&sequences](const fsl::EventEnvelope& event) {
        sequences.push_back(event.sequence());
        return true;
      });
  if (!replay_result.has_value()) {
    fail(replay_result.status());
  }
  for (const fsl::LedgerSequence sequence : sequences) {
    fsl::AuditExportRequest export_request;
    export_request.from = sequence;
    export_request.to = sequence;
    export_request.max_events = 1;
    auto text = ledger.export_audit_json(export_request);
    if (!text.has_value()) {
      fail(text.status());
    }
    std::fputs(text->c_str(), stdout);
  }
  std::fprintf(stderr, "replayed=%llu\n", static_cast<unsigned long long>(sequences.size()));
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_verify(const Options& options) {
  fsl::Ledger ledger = open_read_only(options);
  fsl::VerifyRequest request;
  request.verify_index = options.verify_index;
  if (options.scope.has_value()) {
    if (*options.scope == "manifest-tail") {
      request.scope = fsl::VerifyScope::kManifestTail;
    } else if (*options.scope == "full") {
      request.scope = fsl::VerifyScope::kFull;
    } else if (*options.scope == "from-checkpoint") {
      request.scope = fsl::VerifyScope::kFromCheckpoint;
    } else {
      usage_error("unknown verify scope '" + *options.scope + "'");
    }
  } else {
    request.scope = fsl::VerifyScope::kFull;
  }
  if (options.checkpoint.has_value()) {
    request.checkpoint_sequence = fsl::LedgerSequence(*options.checkpoint);
  }

  const fsl::VerifyReport report = ledger.verify(request);
  std::printf("status=%s\n", report.status.to_string().c_str());
  std::printf("records_verified=%llu\n", static_cast<unsigned long long>(report.records_verified));
  std::printf("bytes_verified=%llu\n", static_cast<unsigned long long>(report.bytes_verified));
  std::printf("segments_verified=%llu\n", static_cast<unsigned long long>(report.segments_verified));
  std::printf("first_sequence=%s\n", report.first_sequence.has_value()
                                         ? std::to_string(report.first_sequence->value()).c_str()
                                         : "none");
  std::printf("last_sequence=%s\n", report.last_sequence.has_value()
                                        ? std::to_string(report.last_sequence->value()).c_str()
                                        : "none");
  std::printf("chain_at_end=%s\n", report.chain_at_end.to_hex().c_str());
  std::printf("index_verified=%s\n", report.index_verified ? "yes" : "no");
  std::printf("index_consistent=%s\n", report.index_consistent ? "yes" : "no");
  for (const fsl::VerifyFinding& finding : report.findings) {
    std::printf("finding code=%s sequence=%s segment=%s offset=%llu message=%s\n",
                std::string(fsl::to_string(finding.code)).c_str(),
                finding.sequence.has_value() ? std::to_string(finding.sequence->value()).c_str() : "none",
                finding.segment_index.has_value()
                    ? std::to_string(finding.segment_index->value()).c_str()
                    : "none",
                static_cast<unsigned long long>(finding.offset), finding.message.c_str());
  }
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return report.ok() ? kExitOk : kExitFailure;
}

struct StdoutSink final : fsl::IAuditSink {
  bool write(std::string_view record) override {
    return std::fwrite(record.data(), 1, record.size(), stdout) == record.size();
  }
};

int command_export(const Options& options) {
  fsl::Ledger ledger = open_read_only(options);
  fsl::AuditExportRequest request;
  if (options.sequence_from.has_value()) {
    request.from = fsl::LedgerSequence(*options.sequence_from);
  }
  if (options.sequence_to.has_value()) {
    request.to = fsl::LedgerSequence(*options.sequence_to);
  }
  if (options.subject.has_value()) {
    request.subject = parse_subject(*options.subject);
  }
  if (options.limit.has_value()) {
    request.max_events = static_cast<std::size_t>(*options.limit);
  }
  StdoutSink sink;
  const auto status = ledger.export_audit(request, sink);
  if (status.is_error()) {
    fail(status);
  }
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_checkpoints(const Options& options) {
  fsl::Ledger ledger = open_read_only(options);
  const auto checkpoints = ledger.checkpoints();
  if (!checkpoints.has_value()) {
    fail(checkpoints.status());
  }
  for (const fsl::CheckpointInfo& info : checkpoints.value()) {
    std::printf(
        "sequence=%llu file=%s logical_tick=%llu segment=%llu offset=%llu events=%llu chain=%s "
        "created_unix_nanos=%llu\n",
        static_cast<unsigned long long>(info.sequence.value()), info.file_name.c_str(),
        static_cast<unsigned long long>(info.logical_tick.value()),
        static_cast<unsigned long long>(info.segment_index.value()),
        static_cast<unsigned long long>(info.offset),
        static_cast<unsigned long long>(info.event_count), info.chain.to_hex().c_str(),
        static_cast<unsigned long long>(info.created_unix_nanos));
  }
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_subjects(const Options& options) {
  fsl::Ledger ledger = open_read_only(options);
  if (!options.limit.has_value()) {
    usage_error("subjects requires --limit");
  }
  std::optional<fsl::SubjectRef> after;
  if (options.subject.has_value()) {
    after = parse_subject(*options.subject);
  }
  auto listing = ledger.subjects(static_cast<std::size_t>(*options.limit), after);
  if (!listing.has_value()) {
    fail(listing.status());
  }
  for (const fsl::SubjectView& view : listing.value()) {
    std::printf("subject=%s registered=%s retired=%s events=%llu first=%s last=%s\n",
                view.subject.to_string().c_str(), view.registered ? "yes" : "no",
                view.retired ? "yes" : "no", static_cast<unsigned long long>(view.event_count),
                view.first_sequence.has_value()
                    ? std::to_string(view.first_sequence->value()).c_str()
                    : "none",
                view.last_sequence.has_value() ? std::to_string(view.last_sequence->value()).c_str()
                                               : "none");
  }
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_sources(const Options& options) {
  fsl::Ledger ledger = open_read_only(options);
  if (!options.limit.has_value()) {
    usage_error("sources requires --limit");
  }
  auto listing = ledger.sources(static_cast<std::size_t>(*options.limit));
  if (!listing.has_value()) {
    fail(listing.status());
  }
  for (const fsl::SourceView& view : listing.value()) {
    std::printf("source=%s generation=%llu events=%llu last_sequence=%s last_source_sequence=%s\n",
                view.source.str().c_str(), static_cast<unsigned long long>(view.generation.value()),
                static_cast<unsigned long long>(view.event_count),
                view.last_sequence.has_value() ? std::to_string(view.last_sequence->value()).c_str()
                                               : "none",
                view.last_source_sequence.has_value()
                    ? std::to_string(view.last_source_sequence->value()).c_str()
                    : "none");
  }
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_checkpoint(const Options& options) {
  fsl::Ledger ledger = open_read_write(options);
  auto info = ledger.create_checkpoint();
  if (!info.has_value()) {
    fail(info.status());
  }
  std::printf("sequence=%llu file=%s offset=%llu events=%llu chain=%s\n",
              static_cast<unsigned long long>(info->sequence.value()), info->file_name.c_str(),
              static_cast<unsigned long long>(info->offset),
              static_cast<unsigned long long>(info->event_count), info->chain.to_hex().c_str());
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_rotate(const Options& options) {
  fsl::Ledger ledger = open_read_write(options);
  const auto rotated = ledger.rotate_segment();
  if (!rotated.has_value()) {
    fail(rotated);
  }
  const auto state = ledger.state();
  if (!state.has_value()) {
    fail(state.status());
  }
  std::printf("active_segment=%llu segment_count=%llu committed_sequence=%s\n",
              static_cast<unsigned long long>(state->active_segment.value()),
              static_cast<unsigned long long>(state->segment_count),
              state->watermark.sequence.has_value()
                  ? std::to_string(state->watermark.sequence->value()).c_str()
                  : "none");
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return kExitOk;
}

int command_recover(const Options& options) {
  fsl::Ledger ledger = open_read_write(options);
  fsl::RecoveryRequest request;
  request.dry_run = options.dry_run;
  request.rebuild_index = options.rebuild_index;
  const fsl::RecoveryReport report = ledger.recover(request);
  std::printf("status=%s\n", report.status.to_string().c_str());
  std::printf("dry_run=%s\n", report.dry_run ? "yes" : "no");
  std::printf("truncated=%s\n", report.truncated ? "yes" : "no");
  std::printf("truncated_tail_bytes=%llu\n", static_cast<unsigned long long>(report.truncated_tail_bytes));
  std::printf("truncated_tail_frames=%llu\n", static_cast<unsigned long long>(report.truncated_tail_frames));
  std::printf("index_rebuilt=%s\n", report.index_rebuilt ? "yes" : "no");
  std::printf("manifest_recovered_from_backup=%s\n",
              report.manifest_recovered_from_backup ? "yes" : "no");
  std::printf("replayed_events=%llu\n", static_cast<unsigned long long>(report.replayed_events));
  for (const fsl::RecoveryNote& note : report.notes) {
    std::printf("note code=%s message=%s\n", std::string(fsl::to_string(note.code)).c_str(),
                note.message.c_str());
  }
  const auto closed = ledger.close();
  if (!closed.has_value()) {
    fail(closed);
  }
  return report.status.is_ok() ? kExitOk : kExitFailure;
}

}  // namespace

int main(int argc, char** argv) {
  const Parsed parsed = parse(argc, argv);
  if (parsed.options.help) {
    print_help();
    return kExitOk;
  }
  const Options& options = parsed.options;
  const std::string& command = options.command;

  if (command == "init") {
    return command_init(options);
  }
  if (command == "append") {
    return command_append(options);
  }
  if (command == "inspect") {
    return command_inspect(options);
  }
  if (command == "segments") {
    return command_segments(options);
  }
  if (command == "read") {
    return command_read(options);
  }
  if (command == "find") {
    return command_find(options);
  }
  if (command == "query") {
    return command_query(options);
  }
  if (command == "replay") {
    return command_replay(options);
  }
  if (command == "verify") {
    return command_verify(options);
  }
  if (command == "export") {
    return command_export(options);
  }
  if (command == "checkpoints") {
    return command_checkpoints(options);
  }
  if (command == "subjects") {
    return command_subjects(options);
  }
  if (command == "sources") {
    return command_sources(options);
  }
  if (command == "checkpoint") {
    return command_checkpoint(options);
  }
  if (command == "rotate") {
    return command_rotate(options);
  }
  if (command == "recover") {
    return command_recover(options);
  }
  std::fprintf(stderr, "fsl: unknown command '%s'\n", command.c_str());
  std::fprintf(stderr, "run 'fsl --help' for usage\n");
  return kExitUsage;
}

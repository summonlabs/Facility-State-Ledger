// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "detail/codec.hpp"

#include "fsl/text.hpp"

namespace fsl::detail {
namespace {

constexpr std::uint32_t kMaxSubjectKeyBytes = 512;
constexpr std::uint32_t kMaxSourceBytes = 128;
constexpr std::uint32_t kMaxSchemaBytes = 96;
constexpr std::uint32_t kMaxAttributeCount = 64;

[[nodiscard]] bool decode_event_kind(ByteReader& reader, EventKind& out) noexcept {
  std::uint8_t raw = 0;
  if (!reader.u8(raw)) {
    return false;
  }
  if (raw < static_cast<std::uint8_t>(EventKind::kLedgerOpened) ||
      raw > static_cast<std::uint8_t>(EventKind::kPolicyAttested)) {
    return false;
  }
  out = static_cast<EventKind>(raw);
  return true;
}

[[nodiscard]] bool decode_relationship_kind(ByteReader& reader, RelationshipKind& out) noexcept {
  std::uint8_t raw = 0;
  if (!reader.u8(raw)) {
    return false;
  }
  if (raw < static_cast<std::uint8_t>(RelationshipKind::kContainedIn) ||
      raw > static_cast<std::uint8_t>(RelationshipKind::kDependsOn)) {
    return false;
  }
  out = static_cast<RelationshipKind>(raw);
  return true;
}

[[nodiscard]] bool decode_subject_kind(ByteReader& reader, SubjectKind& out) noexcept {
  std::uint8_t raw = 0;
  if (!reader.u8(raw)) {
    return false;
  }
  if (raw < static_cast<std::uint8_t>(SubjectKind::kRack) ||
      raw > static_cast<std::uint8_t>(SubjectKind::kLedger)) {
    return false;
  }
  out = static_cast<SubjectKind>(raw);
  return true;
}

}  // namespace

void encode_subject_ref(ByteWriter& writer, const SubjectRef& reference) {
  writer.u8(static_cast<std::uint8_t>(reference.kind()));
  writer.sized_string(reference.key().view());
}

bool decode_subject_ref(ByteReader& reader, std::optional<SubjectRef>& out) {
  SubjectKind kind = SubjectKind::kRack;
  if (!decode_subject_kind(reader, kind)) {
    return false;
  }
  std::string key;
  if (!reader.sized_string(key, kMaxSubjectKeyBytes)) {
    return false;
  }
  auto reference = SubjectRef::create(kind, key);
  if (!reference.has_value()) {
    return false;
  }
  out = std::move(reference).value();
  return true;
}

void encode_provenance(ByteWriter& writer, const ProvenanceRecord& provenance) {
  writer.sized_string(provenance.source().view());
  writer.u64(provenance.source_generation().value());
  writer.optional_u64(provenance.source_sequence().has_value()
                          ? std::optional<std::uint64_t>(provenance.source_sequence()->value())
                          : std::nullopt);
  writer.optional_u64(provenance.source_wall_clock_unix_nanos());
  writer.u32(static_cast<std::uint32_t>(provenance.attributes().size()));
  for (const ProvenanceAttribute& attribute : provenance.attributes()) {
    writer.sized_string(attribute.key);
    writer.sized_string(attribute.value);
  }
}

Result<ProvenanceRecord> decode_provenance(ByteReader& reader) {
  std::string source_text;
  if (!reader.sized_string(source_text, kMaxSourceBytes)) {
    return malformed_input(ErrorCode::kTruncatedInput, "provenance source is malformed");
  }
  auto source = SourceComponentId::parse(source_text);
  if (!source.has_value()) {
    return source.status();
  }
  std::uint64_t generation_raw = 0;
  if (!reader.u64(generation_raw)) {
    return malformed_input(ErrorCode::kTruncatedInput, "provenance generation is truncated");
  }
  if (generation_raw == 0) {
    return malformed_input(ErrorCode::kInvalidEnumValue, "provenance source generation zero is invalid");
  }
  std::optional<std::uint64_t> sequence_raw;
  if (!reader.optional_u64(sequence_raw)) {
    return malformed_input(ErrorCode::kTruncatedInput, "provenance sequence is truncated");
  }
  if (sequence_raw.has_value() && *sequence_raw == 0) {
    return malformed_input(ErrorCode::kInvalidEnumValue, "provenance source sequence zero is invalid");
  }
  std::optional<std::uint64_t> wall_clock;
  if (!reader.optional_u64(wall_clock)) {
    return malformed_input(ErrorCode::kTruncatedInput, "provenance wall clock is truncated");
  }
  std::uint32_t attribute_count = 0;
  if (!reader.u32(attribute_count)) {
    return malformed_input(ErrorCode::kTruncatedInput, "provenance attribute count is truncated");
  }
  if (attribute_count > kMaxAttributeCount) {
    return malformed_input(ErrorCode::kCountOutOfRange,
                           "provenance declares " + std::to_string(attribute_count) +
                               " attributes, which exceeds the structural limit");
  }
  std::vector<ProvenanceAttribute> attributes;
  attributes.reserve(attribute_count);
  for (std::uint32_t i = 0; i < attribute_count; ++i) {
    ProvenanceAttribute attribute;
    if (!reader.sized_string(attribute.key, ProvenanceAttribute::kMaxKeyLength)) {
      return malformed_input(ErrorCode::kTruncatedInput, "provenance attribute key is malformed");
    }
    if (!reader.sized_string(attribute.value, ProvenanceAttribute::kMaxValueLength)) {
      return malformed_input(ErrorCode::kSizeOutOfRange, "provenance attribute value is too long");
    }
    attributes.push_back(std::move(attribute));
  }

  std::optional<SourceSequence> sequence;
  if (sequence_raw.has_value()) {
    sequence = SourceSequence(*sequence_raw);
  }
  auto record = ProvenanceRecord::create(std::move(source).value(),
                                         SourceGeneration(generation_raw),
                                         sequence,
                                         wall_clock,
                                         std::move(attributes));
  if (!record.has_value()) {
    return record.status();
  }
  return std::move(record).value();
}

namespace {

/// The caller-controlled content of an event, shared by the live commit path and
/// the decode path so that the bytes hashed for a content digest are produced by
/// exactly one function.
struct ContentFields {
  EventId event_id;
  FacilityGeneration facility_generation{1};
  std::optional<FacilityEpoch> epoch;
  EventKind kind = EventKind::kLedgerOpened;
  SubjectRef subject;
  std::optional<EventId> correction_target;
  SchemaId payload_schema;
  SchemaVersion payload_schema_version{1};
  std::span<const std::uint8_t> payload;
  ProvenanceRecord provenance;
};

void encode_content(ByteWriter& writer, const ContentFields& fields) {
  writer.id128(fields.event_id);
  writer.u64(fields.facility_generation.value());
  writer.boolean(fields.epoch.has_value());
  if (fields.epoch.has_value()) {
    writer.u64(fields.epoch->value());
  }
  writer.u8(static_cast<std::uint8_t>(fields.kind));
  encode_subject_ref(writer, fields.subject);
  writer.boolean(fields.correction_target.has_value());
  if (fields.correction_target.has_value()) {
    writer.id128(*fields.correction_target);
  }
  writer.sized_string(fields.payload_schema.view());
  writer.u32(fields.payload_schema_version.value());
  writer.sized_bytes(fields.payload);
  encode_provenance(writer, fields.provenance);
}

}  // namespace

std::vector<std::uint8_t> encode_submission(const SubmittedObservation& observation) {
  std::vector<std::uint8_t> out;
  out.reserve(observation.payload().size() + 256);
  ByteWriter writer(out);
  writer.u32(kSubmissionVersion);
  const ContentFields fields{observation.event_id(),
                             observation.facility_generation(),
                             observation.epoch(),
                             observation.kind(),
                             observation.subject(),
                             observation.correction_target(),
                             observation.payload_schema(),
                             observation.payload_schema_version(),
                             observation.payload(),
                             observation.provenance()};
  encode_content(writer, fields);
  return out;
}

std::vector<std::uint8_t> encode_envelope(const EventEnvelope& envelope) {
  std::vector<std::uint8_t> out;
  out.reserve(envelope.payload().size() + 384);
  ByteWriter writer(out);
  writer.u32(kEnvelopeVersion);
  writer.u64(envelope.sequence().value());
  writer.u64(envelope.accepted_at().logical_tick.value());
  writer.u64(envelope.accepted_at().monotonic_nanoseconds);
  writer.digest(envelope.content_digest());
  writer.boolean(envelope.idempotency_token().has_value());
  if (envelope.idempotency_token().has_value()) {
    writer.id128(*envelope.idempotency_token());
  }
  writer.u32(kSubmissionVersion);
  const ContentFields fields{envelope.event_id(),
                             envelope.facility_generation(),
                             envelope.epoch(),
                             envelope.kind(),
                             envelope.subject(),
                             envelope.correction_target(),
                             envelope.payload_schema(),
                             envelope.payload_schema_version(),
                             envelope.payload(),
                             envelope.provenance()};
  encode_content(writer, fields);
  return out;
}

Result<EventEnvelope> decode_envelope(std::span<const std::uint8_t> body) {
  ByteReader reader(body);
  std::uint32_t envelope_version = 0;
  std::uint64_t sequence = 0;
  std::uint64_t logical_tick = 0;
  std::uint64_t monotonic_nanos = 0;
  Digest content_digest;
  bool has_token = false;
  IdempotencyToken token;
  std::uint32_t submission_version = 0;
  EventId event_id;
  std::uint64_t facility_generation = 0;
  bool has_epoch = false;
  std::uint64_t epoch = 0;
  EventKind kind = EventKind::kLedgerOpened;
  std::optional<SubjectRef> subject;
  bool has_correction = false;
  EventId correction_target;

  if (!reader.u32(envelope_version)) {
    return malformed_input(ErrorCode::kTruncatedInput, "event frame body is truncated");
  }
  if (envelope_version != kEnvelopeVersion) {
    return unsupported_version(ErrorCode::kUnsupportedFormatVersion,
                               "event envelope version " + std::to_string(envelope_version) +
                                   " is not supported");
  }
  if (!reader.u64(sequence) || !reader.u64(logical_tick) || !reader.u64(monotonic_nanos) ||
      !reader.digest(content_digest) || !reader.boolean(has_token)) {
    return malformed_input(ErrorCode::kTruncatedInput, "event frame body is truncated");
  }
  if (has_token && !reader.id128(token)) {
    return malformed_input(ErrorCode::kTruncatedInput, "event frame body is truncated");
  }
  if (!reader.u32(submission_version)) {
    return malformed_input(ErrorCode::kTruncatedInput, "event frame body is truncated");
  }
  if (submission_version != kSubmissionVersion) {
    return unsupported_version(ErrorCode::kUnsupportedFormatVersion,
                               "submission encoding version " + std::to_string(submission_version) +
                                   " is not supported");
  }
  if (sequence == 0 || logical_tick == 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid,
                           "committed event carries a zero sequence or logical tick");
  }
  if (!reader.id128(event_id) || !reader.u64(facility_generation) || !reader.boolean(has_epoch)) {
    return malformed_input(ErrorCode::kTruncatedInput, "event frame body is truncated");
  }
  if (has_epoch && !reader.u64(epoch)) {
    return malformed_input(ErrorCode::kTruncatedInput, "event frame body is truncated");
  }
  if (facility_generation == 0 || (has_epoch && epoch == 0)) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid,
                           "committed event carries a zero facility generation or epoch");
  }
  if (!decode_event_kind(reader, kind) || !decode_subject_ref(reader, subject) ||
      !reader.boolean(has_correction)) {
    return malformed_input(ErrorCode::kInvalidEnumValue, "event frame body carries an invalid domain value");
  }
  if (has_correction && !reader.id128(correction_target)) {
    return malformed_input(ErrorCode::kTruncatedInput, "event frame body is truncated");
  }
  std::string schema_text;
  std::uint32_t schema_version = 0;
  if (!reader.sized_string(schema_text, kMaxSchemaBytes) || !reader.u32(schema_version)) {
    return malformed_input(ErrorCode::kTruncatedInput, "event frame body is truncated");
  }
  if (schema_version == 0) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "payload schema version zero is invalid");
  }
  auto schema = SchemaId::parse(schema_text);
  if (!schema.has_value()) {
    return schema.status();
  }
  std::span<const std::uint8_t> payload;
  if (!reader.sized_bytes(payload, ByteReader::kMaxSizedField)) {
    return malformed_input(ErrorCode::kSizeOutOfRange, "event payload length is out of range");
  }
  auto provenance = decode_provenance(reader);
  if (!provenance.has_value()) {
    return provenance.status();
  }
  if (!reader.at_end()) {
    return malformed_input(ErrorCode::kUnexpectedTrailingBytes, "event frame body has trailing bytes");
  }
  if (event_id.is_nil()) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid, "committed event carries a nil event identity");
  }
  if (has_token && token.is_nil()) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid,
                           "committed event carries a nil idempotency token");
  }
  const bool epoch_permitted = kind != EventKind::kLedgerOpened && kind != EventKind::kGenerationAdvanced;
  if (has_epoch != epoch_permitted) {
    return malformed_input(ErrorCode::kHeaderFieldInvalid,
                           "committed event carries an epoch field that does not match its kind");
  }

  std::optional<FacilityEpoch> epoch_value;
  if (has_epoch) {
    epoch_value = FacilityEpoch(epoch);
  }
  std::optional<EventId> correction;
  if (has_correction) {
    correction = correction_target;
  }

  // Re-derive the content digest from the decoded fields and require it to match
  // what the frame claims. A record whose content and digest disagree is not a
  // record this ledger ever wrote.
  const ContentFields fields{event_id,
                             FacilityGeneration(facility_generation),
                             epoch_value,
                             kind,
                             std::move(subject).value(),
                             correction,
                             std::move(schema).value(),
                             SchemaVersion(schema_version),
                             payload,
                             std::move(provenance).value()};
  std::vector<std::uint8_t> content;
  ByteWriter content_writer(content);
  content_writer.u32(kSubmissionVersion);
  encode_content(content_writer, fields);
  if (Sha256::hash(content.data(), content.size()) != content_digest) {
    return integrity_failure(ErrorCode::kDigestMismatch,
                             "committed event content does not match its content digest");
  }

  EventEnvelope envelope(LedgerSequence(sequence),
                         event_id,
                         has_token ? std::optional<IdempotencyToken>(token) : std::nullopt,
                         FacilityGeneration(facility_generation),
                         epoch_value,
                         kind,
                         std::move(fields.subject),
                         correction,
                         std::move(fields.payload_schema),
                         SchemaVersion(schema_version),
                         std::vector<std::uint8_t>(payload.begin(), payload.end()),
                         std::move(fields.provenance),
                         AcceptedAt{LogicalTick(logical_tick), monotonic_nanos},
                         content_digest,
                         Sha256::hash(body.data(), body.size()));
  return envelope;
}

std::vector<std::uint8_t> subject_key(const SubjectRef& reference) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  encode_subject_ref(writer, reference);
  return out;
}

std::vector<std::uint8_t> relationship_key(const SubjectRef& subject,
                                           RelationshipKind kind,
                                           const SubjectRef& related) {
  std::vector<std::uint8_t> out;
  ByteWriter writer(out);
  encode_subject_ref(writer, subject);
  writer.u8(static_cast<std::uint8_t>(kind));
  encode_subject_ref(writer, related);
  return out;
}

bool decode_relationship_key(std::span<const std::uint8_t> key,
                             std::optional<SubjectRef>& subject,
                             RelationshipKind& kind,
                             std::optional<SubjectRef>& related) {
  ByteReader reader(key);
  if (!decode_subject_ref(reader, subject) || !decode_relationship_kind(reader, kind) ||
      !decode_subject_ref(reader, related)) {
    return false;
  }
  return reader.at_end();
}

}  // namespace fsl::detail

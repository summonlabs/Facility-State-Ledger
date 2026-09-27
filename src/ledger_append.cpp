// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstring>
#include <limits>

#include "detail/codec.hpp"
#include "detail/ledger_impl.hpp"
#include "fsl/payload.hpp"
#include "fsl/text.hpp"

/// \file ledger_append.cpp
/// The commit path: validate, admit, reserve, write, flush, publish.
///
/// A commit is durable when, and only when, the replacement manifest that names
/// it has been flushed into place. Frame bytes always reach stable storage
/// before the manifest that acknowledges them, so a crash can lose an
/// unacknowledged commit but can never expose a commit whose bytes are missing.
/// Recovery enforces both directions of that rule.

namespace fsl::detail {
namespace {

void store_u64_le(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

}  // namespace

void LedgerImpl::inject(Boundary boundary) const {
  if (options_.fault_injector != nullptr) {
    options_.fault_injector->on_boundary(boundary);
  }
}

// -- Derived-state probes -----------------------------------------------------

std::optional<bool> LedgerImpl::subject_is_live(const SubjectRef& reference) const {
  const std::vector<std::uint8_t> key = subject_key(reference);
  const std::vector<Posting>* postings = index_.postings(IndexKind::kSubject, key);
  if (postings == nullptr || postings->empty()) {
    return std::nullopt;
  }
  // Only registration and retirement change a subject's lifecycle. Every other
  // subject posting is an event *about* the subject and leaves its state alone,
  // so the most recent lifecycle posting decides, not the most recent posting.
  for (auto entry = postings->rbegin(); entry != postings->rend(); ++entry) {
    if (entry->extra == static_cast<std::uint64_t>(EventKind::kSubjectRegistered)) {
      return true;
    }
    if (entry->extra == static_cast<std::uint64_t>(EventKind::kSubjectRetired)) {
      return false;
    }
  }
  return std::nullopt;
}

bool LedgerImpl::subject_is_known(const SubjectRef& reference) const {
  const std::vector<std::uint8_t> key = subject_key(reference);
  const std::vector<Posting>* postings = index_.postings(IndexKind::kSubject, key);
  return postings != nullptr && !postings->empty();
}

bool LedgerImpl::event_exists(const EventId& identity) const {
  return index_.event_sequence(identity).has_value();
}

bool LedgerImpl::relationship_is_asserted(const SubjectRef& subject,
                                          RelationshipKind kind,
                                          const SubjectRef& related) const {
  const std::vector<std::uint8_t> key = relationship_key(subject, kind, related);
  const std::vector<Posting>* postings = index_.postings(IndexKind::kRelationship, key);
  if (postings == nullptr || postings->empty()) {
    return false;
  }
  return postings->back().extra == static_cast<std::uint64_t>(EventKind::kRelationshipAsserted);
}

// -- Admission ----------------------------------------------------------------

Status LedgerImpl::admit(const SubmittedObservation& observation) const {
  const EventKind kind = observation.kind();

  if (observation.facility_generation().value() < derived_.facility_generation) {
    return stale_generation(ErrorCode::kStaleFacilityGeneration,
                            "the submission targets facility generation " +
                                std::to_string(observation.facility_generation().value()) +
                                " but the authoritative generation is " +
                                std::to_string(derived_.facility_generation));
  }
  if (observation.facility_generation().value() > derived_.facility_generation) {
    return conflict(ErrorCode::kFutureFacilityGeneration,
                    "the submission targets facility generation " +
                        std::to_string(observation.facility_generation().value()) +
                        " which has not been established; the authoritative generation is " +
                        std::to_string(derived_.facility_generation));
  }

  if (kind == EventKind::kLedgerOpened) {
    if (manifest_.committed_sequence != 0) {
      return conflict(ErrorCode::kIllegalLifecycleTransition,
                      "a ledger records its own creation exactly once, as its first committed event");
    }
  } else if (kind == EventKind::kEpochOpened) {
    if (derived_.open_epoch != 0) {
      return conflict(ErrorCode::kIllegalLifecycleTransition,
                      "epoch " + std::to_string(derived_.open_epoch) +
                          " is already open; an epoch must be closed before the next is opened");
    }
    auto opened = payload::decode_epoch_opened(observation.payload(), observation.payload_schema_version());
    if (!opened.has_value()) {
      return opened.status();
    }
    if (opened.value().generation.value() != observation.facility_generation().value()) {
      return conflict(ErrorCode::kIllegalLifecycleTransition,
                      "an epoch is opened against the generation that is current when it opens");
    }
    if (!observation.epoch().has_value() || !(opened.value().epoch == *observation.epoch())) {
      return conflict(ErrorCode::kIllegalLifecycleTransition,
                      "the envelope epoch and the payload epoch must agree");
    }
    const std::uint64_t expected = derived_.latest_epoch + 1;
    if (opened.value().epoch.value() != expected) {
      return conflict(ErrorCode::kIllegalLifecycleTransition,
                      "epoch " + std::to_string(opened.value().epoch.value()) +
                          " is not the next epoch; the next epoch is " + std::to_string(expected));
    }
  } else if (kind == EventKind::kEpochClosed) {
    if (derived_.open_epoch == 0) {
      return stale_epoch(ErrorCode::kUnknownEpoch, "no epoch is open");
    }
    auto closed = payload::decode_epoch_closed(observation.payload(), observation.payload_schema_version());
    if (!closed.has_value()) {
      return closed.status();
    }
    if (!observation.epoch().has_value() || !(closed.value().epoch == *observation.epoch())) {
      return conflict(ErrorCode::kIllegalLifecycleTransition,
                      "the envelope epoch and the payload epoch must agree");
    }
    if (closed.value().epoch.value() != derived_.open_epoch) {
      return stale_epoch(ErrorCode::kStaleEpoch,
                         "epoch " + std::to_string(closed.value().epoch.value()) +
                             " is not the open epoch; epoch " + std::to_string(derived_.open_epoch) +
                             " is open");
    }
  } else if (kind == EventKind::kGenerationAdvanced) {
    if (derived_.open_epoch != 0) {
      return conflict(ErrorCode::kIllegalLifecycleTransition,
                      "the facility generation may only advance while no epoch is open; epoch " +
                          std::to_string(derived_.open_epoch) + " is open");
    }
    auto advanced =
        payload::decode_generation_advanced(observation.payload(), observation.payload_schema_version());
    if (!advanced.has_value()) {
      return advanced.status();
    }
    if (advanced.value().from.value() != derived_.facility_generation) {
      return stale_generation(ErrorCode::kStaleFacilityGeneration,
                              "the payload advances from generation " +
                                  std::to_string(advanced.value().from.value()) +
                                  " but the authoritative generation is " +
                                  std::to_string(derived_.facility_generation));
    }
  } else {
    if (!observation.epoch().has_value()) {
      return stale_epoch(ErrorCode::kUnknownEpoch, "the event kind requires an epoch and none was supplied");
    }
    if (derived_.open_epoch == 0) {
      return stale_epoch(ErrorCode::kUnknownEpoch,
                         "no facility epoch is open; open one before recording facility events");
    }
    if (observation.epoch()->value() < derived_.open_epoch) {
      return stale_epoch(ErrorCode::kEpochClosed,
                         "epoch " + std::to_string(observation.epoch()->value()) +
                             " is closed; the open epoch is " + std::to_string(derived_.open_epoch));
    }
    if (observation.epoch()->value() > derived_.open_epoch) {
      return stale_epoch(ErrorCode::kUnknownEpoch,
                         "epoch " + std::to_string(observation.epoch()->value()) +
                             " has not been opened; the open epoch is " +
                             std::to_string(derived_.open_epoch));
    }
  }

  switch (kind) {
    case EventKind::kLedgerOpened:
    case EventKind::kEpochOpened:
    case EventKind::kEpochClosed:
    case EventKind::kGenerationAdvanced:
      break;
    case EventKind::kSubjectRegistered: {
      auto registered =
          payload::decode_subject_registered(observation.payload(), observation.payload_schema_version());
      if (!registered.has_value()) {
        return registered.status();
      }
      if (!(registered.value().subject == observation.subject())) {
        return conflict(ErrorCode::kSubjectKindMismatch,
                        "the payload subject must be the subject the envelope names");
      }
      if (subject_is_live(observation.subject()).value_or(false)) {
        return conflict(ErrorCode::kSubjectAlreadyRegistered,
                        "subject " + observation.subject().to_string() + " is already registered");
      }
      if (subject_kind_requires_location(observation.subject().kind()) &&
          !registered.value().location.has_value()) {
        return invalid_argument(ErrorCode::kMissingSubjectLocation,
                                "subject " + observation.subject().to_string() +
                                    " is a physical entity and must name its location");
      }
      if (registered.value().location.has_value() && !subject_is_known(*registered.value().location)) {
        return invalid_argument(ErrorCode::kDanglingSubjectReference,
                                "the registration names location " +
                                    registered.value().location->to_string() +
                                    " which is not a known subject");
      }
      break;
    }
    case EventKind::kObservationAccepted:
    case EventKind::kSubjectMutated: {
      const std::optional<bool> live = subject_is_live(observation.subject());
      if (!live.has_value()) {
        return not_found(ErrorCode::kSubjectNotRegistered,
                         "subject " + observation.subject().to_string() + " is not registered");
      }
      if (!live.value()) {
        return conflict(ErrorCode::kSubjectRetired,
                        "subject " + observation.subject().to_string() +
                            " is retired and accepts no further records");
      }
      break;
    }
    case EventKind::kSubjectRetired: {
      const std::optional<bool> live = subject_is_live(observation.subject());
      if (!live.has_value()) {
        return not_found(ErrorCode::kSubjectNotRegistered,
                         "subject " + observation.subject().to_string() + " is not registered");
      }
      if (!live.value()) {
        return conflict(ErrorCode::kSubjectRetired,
                        "subject " + observation.subject().to_string() + " is already retired");
      }
      break;
    }
    case EventKind::kRelationshipAsserted:
    case EventKind::kRelationshipRetracted: {
      const std::optional<bool> live = subject_is_live(observation.subject());
      if (!live.has_value()) {
        return not_found(ErrorCode::kSubjectNotRegistered,
                         "subject " + observation.subject().to_string() + " is not registered");
      }
      if (!live.value()) {
        return conflict(ErrorCode::kSubjectRetired,
                        "subject " + observation.subject().to_string() + " is retired");
      }
      auto relationship =
          payload::decode_relationship(observation.payload(), observation.payload_schema_version());
      if (!relationship.has_value()) {
        return relationship.status();
      }
      if (relationship.value().related == observation.subject()) {
        return invalid_argument(ErrorCode::kSelfReference,
                                "a subject cannot hold a relationship to itself");
      }
      if (!subject_is_known(relationship.value().related)) {
        return invalid_argument(ErrorCode::kDanglingSubjectReference,
                                "the relationship names " + relationship.value().related.to_string() +
                                    " which is not a known subject");
      }
      const bool asserted = relationship_is_asserted(observation.subject(), relationship.value().kind,
                                                     relationship.value().related);
      if (kind == EventKind::kRelationshipAsserted && asserted) {
        return conflict(ErrorCode::kIllegalLifecycleTransition,
                        "that relationship is already asserted and has not been retracted");
      }
      if (kind == EventKind::kRelationshipRetracted && !asserted) {
        return conflict(ErrorCode::kIllegalLifecycleTransition,
                        "that relationship is not currently asserted");
      }
      break;
    }
    case EventKind::kReconciliationRecorded:
    case EventKind::kCorrectionRecorded: {
      if (!subject_is_known(observation.subject())) {
        return not_found(ErrorCode::kSubjectNotRegistered,
                         "subject " + observation.subject().to_string() + " is not a known subject");
      }
      if (kind == EventKind::kCorrectionRecorded) {
        if (!observation.correction_target().has_value()) {
          return invalid_argument(ErrorCode::kMissingRequiredAttribute,
                                  "a correction must name the committed event it corrects");
        }
        if (!event_exists(*observation.correction_target())) {
          return not_found(ErrorCode::kCorrectionTargetNotFound,
                           "the correction target is not a committed event of this ledger");
        }
      }
      break;
    }
    case EventKind::kPolicyAttested: {
      const std::optional<bool> live = subject_is_live(observation.subject());
      if (!live.has_value()) {
        return not_found(ErrorCode::kSubjectNotRegistered,
                         "subject " + observation.subject().to_string() + " is not registered");
      }
      if (!live.value()) {
        return conflict(ErrorCode::kSubjectRetired,
                        "subject " + observation.subject().to_string() + " is retired");
      }
      break;
    }
  }

  // Producer incarnation fencing: a superseded incarnation of a component must
  // not be able to publish after it restarted.
  const std::vector<std::uint8_t> source_key(observation.provenance().source().view().begin(),
                                             observation.provenance().source().view().end());
  const std::vector<Posting>* source_postings = index_.postings(IndexKind::kSource, source_key);
  if (source_postings != nullptr && !source_postings->empty()) {
    const Posting& watermark = source_postings->back();
    const auto generation = observation.provenance().source_generation().value();
    if (generation < watermark.extra) {
      return stale_source(ErrorCode::kStaleSourceGeneration,
                          "source " + observation.provenance().source().str() +
                              " presented incarnation " + std::to_string(generation) +
                              " but incarnation " + std::to_string(watermark.extra) +
                              " has already published to this ledger");
    }
    if (generation == watermark.extra && observation.provenance().source_sequence().has_value() &&
        watermark.aux != 0 && observation.provenance().source_sequence()->value() <= watermark.aux) {
      return stale_source(ErrorCode::kSourceSequenceRegression,
                          "source " + observation.provenance().source().str() +
                              " presented source sequence " +
                              std::to_string(observation.provenance().source_sequence()->value()) +
                              " after " + std::to_string(watermark.aux) + " was already recorded");
    }
  } else if (index_.distinct_keys(IndexKind::kSource) >= options_.max_sources) {
    return capacity_exceeded(ErrorCode::kCapacityLimitExceeded,
                             "the ledger already tracks the configured maximum number of producing "
                             "components");
  }
  return Status::ok();
}

// -- Planning -----------------------------------------------------------------

Status LedgerImpl::plan_observation(const SubmittedObservation& observation,
                                    std::uint64_t sequence,
                                    std::uint64_t logical_tick,
                                    const PendingIdentities* pending,
                                    PlannedEvent& planned) const {
  if (observation.payload().size() > options_.max_payload_bytes) {
    return capacity_exceeded(ErrorCode::kSizeOutOfRange,
                             "the payload is " + std::to_string(observation.payload().size()) +
                                 " bytes and the configured maximum is " +
                                 std::to_string(options_.max_payload_bytes));
  }
  if (Status status = payload::validate(observation.kind(), observation.payload_schema(),
                                        observation.payload_schema_version(), observation.payload());
      status.is_error()) {
    return status;
  }

  // Duplicate resolution precedes admission policy. A retry of an
  // already-committed event must be answered idempotently even though its
  // generation, epoch or source watermark has since moved on; otherwise a
  // legitimate retry would be rejected as stale.
  if (auto existing = index_.event_sequence(observation.event_id()); existing.has_value()) {
    auto recorded = read_committed(*existing);
    if (!recorded.has_value()) {
      return recorded.status();
    }
    if (recorded->content_digest() != observation.content_digest()) {
      return conflict(ErrorCode::kDuplicateEventId,
                      "event identity " + observation.event_id().to_hex() +
                          " is already committed with different content at sequence " +
                          std::to_string(existing->value()));
    }
    planned.duplicate = true;
    planned.sequence = existing->value();
    return Status::ok();
  }

  if (observation.idempotency_token().has_value()) {
    if (auto existing = index_.token_sequence(*observation.idempotency_token()); existing.has_value()) {
      auto recorded = read_committed(*existing);
      if (!recorded.has_value()) {
        return recorded.status();
      }
      if (recorded->content_digest() != observation.content_digest()) {
        return conflict(ErrorCode::kDuplicateIdempotencyToken,
                        "idempotency token " + observation.idempotency_token()->to_hex() +
                            " is already committed with different content at sequence " +
                            std::to_string(existing->value()));
      }
      planned.duplicate = true;
      planned.sequence = existing->value();
      return Status::ok();
    }
  }

  if (pending != nullptr) {
    if (pending->event_ids.find(observation.event_id()) != pending->event_ids.end()) {
      return conflict(ErrorCode::kDuplicateEventId,
                      "event identity " + observation.event_id().to_hex() +
                          " appears more than once in the same batch");
    }
    if (observation.idempotency_token().has_value() &&
        pending->tokens.find(*observation.idempotency_token()) != pending->tokens.end()) {
      return conflict(ErrorCode::kDuplicateIdempotencyToken,
                      "idempotency token " + observation.idempotency_token()->to_hex() +
                          " appears more than once in the same batch");
    }
  }

  if (Status status = admit(observation); status.is_error()) {
    return status;
  }
  if (auto reject = index_.check_event_identity(observation.event_id()); reject.has_value()) {
    return capacity_exceeded(reject->code, reject->message);
  }
  if (observation.idempotency_token().has_value()) {
    if (auto reject = index_.check_token_identity(*observation.idempotency_token()); reject.has_value()) {
      return capacity_exceeded(reject->code, reject->message);
    }
  }
  {
    const std::vector<std::uint8_t> key = subject_key(observation.subject());
    if (auto reject = index_.check_posting_capacity(IndexKind::kSubject, key); reject.has_value()) {
      return capacity_exceeded(reject->code, reject->message);
    }
  }
  {
    const std::vector<std::uint8_t> key(observation.provenance().source().view().begin(),
                                        observation.provenance().source().view().end());
    if (auto reject = index_.check_posting_capacity(IndexKind::kSource, key); reject.has_value()) {
      return capacity_exceeded(reject->code, reject->message);
    }
  }

  // The encoded record must fit inside one segment; a single event larger than a
  // whole segment could never be committed with the ordering this format uses.
  if (kFrameOverhead + observation.payload().size() + 1024 > options_.max_segment_bytes - kSegmentHeaderSize) {
    return capacity_exceeded(ErrorCode::kCapacityLimitExceeded,
                             "the encoded record would not fit in one segment; raise max_segment_bytes or "
                             "reduce the payload size");
  }

  planned.sequence = sequence;
  planned.logical_tick = logical_tick;
  planned.monotonic_nanos = clock_->monotonic_nanoseconds();
  return Status::ok();
}

Status LedgerImpl::plan_batch(std::span<const SubmittedObservation> observations,
                              std::vector<PlannedEvent>& planned,
                              std::size_t& duplicate_count) const {
  PendingIdentities pending;
  planned.clear();
  planned.reserve(observations.size());
  duplicate_count = 0;

  std::uint64_t sequence = manifest_.committed_sequence;
  std::uint64_t logical_tick = manifest_.committed_logical_tick;

  for (const SubmittedObservation& observation : observations) {
    if (sequence == std::numeric_limits<std::uint64_t>::max() ||
        logical_tick == std::numeric_limits<std::uint64_t>::max()) {
      return capacity_exceeded(ErrorCode::kArithmeticOverflow, "the ledger sequence counter is exhausted");
    }
    PlannedEvent entry(observation);
    if (Status status = plan_observation(observation, sequence + 1, logical_tick + 1, &pending, entry);
        status.is_error()) {
      return status;
    }
    if (entry.duplicate) {
      ++duplicate_count;
    } else {
      ++sequence;
      ++logical_tick;
      pending.event_ids.emplace(observation.event_id(), planned.size());
      if (observation.idempotency_token().has_value()) {
        pending.tokens.emplace(*observation.idempotency_token(), planned.size());
      }
    }
    planned.push_back(std::move(entry));
  }
  return Status::ok();
}

// -- Commit -------------------------------------------------------------------

Status LedgerImpl::verify_manifest_authority() const {
  if (!manifest_authority_trusted_) {
    // The published manifest is known to be unusable; the publication that
    // replaces it is the repair, so it must not be blocked by what it repairs.
    return Status::ok();
  }
  auto present = path_exists(paths_.manifest());
  if (!present.has_value()) {
    return present.status();
  }
  if (!present.value()) {
    // No publication exists yet, so there is nothing that could disagree with
    // this handle. This is the first publication of a new ledger.
    return Status::ok();
  }
  auto reader = File::open_read(paths_.manifest());
  if (!reader.has_value()) {
    return reader.status();
  }
  File handle = std::move(reader).value();
  std::uint8_t raw[24] = {};
  auto read = handle.read_at(0, std::span<std::uint8_t>(raw, sizeof(raw)));
  if (!read.has_value()) {
    return read.status();
  }
  if (read.value() < sizeof(raw)) {
    return integrity_failure(ErrorCode::kManifestAheadOfLog,
                             "the published manifest became shorter than a manifest header");
  }
  std::uint64_t published = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    published |= static_cast<std::uint64_t>(raw[16 + i]) << (8u * i);
  }
  if (published != manifest_.manifest_generation) {
    return conflict(ErrorCode::kConcurrentModification,
                    "the published manifest generation is " + std::to_string(published) +
                        " but this handle last published " +
                        std::to_string(manifest_.manifest_generation) +
                        "; another writer has modified this ledger");
  }
  return Status::ok();
}

Status LedgerImpl::publish_manifest(bool check_authority) {
  Manifest next = manifest_;
  next.manifest_generation = manifest_.manifest_generation + 1;
  next.flags = durable_ ? kManifestFlagDurable : 0;
  next.active_segment_offset = (next.committed_segment_index == next.active_segment_index)
                                   ? next.committed_offset
                                   : kSegmentHeaderSize;
  const std::vector<std::uint8_t> bytes = encode_manifest(next);

  // The exclusive writer lock is the primary exclusion mechanism. Re-reading the
  // published generation is a secondary detector for a writer that ignored the
  // lock; it is sampled rather than performed on every commit because it costs
  // an extra open. It always runs on structural publications.
  if (check_authority) {
    if (Status status = verify_manifest_authority(); status.is_error()) {
      return status;
    }
  } else if (stats_.durable_commits == 0) {
    if (Status status = verify_manifest_authority(); status.is_error()) {
      return status;
    }
  }

  {
    auto staged = File::open_write_truncate(paths_.manifest_temp());
    if (!staged.has_value()) {
      return staged.status();
    }
    File handle = std::move(staged).value();
    if (Status status = handle.append(bytes); status.is_error()) {
      return status;
    }
    if (durable_) {
      if (Status status = handle.flush(); status.is_error()) {
        return status;
      }
      stats_.flush_calls += 1;
    }
    handle.close();
  }

  inject(Boundary::kBeforeManifestPublish);

  if (Status status = atomic_replace(paths_.manifest_temp(), paths_.manifest(), paths_.manifest_backup());
      status.is_error()) {
    return status;
  }
  if (durable_ && options_.flush_directory) {
    if (Status status = flush_directory(root_); status.is_error()) {
      return status;
    }
  }

  manifest_ = next;
  manifest_generation_ = ManifestGeneration(manifest_.manifest_generation);
  manifest_authority_trusted_ = true;
  inject(Boundary::kAfterManifestPublish);
  return Status::ok();
}

std::vector<std::uint8_t> LedgerImpl::encode_envelope_body(const PlannedEvent& entry) const {
  const SubmittedObservation& observation = entry.observation;
  EventEnvelope envelope(LedgerSequence(entry.sequence),
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
                         AcceptedAt{LogicalTick(entry.logical_tick), entry.monotonic_nanos},
                         observation.content_digest(),
                         Digest{});
  return encode_envelope(envelope);
}

Result<EventEnvelope> LedgerImpl::make_envelope(const PlannedEvent& entry) const {
  const SubmittedObservation& observation = entry.observation;
  const std::vector<std::uint8_t> body = encode_envelope_body(entry);
  return EventEnvelope(LedgerSequence(entry.sequence),
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
                       AcceptedAt{LogicalTick(entry.logical_tick), entry.monotonic_nanos},
                       observation.content_digest(),
                       Sha256::hash(body.data(), body.size()));
}

Status LedgerImpl::commit_planned(const std::vector<PlannedEvent>& planned,
                                  std::vector<EventEnvelope>& committed,
                                  std::size_t& duplicate_count) {
  std::vector<const PlannedEvent*> fresh;
  std::vector<std::size_t> fresh_positions;
  fresh.reserve(planned.size());
  fresh_positions.reserve(planned.size());
  for (std::size_t i = 0; i < planned.size(); ++i) {
    if (!planned[i].duplicate) {
      fresh.push_back(&planned[i]);
      fresh_positions.push_back(i);
    }
  }

  if (fresh.empty()) {
    for (const PlannedEvent& entry : planned) {
      auto recorded = read_committed(LedgerSequence(entry.sequence));
      if (!recorded.has_value()) {
        return recorded.status();
      }
      committed.push_back(std::move(recorded).value());
    }
    duplicate_count = planned.size();
    return Status::ok();
  }

  // 1. Encode every body before anything is written, so that no failure can
  //    leave a partially written batch behind.
  std::vector<std::vector<std::uint8_t>> bodies;
  bodies.reserve(fresh.size());
  std::uint64_t total_bytes = 0;
  for (const PlannedEvent* entry : fresh) {
    bodies.push_back(encode_envelope_body(*entry));
    total_bytes += kFrameOverhead + bodies.back().size();
  }
  const std::uint64_t segment_capacity = options_.max_segment_bytes - kSegmentHeaderSize;
  for (const std::vector<std::uint8_t>& body : bodies) {
    if (kFrameOverhead + body.size() > segment_capacity) {
      return capacity_exceeded(
          ErrorCode::kCapacityLimitExceeded,
          "a single encoded record does not fit in one segment; raise max_segment_bytes or reduce the "
          "payload size");
    }
  }

  // 2. Rotate first when the batch does not fit, so that a batch is never split
  //    across a segment boundary.
  const bool active_is_empty = active_offset_ == kSegmentHeaderSize && active_record_index_ == 0;
  if (!active_is_empty && active_offset_ - kSegmentHeaderSize + total_bytes > segment_capacity) {
    if (Status status = rotate_locked(); status.is_error()) {
      return status;
    }
  }

  inject(Boundary::kAfterEncode);

  // 3. Encode the frames against the segment's chain.
  std::vector<std::uint8_t> wire;
  wire.reserve(static_cast<std::size_t>(total_bytes));
  Digest chain = active_chain_;
  std::uint64_t record_index = active_record_index_;
  std::vector<std::uint64_t> offsets;
  offsets.reserve(fresh.size());
  for (std::size_t i = 0; i < fresh.size(); ++i) {
    offsets.push_back(active_offset_ + wire.size());
    ++record_index;
    const std::size_t frame_begin = wire.size();
    encode_frame(FrameKind::kEvent, record_index, fresh[i]->sequence, fresh[i]->logical_tick, bodies[i],
                 chain, wire);
    chain = frame_chain_of(std::span<const std::uint8_t>(wire).subspan(frame_begin));
  }
  const std::uint64_t new_end_offset = active_offset_ + wire.size();
  const std::uint64_t new_record_index = record_index;

  // 4. Write and flush the segment. Until the manifest names these frames they
  //    are an unacknowledged tail, not committed state.
  if (Status status = active_file_.append(wire); status.is_error()) {
    return status;
  }
  inject(Boundary::kAfterSegmentWrite);

  if (durable_) {
    if (Status status = active_file_.flush(); status.is_error()) {
      return status;
    }
    stats_.flush_calls += 1;
  }
  inject(Boundary::kAfterSegmentFlush);

  // 5. Publish the commit watermark. This is the acknowledgement boundary.
  const std::uint64_t new_count = manifest_.total_event_count + fresh.size();
  set_committed_state(fresh.back()->sequence, fresh.back()->logical_tick, manifest_.active_segment_index,
                      new_end_offset, new_record_index, chain, new_count);
  if (Status status = publish_manifest(true); status.is_error()) {
    return status;
  }

  // 6. Bring derived state forward. Everything below is derived and rebuildable.
  //    Results are collected per planned event so that they are returned in the
  //    order the caller submitted them, duplicates included.
  std::vector<std::optional<EventEnvelope>> ordered(planned.size());
  std::size_t ordered_index = 0;
  events_since_checkpoint_ += fresh.size();
  for (std::size_t i = 0; i < fresh.size(); ++i) {
    auto envelope = make_envelope(*fresh[i]);
    if (!envelope.has_value()) {
      return envelope.status();
    }
    const std::vector<IndexEntry> entries = index_entries_for(envelope.value());
    for (const IndexEntry& index_entry : entries) {
      if (index_entry.kind == IndexKind::kEventId) {
        if (auto reject = index_.add_identity(envelope->event_id(), envelope->sequence()); reject.has_value()) {
          return capacity_exceeded(reject->code, reject->message);
        }
      } else if (index_entry.kind == IndexKind::kIdempotency) {
        if (auto reject = index_.add_token(*envelope->idempotency_token(), envelope->sequence());
            reject.has_value()) {
          return capacity_exceeded(reject->code, reject->message);
        }
      }
      if (auto reject = index_.add_posting(index_entry.kind, index_entry.key,
                                           Posting{index_entry.sequence, index_entry.extra, index_entry.aux});
          reject.has_value()) {
        return capacity_exceeded(reject->code, reject->message);
      }
      fold_entry_into_derived(index_entry);
    }
    if (Status status = append_index_entries(entries); status.is_error()) {
      return status;
    }
    ordered[fresh_positions[i]] = std::move(envelope).value();
    ++ordered_index;
  }

  {
    std::unique_lock<std::shared_mutex> guard(segments_mutex_);
    for (const auto& descriptor : segments_) {
      if (descriptor->index == manifest_.active_segment_index) {
        descriptor->frame_offsets.insert(descriptor->frame_offsets.end(), offsets.begin(), offsets.end());
        resident_frame_offsets_ += offsets.size();
        descriptor->frame_count.store(descriptor->frame_offsets.size(), std::memory_order_release);
        descriptor->end_sequence.store(fresh.back()->sequence, std::memory_order_release);
        descriptor->end_offset.store(new_end_offset, std::memory_order_release);
        break;
      }
    }
  }

  active_offset_ = new_end_offset;
  active_record_index_ = new_record_index;
  active_chain_ = chain;

  for (std::size_t i = 0; i < planned.size(); ++i) {
    if (!ordered[i].has_value()) {
      auto recorded = read_committed(LedgerSequence(planned[i].sequence));
      if (!recorded.has_value()) {
        return recorded.status();
      }
      ordered[i] = std::move(recorded).value();
    }
  }
  committed.clear();
  committed.reserve(planned.size());
  for (std::optional<EventEnvelope>& entry : ordered) {
    committed.push_back(std::move(entry).value());
  }

  duplicate_count = planned.size() - fresh.size();
  stats_.durable_commits += 1;
  stats_.committed_events = new_count;
  stats_.committed_frames += fresh.size();

  if (options_.auto_checkpoint_interval != 0 &&
      events_since_checkpoint_ >= options_.auto_checkpoint_interval) {
    if (Status status = flush_index(true); status.is_error()) {
      return status;
    }
    auto checkpoint = create_checkpoint_locked();
    if (!checkpoint.has_value()) {
      note(checkpoint.status().code(),
           "the automatic checkpoint was skipped: " + checkpoint.status().message());
      events_since_checkpoint_ = 0;
    }
  }

  inject(Boundary::kAfterCommitAck);
  return Status::ok();
}

// -- Public mutation surface --------------------------------------------------

Result<AppendOutcome> LedgerImpl::append(const SubmittedObservation& observation) {
  auto batch = append_batch(std::span<const SubmittedObservation>(&observation, 1));
  if (!batch.has_value()) {
    return batch.status();
  }
  const bool duplicate = batch.value().duplicates == 1;
  return AppendOutcome(std::move(batch.value().events.front()), duplicate);
}

Result<BatchOutcome> LedgerImpl::append_batch(std::span<const SubmittedObservation> observations) {
  if (observations.empty()) {
    return invalid_argument(ErrorCode::kBatchEmpty, "an atomic batch must hold at least one submission");
  }
  if (observations.size() > options_.max_batch_events) {
    return capacity_exceeded(ErrorCode::kBatchTooLarge,
                             "the batch holds " + std::to_string(observations.size()) +
                                 " submissions and the configured maximum is " +
                                 std::to_string(options_.max_batch_events));
  }

  std::lock_guard<std::mutex> guard(mutex_);
  if (closed_.load(std::memory_order_acquire)) {
    return closed(ErrorCode::kLedgerClosed, "the handle is closed");
  }
  if (!writable_) {
    return read_only(ErrorCode::kReadOnlyHandle, "this handle was opened read-only");
  }

  std::vector<PlannedEvent> planned;
  std::size_t duplicate_count = 0;
  if (Status status = plan_batch(observations, planned, duplicate_count); status.is_error()) {
    stats_.rejected_appends += observations.size();
    return status;
  }

  BatchOutcome outcome;
  if (Status status = commit_planned(planned, outcome.events, duplicate_count); status.is_error()) {
    stats_.rejected_appends += observations.size();
    return status;
  }
  outcome.duplicates = duplicate_count;
  outcome.nothing_written = duplicate_count == planned.size();
  stats_.duplicate_appends += duplicate_count;
  return outcome;
}

// -- Rotation, index flush, checkpoints, lifecycle ----------------------------

Status LedgerImpl::open_segment(std::uint64_t segment_index, std::uint64_t base_sequence) {
  const std::filesystem::path path = paths_.segment(SegmentIndex(segment_index));
  auto created = File::create_exclusive(path);
  if (!created.has_value()) {
    // A leftover file from an interrupted rotation is reused once it is known to
    // hold no frames.
    auto existing = File::open_read_write(path);
    if (!existing.has_value()) {
      return created.status();
    }
    File handle = std::move(existing).value();
    auto size = handle.size();
    if (!size.has_value()) {
      return size.status();
    }
    if (size.value() > kSegmentHeaderSize) {
      return integrity_failure(ErrorCode::kSegmentOutOfOrder,
                               "refusing to reuse " + path.filename().string() +
                                   " because it already holds frames");
    }
    if (Status status = handle.truncate(0); status.is_error()) {
      return status;
    }
    active_file_ = std::move(handle);
  } else {
    active_file_ = std::move(created).value();
  }

  SegmentHeader header;
  header.format_version = kSegmentFormatVersion;
  header.ledger_id = manifest_.ledger_id;
  header.segment_index = segment_index;
  header.base_sequence = base_sequence;
  header.created_unix_nanos = options_.segment_timestamp_unix_nanos.has_value()
                                  ? *options_.segment_timestamp_unix_nanos
                                  : clock_->unix_nanoseconds();
  if (Status status = active_file_.append(encode_segment_header(header)); status.is_error()) {
    return status;
  }
  if (durable_) {
    if (Status status = active_file_.flush(); status.is_error()) {
      return status;
    }
    stats_.flush_calls += 1;
  }

  auto reader = File::open_read(path);
  if (!reader.has_value()) {
    return reader.status();
  }
  auto descriptor =
      std::make_shared<SegmentDescriptor>(segment_index, base_sequence, kSegmentHeaderSize,
                                          std::move(reader).value());
  {
    std::unique_lock<std::shared_mutex> guard(segments_mutex_);
    segments_.push_back(descriptor);
  }
  manifest_.segment_count = segment_index;
  manifest_.active_segment_index = segment_index;
  active_header_ = header;
  active_offset_ = kSegmentHeaderSize;
  active_record_index_ = 0;
  active_first_sequence_ = base_sequence;
  active_chain_ = segment_chain_seed(header);
  stats_.segment_count = manifest_.segment_count;
  return Status::ok();
}

Status LedgerImpl::seal_active_segment() {
  SegmentSealBody seal;
  seal.record_count = active_record_index_;
  seal.first_sequence = active_record_index_ == 0 ? 0 : active_first_sequence_;
  seal.last_sequence = active_record_index_ == 0 ? 0 : active_first_sequence_ + active_record_index_ - 1;
  seal.sealed_offset = active_offset_;
  const std::vector<std::uint8_t> body = encode_segment_seal(seal);
  scratch_frame_.clear();
  encode_frame(FrameKind::kSegmentSeal, active_record_index_ + 1, 0, 0, body, active_chain_, scratch_frame_);
  if (Status status = active_file_.append(scratch_frame_); status.is_error()) {
    return status;
  }
  if (durable_) {
    if (Status status = active_file_.flush(); status.is_error()) {
      return status;
    }
    stats_.flush_calls += 1;
  }
  inject(Boundary::kAfterRotateSealWrite);
  {
    std::unique_lock<std::shared_mutex> guard(segments_mutex_);
    for (const auto& descriptor : segments_) {
      if (descriptor->index == manifest_.active_segment_index) {
        descriptor->sealed.store(true, std::memory_order_release);
        descriptor->end_offset.store(active_offset_ + scratch_frame_.size(), std::memory_order_release);
        break;
      }
    }
  }
  return Status::ok();
}

Status LedgerImpl::rotate_locked() {
  if (manifest_.active_segment_index == std::numeric_limits<std::uint64_t>::max()) {
    return capacity_exceeded(ErrorCode::kArithmeticOverflow, "the segment index counter is exhausted");
  }
  if (Status status = flush_index(true); status.is_error()) {
    return status;
  }
  if (Status status = seal_active_segment(); status.is_error()) {
    return status;
  }
  const std::uint64_t next_base =
      active_record_index_ == 0 ? active_first_sequence_ : active_first_sequence_ + active_record_index_;
  if (Status status = open_segment(manifest_.active_segment_index + 1, next_base); status.is_error()) {
    return status;
  }
  return publish_manifest(true);
}

Result<void> LedgerImpl::rotate_segment() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (closed_.load(std::memory_order_acquire)) {
    return closed(ErrorCode::kLedgerClosed, "the handle is closed");
  }
  if (!writable_) {
    return read_only(ErrorCode::kReadOnlyHandle, "this handle was opened read-only");
  }
  return rotate_locked();
}

Status LedgerImpl::flush_index(bool force) {
  if (!writable_ || !index_file_.valid()) {
    return Status::ok();
  }
  if (!index_file_dirty_ && !force) {
    return Status::ok();
  }
  const IndexSealBody seal{index_entry_count_, manifest_.committed_sequence, index_generation_};
  scratch_frame_.clear();
  encode_frame(FrameKind::kIndexSeal, index_entry_count_ + 1, 0, 0, encode_index_seal(seal), Digest{},
               scratch_frame_);
  if (Status status = index_file_.append(scratch_frame_); status.is_error()) {
    return status;
  }
  if (durable_) {
    if (Status status = index_file_.flush(); status.is_error()) {
      return status;
    }
    stats_.flush_calls += 1;
  }
  index_durable_through_ = manifest_.committed_sequence;
  index_file_dirty_ = false;
  return Status::ok();
}

Status LedgerImpl::write_checkpoint_file(const Checkpoint& checkpoint) {
  const std::filesystem::path final_path = paths_.checkpoint(LedgerSequence(checkpoint.sequence));
  std::filesystem::path temporary = final_path;
  temporary += ".tmp";
  {
    auto staged = File::open_write_truncate(temporary);
    if (!staged.has_value()) {
      return staged.status();
    }
    File handle = std::move(staged).value();
    if (Status status = handle.append(encode_checkpoint(checkpoint)); status.is_error()) {
      return status;
    }
    if (durable_) {
      if (Status status = handle.flush(); status.is_error()) {
        return status;
      }
      stats_.flush_calls += 1;
    }
    handle.close();
  }
  if (Status status = atomic_replace(temporary, final_path, std::nullopt); status.is_error()) {
    return status;
  }
  if (durable_ && options_.flush_directory) {
    return flush_directory(paths_.checkpoints_directory());
  }
  return Status::ok();
}

Status LedgerImpl::prune_checkpoints() {
  auto retained = checkpoints();
  if (!retained.has_value()) {
    return retained.status();
  }
  if (retained.value().size() <= options_.max_checkpoints) {
    return Status::ok();
  }
  const std::size_t excess = retained.value().size() - options_.max_checkpoints;
  for (std::size_t i = 0; i < excess; ++i) {
    const std::filesystem::path path = paths_.checkpoints_directory() / retained.value()[i].file_name;
    if (Status status = remove_file(path); status.is_error()) {
      note(status.code(), "could not remove superseded checkpoint " + retained.value()[i].file_name);
    }
  }
  return Status::ok();
}

Result<CheckpointInfo> LedgerImpl::create_checkpoint() {
  std::lock_guard<std::mutex> guard(mutex_);
  return create_checkpoint_locked();
}

Result<CheckpointInfo> LedgerImpl::create_checkpoint_locked() {
  if (closed_.load(std::memory_order_acquire)) {
    return closed(ErrorCode::kLedgerClosed, "the handle is closed");
  }
  if (!writable_) {
    return read_only(ErrorCode::kReadOnlyHandle, "this handle was opened read-only");
  }
  if (manifest_.committed_sequence == 0) {
    return not_found(ErrorCode::kNotFound, "the ledger holds no committed events to checkpoint");
  }

  Checkpoint checkpoint;
  checkpoint.ledger_id = manifest_.ledger_id;
  checkpoint.sequence = manifest_.committed_sequence;
  checkpoint.logical_tick = manifest_.committed_logical_tick;
  checkpoint.segment_index = manifest_.committed_segment_index;
  checkpoint.offset = manifest_.committed_offset;
  checkpoint.chain_at_sequence = manifest_.committed_chain;
  checkpoint.manifest_generation = manifest_.manifest_generation;
  checkpoint.facility_generation = derived_.facility_generation;
  checkpoint.open_epoch = derived_.open_epoch;
  checkpoint.latest_epoch = derived_.latest_epoch;
  checkpoint.event_count = manifest_.total_event_count;
  checkpoint.created_unix_nanos = options_.segment_timestamp_unix_nanos.has_value()
                                      ? *options_.segment_timestamp_unix_nanos
                                      : clock_->unix_nanoseconds();

  const std::filesystem::path path = paths_.checkpoint(LedgerSequence(checkpoint.sequence));
  auto present = path_exists(path);
  if (!present.has_value()) {
    return present.status();
  }
  if (!present.value()) {
    if (Status status = write_checkpoint_file(checkpoint); status.is_error()) {
      return status;
    }
    if (manifest_.checkpoint_count != std::numeric_limits<std::uint64_t>::max()) {
      manifest_.checkpoint_count += 1;
    }
  }
  manifest_.latest_checkpoint_sequence = checkpoint.sequence;
  if (Status status = publish_manifest(true); status.is_error()) {
    return status;
  }
  if (Status status = prune_checkpoints(); status.is_error()) {
    return status;
  }
  events_since_checkpoint_ = 0;

  return CheckpointInfo{LedgerSequence(checkpoint.sequence),
                        LogicalTick(checkpoint.logical_tick),
                        SegmentIndex(checkpoint.segment_index),
                        checkpoint.offset,
                        checkpoint.chain_at_sequence,
                        checkpoint.event_count,
                        checkpoint.created_unix_nanos,
                        path.filename().string()};
}

Result<void> LedgerImpl::flush() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (closed_.load(std::memory_order_acquire)) {
    return closed(ErrorCode::kLedgerClosed, "the handle is closed");
  }
  if (!writable_) {
    return Status::ok();
  }
  if (Status status = flush_index(true); status.is_error()) {
    return status;
  }
  manifest_.index_through_sequence = manifest_.committed_sequence;
  return publish_manifest(true);
}

Result<void> LedgerImpl::rebuild_index() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (closed_.load(std::memory_order_acquire)) {
    return closed(ErrorCode::kLedgerClosed, "the handle is closed");
  }
  if (!writable_) {
    return read_only(ErrorCode::kReadOnlyHandle, "this handle was opened read-only");
  }
  return rebuild_index_from_log(&open_report_);
}

Result<void> LedgerImpl::close() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (closed_.load(std::memory_order_acquire)) {
    return Status::ok();
  }
  if (writable_) {
    if (Status status = flush_index(true); status.is_error()) {
      closed_.store(true, std::memory_order_release);
      return status;
    }
  }
  active_file_.close();
  index_file_.close();
  {
    std::unique_lock<std::shared_mutex> segment_guard(segments_mutex_);
    segments_.clear();
  }
  writer_lock_.release();
  closed_.store(true, std::memory_order_release);
  return Status::ok();
}

RecoveryReport LedgerImpl::recover(const RecoveryRequest& request) {
  RecoveryReport report;
  report.dry_run = request.dry_run;
  std::lock_guard<std::mutex> guard(mutex_);
  if (closed_.load(std::memory_order_acquire)) {
    report.status = closed(ErrorCode::kLedgerClosed, "the handle is closed");
    return report;
  }
  if (!writable_) {
    report.status = read_only(ErrorCode::kReadOnlyHandle,
                              mode_ == OpenMode::kDiagnose
                                  ? "diagnostic handles never mutate; reopen read-write to recover"
                                  : "this handle was opened read-only");
    return report;
  }
  if (request.dry_run) {
    // A dry run reports exactly what a real recovery would do without changing
    // anything: the committed prefix is re-validated and the unacknowledged tail
    // is measured.
    if (Status status = validate_committed_prefix(true, false); status.is_error()) {
      report.status = status;
      return report;
    }
    report.truncated_tail_bytes = open_report_.truncated_tail_bytes;
    report.truncated_tail_frames = open_report_.truncated_tail_frames;
    report.truncated = report.truncated_tail_bytes > 0;
    report.manifest_recovered_from_backup = open_report_.manifest_recovered_from_backup;
    report.notes = open_report_.notes;
    return report;
  }

  if (Status status = flush_index(true); status.is_error()) {
    report.status = status;
    return report;
  }
  if (Status status = validate_committed_prefix(true, true); status.is_error()) {
    report.status = status;
    return report;
  }
  inject(Boundary::kAfterRecoveryScan);

  report.truncated_tail_bytes = open_report_.truncated_tail_bytes;
  report.truncated_tail_frames = open_report_.truncated_tail_frames;
  report.truncated = report.truncated_tail_bytes > 0;

  if (request.rebuild_index || report.truncated) {
    if (Status status = rebuild_index_from_log(&open_report_); status.is_error()) {
      report.status = status;
      return report;
    }
    report.index_rebuilt = true;
  }
  report.manifest_recovered_from_backup = open_report_.manifest_recovered_from_backup;
  report.replayed_events = open_report_.replayed_events;
  report.notes = open_report_.notes;
  return report;
}

}  // namespace fsl::detail

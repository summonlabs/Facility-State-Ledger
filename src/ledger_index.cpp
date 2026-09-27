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

/// \file ledger_index.cpp
/// The derived index: loading it, bringing it forward from the authoritative
/// log, and rebuilding it.
///
/// The index is never authoritative. Every posting is (kind, key, sequence), and
/// a posting only ever narrows a search: the record it names is re-read from the
/// log and re-validated before it reaches a caller. The index therefore cannot
/// contradict the log, and discarding any part of it is always safe.

namespace fsl::detail {
namespace {

constexpr std::size_t kIndexReadChunk = 256u * 1024u;
constexpr std::size_t kMaxIndexFiles = 4096;

void store_u64_le(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

[[nodiscard]] std::uint64_t load_u64_le(std::span<const std::uint8_t> bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes.size() && i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (8u * i);
  }
  return value;
}

/// Forward frame reader over one file, used for index segments.
class RawFrameStream {
 public:
  explicit RawFrameStream(File* file) : file_(file) {}

  /// Reads the next frame. Returns kIncomplete at a clean end of file and for a
  /// trailing partial frame.
  [[nodiscard]] FrameDecodeOutcome next(DecodedFrame& out) {
    while (true) {
      if (offset_ >= limit_) {
        return FrameDecodeOutcome::kIncomplete;
      }
      const bool buffered =
          offset_ >= buffer_base_ && (offset_ - buffer_base_) + kFrameOverhead <= buffer_length_;
      if (!buffered) {
        const auto want =
            static_cast<std::size_t>(std::min<std::uint64_t>(limit_ - offset_, kIndexReadChunk));
        if (want < kFrameOverhead) {
          return FrameDecodeOutcome::kIncomplete;
        }
        buffer_.assign(want, 0);
        auto read = file_->read_at(offset_, buffer_);
        if (!read.has_value() || read.value() != want) {
          return FrameDecodeOutcome::kIncomplete;
        }
        buffer_base_ = offset_;
        buffer_length_ = read.value();
      }
      const std::size_t within = static_cast<std::size_t>(offset_ - buffer_base_);
      const std::span<const std::uint8_t> available(buffer_.data() + within, buffer_length_ - within);
      const FrameDecodeOutcome outcome = decode_frame(available, out);
      if (outcome == FrameDecodeOutcome::kIncomplete && buffer_base_ + buffer_length_ < limit_) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(
            limit_ - offset_, std::max<std::uint64_t>(buffer_length_ * 2, kIndexReadChunk)));
        buffer_.assign(want, 0);
        auto read = file_->read_at(offset_, buffer_);
        if (!read.has_value() || read.value() != want) {
          return FrameDecodeOutcome::kIncomplete;
        }
        buffer_base_ = offset_;
        buffer_length_ = read.value();
        continue;
      }
      if (outcome == FrameDecodeOutcome::kOk) {
        frame_offset_ = offset_;
        offset_ += out.header.frame_size;
      }
      return outcome;
    }
  }

  void set_limit(std::uint64_t limit) noexcept { limit_ = limit; }
  [[nodiscard]] std::uint64_t frame_offset() const noexcept { return frame_offset_; }

 private:
  File* file_;
  std::uint64_t offset_ = 0;
  std::uint64_t limit_ = 0;
  std::uint64_t frame_offset_ = 0;
  std::uint64_t buffer_base_ = 0;
  std::size_t buffer_length_ = 0;
  std::vector<std::uint8_t> buffer_;
};

}  // namespace

EventId LedgerImpl::derive_creation_event_id(const LedgerId& identity) noexcept {
  Sha256 hasher;
  constexpr std::string_view kDomain = "fsl.ledger-opened.v1";
  hasher.update(kDomain);
  hasher.update(identity.data(), LedgerId::kByteSize);
  const Digest digest = hasher.finish();
  return EventId::from_bytes(digest.data());
}

std::vector<IndexEntry> LedgerImpl::index_entries_for(const EventEnvelope& envelope) const {
  const auto sequence = envelope.sequence().value();
  const auto kind = static_cast<std::uint64_t>(envelope.kind());
  std::vector<IndexEntry> entries;
  entries.reserve(8);

  IndexEntry identity;
  identity.kind = IndexKind::kEventId;
  identity.sequence = sequence;
  identity.extra = kind;
  const EventId& identity_id = envelope.event_id();
  identity.key.assign(identity_id.data(), identity_id.data() + EventId::kByteSize);
  entries.push_back(std::move(identity));

  if (envelope.idempotency_token().has_value()) {
    IndexEntry token;
    token.kind = IndexKind::kIdempotency;
    token.sequence = sequence;
    token.extra = kind;
    const IdempotencyToken& token_value = *envelope.idempotency_token();
    token.key.assign(token_value.data(), token_value.data() + IdempotencyToken::kByteSize);
    entries.push_back(std::move(token));
  }

  IndexEntry subject_entry;
  subject_entry.kind = IndexKind::kSubject;
  subject_entry.sequence = sequence;
  subject_entry.extra = kind;
  subject_entry.key = subject_key(envelope.subject());
  entries.push_back(std::move(subject_entry));

  IndexEntry source_entry;
  source_entry.kind = IndexKind::kSource;
  source_entry.sequence = sequence;
  source_entry.extra = envelope.provenance().source_generation().value();
  source_entry.aux = envelope.provenance().source_sequence().has_value()
                         ? envelope.provenance().source_sequence()->value()
                         : 0;
  const SourceComponentId& source_id = envelope.provenance().source();
  source_entry.key.assign(source_id.view().begin(), source_id.view().end());
  entries.push_back(std::move(source_entry));

  if (envelope.epoch().has_value()) {
    IndexEntry epoch;
    epoch.kind = IndexKind::kEpoch;
    epoch.sequence = sequence;
    epoch.extra = kind;
    store_u64_le(epoch.key, envelope.epoch()->value());
    entries.push_back(std::move(epoch));
  }

  // A generation-advanced event establishes the generation it names, so the
  // posting carries the successor. Every other kind belongs to the generation
  // its envelope names.
  std::uint64_t generation_value = envelope.facility_generation().value();
  if (envelope.kind() == EventKind::kGenerationAdvanced) {
    auto advanced = payload::decode_generation_advanced(envelope.payload(), envelope.payload_schema_version());
    if (advanced.has_value()) {
      generation_value = advanced.value().to.value();
    }
  }
  IndexEntry generation;
  generation.kind = IndexKind::kGeneration;
  generation.sequence = sequence;
  generation.extra = kind;
  store_u64_le(generation.key, generation_value);
  entries.push_back(std::move(generation));

  IndexEntry event_kind;
  event_kind.kind = IndexKind::kEventKind;
  event_kind.sequence = sequence;
  event_kind.key.push_back(static_cast<std::uint8_t>(envelope.kind()));
  entries.push_back(std::move(event_kind));

  if (envelope.kind() == EventKind::kRelationshipAsserted ||
      envelope.kind() == EventKind::kRelationshipRetracted) {
    auto relationship = payload::decode_relationship(envelope.payload(), envelope.payload_schema_version());
    if (relationship.has_value()) {
      IndexEntry entry;
      entry.kind = IndexKind::kRelationship;
      entry.sequence = sequence;
      entry.extra = kind;
      entry.key =
          relationship_key(envelope.subject(), relationship.value().kind, relationship.value().related);
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

void LedgerImpl::fold_entry_into_derived(const IndexEntry& entry) {
  switch (entry.kind) {
    case IndexKind::kEpoch: {
      const std::uint64_t epoch = load_u64_le(entry.key);
      if (entry.extra == static_cast<std::uint64_t>(EventKind::kEpochOpened)) {
        derived_.open_epoch = epoch;
        derived_.latest_epoch = std::max(derived_.latest_epoch, epoch);
      } else if (entry.extra == static_cast<std::uint64_t>(EventKind::kEpochClosed)) {
        if (derived_.open_epoch == epoch) {
          derived_.open_epoch = 0;
        }
      }
      break;
    }
    case IndexKind::kGeneration:
      derived_.facility_generation = load_u64_le(entry.key);
      break;
    default:
      break;
  }
}

void LedgerImpl::recompute_derived_from_index() {
  derived_ = DerivedState{};
  // Lifecycle state is a fold over commit order, so the postings are sorted by
  // sequence before they are applied. Iterating the posting map directly would
  // apply them in hash order and could settle on a superseded state.
  struct Dated {
    std::uint64_t sequence = 0;
    IndexKind kind = IndexKind::kEpoch;
    std::uint64_t extra = 0;
    std::vector<std::uint8_t> key;
  };
  std::vector<Dated> dated;
  for (const IndexKind kind : {IndexKind::kEpoch, IndexKind::kGeneration}) {
    for (const std::vector<std::uint8_t>& key : index_.keys(kind)) {
      const std::vector<Posting>* postings = index_.postings(kind, key);
      if (postings == nullptr || postings->empty()) {
        continue;
      }
      dated.push_back(Dated{postings->back().sequence, kind, postings->back().extra, key});
    }
  }
  std::sort(dated.begin(), dated.end(), [](const Dated& lhs, const Dated& rhs) {
    return lhs.sequence < rhs.sequence;
  });
  for (const Dated& entry : dated) {
    IndexEntry reconstruction;
    reconstruction.kind = entry.kind;
    reconstruction.key = entry.key;
    reconstruction.extra = entry.extra;
    reconstruction.sequence = entry.sequence;
    fold_entry_into_derived(reconstruction);
  }
}
Status LedgerImpl::start_index_generation(std::uint64_t generation) {
  index_file_.close();
  index_generation_ = generation;
  index_ordinal_ = 1;
  index_entry_count_ = 0;
  index_durable_through_ = 0;
  index_file_dirty_ = false;
  if (!writable_) {
    return Status::ok();
  }
  const std::filesystem::path path = paths_.index_file(index_generation_, index_ordinal_);
  auto opened = File::open_write_truncate(path);
  if (!opened.has_value()) {
    return opened.status();
  }
  index_file_ = std::move(opened).value();
  return Status::ok();
}

Status LedgerImpl::append_index_entries(const std::vector<IndexEntry>& entries) {
  scratch_frame_.clear();
  for (const IndexEntry& entry : entries) {
    encode_frame(FrameKind::kIndexEntry, index_entry_count_ + 1, entry.sequence, 0,
                 encode_index_entry(entry), Digest{}, scratch_frame_);
    ++index_entry_count_;
  }
  if (!writable_ || !index_file_.valid()) {
    return Status::ok();
  }
  if (!scratch_frame_.empty()) {
    if (Status status = index_file_.append(scratch_frame_); status.is_error()) {
      return status;
    }
    index_file_dirty_ = true;
  }
  return Status::ok();
}

Status LedgerImpl::load_index() {
  index_.clear();
  derived_ = DerivedState{};

  std::vector<std::uint64_t> ordinals;
  std::error_code error;
  if (std::filesystem::is_directory(paths_.index_directory(), error) && !error) {
    auto names = list_directory(paths_.index_directory(), kMaxIndexFiles * 4);
    if (!names.has_value()) {
      return names.status();
    }
    for (const std::string& name : names.value()) {
      std::uint64_t generation = 0;
      std::uint64_t ordinal = 0;
      if (Paths::parse_index_name(name, generation, ordinal) && generation == manifest_.index_generation) {
        ordinals.push_back(ordinal);
      }
    }
  }
  std::sort(ordinals.begin(), ordinals.end());

  if (ordinals.empty()) {
    if (manifest_.committed_sequence == 0) {
      return start_index_generation(manifest_.index_generation);
    }
    return rebuild_index_from_log(&open_report_);
  }

  bool corrupt = false;
  bool truncated = false;
  std::uint64_t loaded_through = 0;
  std::uint64_t seal_through = 0;
  std::uint64_t entries_loaded = 0;
  std::uint64_t truncate_ordinal = 0;
  std::uint64_t truncate_offset = 0;
  const std::uint64_t last_ordinal = ordinals.back();

  for (const std::uint64_t ordinal : ordinals) {
    const std::filesystem::path path = paths_.index_file(manifest_.index_generation, ordinal);
    auto regular = fsl::detail::is_regular_file(path);
    if (!regular.has_value()) {
      return regular.status();
    }
    if (!regular.value()) {
      corrupt = true;
      break;
    }
    auto opened = File::open_read(path);
    if (!opened.has_value()) {
      corrupt = true;
      break;
    }
    File handle = std::move(opened).value();
    auto size = handle.size();
    if (!size.has_value()) {
      return size.status();
    }
    RawFrameStream stream(&handle);
    stream.set_limit(size.value());
    DecodedFrame frame;
    while (true) {
      const FrameDecodeOutcome outcome = stream.next(frame);
      if (outcome == FrameDecodeOutcome::kIncomplete) {
        break;
      }
      if (outcome != FrameDecodeOutcome::kOk) {
        corrupt = true;
        break;
      }
      if (frame.header.kind == FrameKind::kIndexSeal) {
        auto seal = decode_index_seal(frame.body);
        if (!seal.has_value()) {
          corrupt = true;
          break;
        }
        if (seal->index_generation == manifest_.index_generation && seal->through_sequence > seal_through) {
          seal_through = seal->through_sequence;
        }
        continue;
      }
      if (frame.header.kind != FrameKind::kIndexEntry) {
        corrupt = true;
        break;
      }
      auto entry = decode_index_entry(frame.body);
      if (!entry.has_value()) {
        corrupt = true;
        break;
      }
      if (entry->sequence > manifest_.committed_sequence) {
        // The index ran ahead of the commit watermark; the log is authoritative.
        truncated = true;
        truncate_ordinal = ordinal;
        truncate_offset = stream.frame_offset();
        break;
      }
      if (entry->sequence > loaded_through) {
        loaded_through = entry->sequence;
      }
      ++entries_loaded;
      const IndexKind kind = entry->kind;
      const LedgerSequence sequence(entry->sequence);
      if (kind == IndexKind::kEventId) {
        if (auto reject = index_.add_identity(EventId::from_span(entry->key), sequence);
            reject.has_value()) {
          return capacity_exceeded(reject->code, reject->message);
        }
      } else if (kind == IndexKind::kIdempotency) {
        if (auto reject = index_.add_token(IdempotencyToken::from_span(entry->key), sequence);
            reject.has_value()) {
          return capacity_exceeded(reject->code, reject->message);
        }
      }
      if (auto reject = index_.add_posting(kind, entry->key, Posting{entry->sequence, entry->extra, entry->aux});
          reject.has_value()) {
        return capacity_exceeded(reject->code, reject->message);
      }
      fold_entry_into_derived(*entry);
    }
    handle.close();
    if (corrupt || truncated) {
      break;
    }
  }

  if (corrupt) {
    note(ErrorCode::kIndexInconsistent,
         "the derived index for generation " + std::to_string(manifest_.index_generation) +
             " is unreadable or inconsistent; it will be rebuilt from the authoritative log");
    return rebuild_index_from_log(&open_report_);
  }

  index_entry_count_ = entries_loaded;
  const std::uint64_t index_through =
      std::min(std::max(loaded_through, seal_through), manifest_.committed_sequence);
  index_durable_through_ = seal_through;

  if (truncated) {
    note(ErrorCode::kIndexInconsistent, "index entries beyond the commit watermark were discarded");
  }

  const std::uint64_t use_ordinal = truncated ? truncate_ordinal : last_ordinal;
  if (writable_) {
    const std::filesystem::path path = paths_.index_file(manifest_.index_generation, use_ordinal);
    auto opened = File::open_read_write(path);
    if (!opened.has_value()) {
      return rebuild_index_from_log(&open_report_);
    }
    index_file_ = std::move(opened).value();
    if (truncated) {
      if (Status status = index_file_.truncate(truncate_offset); status.is_error()) {
        return status;
      }
    }
    index_ordinal_ = use_ordinal;
    index_file_dirty_ = false;
  }

  if (truncated) {
    // The in-memory index absorbed entries past the watermark before the cut was
    // noticed; drop them so that derived state matches the log exactly.
    index_.drop_postings_above(manifest_.committed_sequence);
    index_entry_count_ = 0;
    for (std::size_t i = 1; i <= 8; ++i) {
      const auto kind = static_cast<IndexKind>(i);
      for (const std::vector<std::uint8_t>& key : index_.keys(kind)) {
        const std::vector<Posting>* postings = index_.postings(kind, key);
        if (postings != nullptr) {
          index_entry_count_ += postings->size();
        }
      }
    }
    recompute_derived_from_index();
  }

  if (index_through < manifest_.committed_sequence) {
    if (Status status = replay_range_into_state(index_through + 1, manifest_.committed_sequence);
        status.is_error()) {
      return status;
    }
    if (writable_) {
      if (Status status = flush_index(true); status.is_error()) {
        return status;
      }
      manifest_.index_through_sequence = manifest_.committed_sequence;
      if (Status status = publish_manifest(true); status.is_error()) {
        return status;
      }
    }
  } else {
    manifest_.index_through_sequence = index_through;
  }
  open_report_.index_truncated = truncated;
  return Status::ok();
}

Status LedgerImpl::replay_range_into_state(std::uint64_t from_sequence, std::uint64_t to_sequence) {
  if (to_sequence < from_sequence) {
    return Status::ok();
  }
  if (from_sequence == 0 || to_sequence > manifest_.committed_sequence) {
    return integrity_failure(ErrorCode::kIndexInconsistent,
                             "derived state was asked to replay outside the committed prefix");
  }
  for (std::uint64_t sequence = from_sequence; sequence <= to_sequence; ++sequence) {
    auto envelope = read_committed(LedgerSequence(sequence));
    if (!envelope.has_value()) {
      return envelope.status();
    }
    const std::vector<IndexEntry> entries = index_entries_for(envelope.value());
    for (const IndexEntry& entry : entries) {
      if (entry.kind == IndexKind::kEventId) {
        if (auto reject = index_.add_identity(envelope->event_id(), envelope->sequence());
            reject.has_value()) {
          return capacity_exceeded(reject->code, reject->message);
        }
      } else if (entry.kind == IndexKind::kIdempotency) {
        if (auto reject = index_.add_token(*envelope->idempotency_token(), envelope->sequence());
            reject.has_value()) {
          return capacity_exceeded(reject->code, reject->message);
        }
      }
      if (auto reject = index_.add_posting(entry.kind, entry.key,
                                           Posting{entry.sequence, entry.extra, entry.aux});
          reject.has_value()) {
        return capacity_exceeded(reject->code, reject->message);
      }
      fold_entry_into_derived(entry);
    }
    if (Status status = append_index_entries(entries); status.is_error()) {
      return status;
    }
    ++open_report_.replayed_events;
  }
  return Status::ok();
}

Status LedgerImpl::rebuild_index_from_log(OpenReport* report) {
  index_.clear();
  derived_ = DerivedState{};
  const std::uint64_t previous_generation = manifest_.index_generation;
  const std::uint64_t previous_through = manifest_.index_through_sequence;
  const std::uint64_t next_generation = previous_generation == std::numeric_limits<std::uint64_t>::max()
                                            ? previous_generation
                                            : previous_generation + 1;
  manifest_.index_generation = next_generation;
  manifest_.index_through_sequence = 0;
  if (Status status = start_index_generation(next_generation); status.is_error()) {
    manifest_.index_generation = previous_generation;
    manifest_.index_through_sequence = previous_through;
    return status;
  }

  if (manifest_.committed_sequence != 0) {
    if (Status status = replay_range_into_state(1, manifest_.committed_sequence); status.is_error()) {
      manifest_.index_generation = previous_generation;
      manifest_.index_through_sequence = previous_through;
      return status;
    }
  }

  open_report_.index_rebuilt = true;
  if (report != nullptr) {
    report->index_rebuilt = true;
  }

  if (!writable_) {
    manifest_.index_generation = previous_generation;
    manifest_.index_through_sequence = previous_through;
    return start_index_generation(previous_generation);
  }

  if (Status status = flush_index(true); status.is_error()) {
    return status;
  }
  manifest_.index_through_sequence = manifest_.committed_sequence;
  if (Status status = publish_manifest(true); status.is_error()) {
    return status;
  }
  // Only after the new generation is durable may the old one be removed.
  std::error_code error;
  if (std::filesystem::is_directory(paths_.index_directory(), error) && !error) {
    auto names = list_directory(paths_.index_directory(), kMaxIndexFiles * 4);
    if (names.has_value()) {
      for (const std::string& name : names.value()) {
        std::uint64_t generation = 0;
        std::uint64_t ordinal = 0;
        if (Paths::parse_index_name(name, generation, ordinal) && generation != next_generation) {
          if (Status status = remove_file(paths_.index_directory() / name); status.is_error()) {
            note(status.code(), "could not remove superseded index file " + name + ": " + status.message());
          }
        }
      }
    }
  }
  return Status::ok();
}

Result<EventEnvelope> LedgerImpl::read_event_at(std::uint64_t segment_index, std::uint64_t offset) const {
  std::shared_ptr<SegmentDescriptor> descriptor;
  {
    std::shared_lock<std::shared_mutex> guard(segments_mutex_);
    for (const auto& candidate : segments_) {
      if (candidate->index == segment_index) {
        descriptor = candidate;
        break;
      }
    }
  }
  if (descriptor == nullptr) {
    return not_found(ErrorCode::kSegmentMissing,
                     "segment " + std::to_string(segment_index) + " is not open");
  }
  std::vector<std::uint8_t> header(kFrameOverhead);
  auto prefix = descriptor->file.read_at(offset, header);
  if (!prefix.has_value()) {
    return prefix.status();
  }
  if (prefix.value() < kFrameOverhead) {
    return integrity_failure(ErrorCode::kTruncatedInput,
                             "the frame at segment " + std::to_string(segment_index) + " offset " +
                                 std::to_string(offset) + " is truncated");
  }
  std::uint32_t frame_size = 0;
  for (int i = 0; i < 4; ++i) {
    frame_size |= static_cast<std::uint32_t>(header[static_cast<std::size_t>(i)]) << (8 * i);
  }
  if (frame_size < kFrameOverhead || frame_size > kMaxFrameSize) {
    return integrity_failure(ErrorCode::kRecordLengthInvalid,
                             "the frame at segment " + std::to_string(segment_index) + " offset " +
                                 std::to_string(offset) + " declares an impossible length");
  }
  std::vector<std::uint8_t> frame(frame_size);
  auto read = descriptor->file.read_at(offset, frame);
  if (!read.has_value()) {
    return read.status();
  }
  if (read.value() != frame.size()) {
    return integrity_failure(ErrorCode::kTruncatedInput,
                             "the frame at segment " + std::to_string(segment_index) + " offset " +
                                 std::to_string(offset) + " is truncated");
  }
  DecodedFrame decoded;
  const FrameDecodeOutcome outcome = decode_frame(frame, decoded);
  if (outcome != FrameDecodeOutcome::kOk) {
    return integrity_failure(outcome == FrameDecodeOutcome::kUnsupportedVersion
                                 ? ErrorCode::kUnsupportedFormatVersion
                                 : ErrorCode::kMidFileCorruption,
                             "the frame at segment " + std::to_string(segment_index) + " offset " +
                                 std::to_string(offset) + " failed integrity validation");
  }
  if (decoded.header.kind != FrameKind::kEvent) {
    return integrity_failure(ErrorCode::kRecordLengthInvalid,
                             "the frame at segment " + std::to_string(segment_index) + " offset " +
                                 std::to_string(offset) + " is not an event record");
  }
  return decode_envelope(decoded.body);
}

}  // namespace fsl::detail

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

/// \file ledger_read.cpp
/// Bounded queries, integrity verification and structural inspection.
///
/// Every read path re-validates the record it returns against the log. The
/// derived index only ever narrows a search; it is never the answer.

namespace fsl::detail {
namespace {

constexpr std::size_t kMaxDirectoryEntries = 65536;
constexpr std::size_t kIndexVerificationCap = 4'000'000;

void store_u64_le(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

[[nodiscard]] bool in_range(std::uint64_t value, std::uint64_t low, std::uint64_t high) noexcept {
  return value >= low && value <= high;
}

}  // namespace

Result<EventEnvelope> LedgerImpl::read(LedgerSequence sequence) const { return read_committed(sequence); }

Result<EventEnvelope> LedgerImpl::find_event(EventId identity) const {
  if (identity.is_nil()) {
    return invalid_argument(ErrorCode::kHeaderFieldInvalid, "the nil identity is never a committed event");
  }
  auto sequence = index_.event_sequence(identity);
  if (!sequence.has_value()) {
    return not_found(ErrorCode::kNotFound,
                     "event identity " + identity.to_hex() + " has no committed record");
  }
  auto envelope = read_committed(*sequence);
  if (!envelope.has_value()) {
    return envelope.status();
  }
  if (!(envelope->event_id() == identity)) {
    return integrity_failure(ErrorCode::kIndexInconsistent,
                             "the derived index located an event whose identity differs; rebuild the index");
  }
  return envelope;
}

Result<EventEnvelope> LedgerImpl::find_idempotency(IdempotencyToken token) const {
  if (token.is_nil()) {
    return invalid_argument(ErrorCode::kHeaderFieldInvalid,
                            "the nil identity is never a valid idempotency token");
  }
  auto sequence = index_.token_sequence(token);
  if (!sequence.has_value()) {
    return not_found(ErrorCode::kNotFound,
                     "idempotency token " + token.to_hex() + " has no committed record");
  }
  auto envelope = read_committed(*sequence);
  if (!envelope.has_value()) {
    return envelope.status();
  }
  if (!envelope->idempotency_token().has_value() || !(*envelope->idempotency_token() == token)) {
    return integrity_failure(ErrorCode::kIndexInconsistent,
                             "the derived index located a token whose record differs; rebuild the index");
  }
  return envelope;
}

Result<QueryPage> LedgerImpl::query(const Query& request) const {
  if (request.limit == 0) {
    return invalid_argument(ErrorCode::kLimitRequired, "a query must supply a positive limit");
  }
  if (request.limit > options_.max_query_limit) {
    return capacity_exceeded(ErrorCode::kLimitOutOfRange,
                             "the requested limit " + std::to_string(request.limit) +
                                 " exceeds the configured maximum " +
                                 std::to_string(options_.max_query_limit));
  }
  if (request.from.has_value() && request.to.has_value() &&
      request.from->value() > request.to->value()) {
    return invalid_argument(ErrorCode::kRangeInvalid,
                            "the query range starts after it ends: " +
                                std::to_string(request.from->value()) + " > " +
                                std::to_string(request.to->value()));
  }

  const std::uint64_t upper = committed_sequence_.load(std::memory_order_acquire);
  if (upper == 0) {
    return QueryPage{};
  }
  std::uint64_t low = request.from.has_value() ? request.from->value() : 1;
  const std::uint64_t high = request.to.has_value() ? std::min(request.to->value(), upper) : upper;
  if (request.after.has_value()) {
    if (request.after->value() == std::numeric_limits<std::uint64_t>::max()) {
      return QueryPage{};
    }
    low = std::max(low, request.after->value() + 1);
  }
  if (low > high) {
    return QueryPage{};
  }

  const auto matches = [&request](const EventEnvelope& envelope) {
    if (request.event_id.has_value() && !(envelope.event_id() == *request.event_id)) {
      return false;
    }
    if (request.subject.has_value() && !(envelope.subject() == *request.subject)) {
      return false;
    }
    if (request.source.has_value() && !(envelope.provenance().source() == *request.source)) {
      return false;
    }
    if (request.facility_generation.has_value() &&
        envelope.facility_generation().value() != request.facility_generation->value()) {
      return false;
    }
    if (request.epoch.has_value() &&
        (!envelope.epoch().has_value() || envelope.epoch()->value() != request.epoch->value())) {
      return false;
    }
    if (request.kind.has_value() && envelope.kind() != *request.kind) {
      return false;
    }
    return true;
  };

  // Choose one posting source to narrow the candidate set. The remaining
  // predicates are applied to the records themselves.
  const std::vector<Posting>* candidates = nullptr;
  bool posting_source = false;
  std::vector<std::uint8_t> single;
  if (request.event_id.has_value()) {
    const std::optional<LedgerSequence> located_sequence = index_.event_sequence(*request.event_id);
    if (located_sequence.has_value()) {
      store_u64_le(single, located_sequence.value().value());
    }
    posting_source = true;
  } else if (request.subject.has_value()) {
    candidates = index_.postings(IndexKind::kSubject, subject_key(*request.subject));
    posting_source = true;
  } else if (request.source.has_value()) {
    const std::vector<std::uint8_t> key(request.source->view().begin(), request.source->view().end());
    candidates = index_.postings(IndexKind::kSource, key);
    posting_source = true;
  } else if (request.epoch.has_value()) {
    std::vector<std::uint8_t> key;
    store_u64_le(key, request.epoch->value());
    candidates = index_.postings(IndexKind::kEpoch, key);
    posting_source = true;
  } else if (request.facility_generation.has_value()) {
    std::vector<std::uint8_t> key;
    store_u64_le(key, request.facility_generation->value());
    candidates = index_.postings(IndexKind::kGeneration, key);
    posting_source = true;
  } else if (request.kind.has_value()) {
    const std::vector<std::uint8_t> key{static_cast<std::uint8_t>(*request.kind)};
    candidates = index_.postings(IndexKind::kEventKind, key);
    posting_source = true;
  }

  QueryPage page;
  const bool indexed =
      posting_source && (candidates != nullptr || index_.postings_complete()) &&
      !(request.event_id.has_value() && single.empty() &&
        !index_.event_sequence(*request.event_id).has_value());
  if (indexed) {
    page.source = QuerySource::kIndexed;
    if (candidates != nullptr) {
      for (const Posting& posting : *candidates) {
        if (!in_range(posting.sequence, low, high)) {
          continue;
        }
        if (page.events.size() >= request.limit) {
          page.exhausted = false;
          break;
        }
        auto envelope = read_committed(LedgerSequence(posting.sequence));
        if (!envelope.has_value()) {
          return envelope.status();
        }
        ++page.records_examined;
        if (matches(envelope.value())) {
          page.events.push_back(std::move(envelope).value());
        }
      }
    } else if (!single.empty()) {
      const std::uint64_t sequence = [&]() {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < single.size() && i < 8; ++i) {
          value |= static_cast<std::uint64_t>(single[i]) << (8u * i);
        }
        return value;
      }();
      if (in_range(sequence, low, high)) {
        auto envelope = read_committed(LedgerSequence(sequence));
        if (!envelope.has_value()) {
          return envelope.status();
        }
        ++page.records_examined;
        if (matches(envelope.value())) {
          page.events.push_back(std::move(envelope).value());
        }
      }
    }
  } else {
    // Linear scan, bounded by the sequence window so that an unindexed predicate
    // can never walk an arbitrarily large ledger.
    if (high - low + 1 > options_.max_scan_window) {
      return capacity_exceeded(ErrorCode::kCapacityLimitExceeded,
                               "this query has no usable index postings and the requested window of " +
                                   std::to_string(high - low + 1) +
                                   " sequences exceeds the configured maximum of " +
                                   std::to_string(options_.max_scan_window) +
                                   "; narrow the range or raise max_scan_window");
    }
    page.source = QuerySource::kLinearScan;
    for (std::uint64_t sequence = low; sequence <= high; ++sequence) {
      if (page.events.size() >= request.limit) {
        page.exhausted = false;
        break;
      }
      auto envelope = read_committed(LedgerSequence(sequence));
      if (!envelope.has_value()) {
        return envelope.status();
      }
      ++page.records_examined;
      if (matches(envelope.value())) {
        page.events.push_back(std::move(envelope).value());
      }
    }
  }

  if (page.events.empty()) {
    page.exhausted = true;
    page.next_cursor.reset();
  } else if (page.exhausted) {
    page.next_cursor.reset();
  } else {
    page.next_cursor = page.events.back().sequence();
  }
  return page;
}

// -- Verification -------------------------------------------------------------

VerifyReport LedgerImpl::verify_index_consistency(const VerifyReport& base) const {
  VerifyReport report = base;
  report.index_verified = true;

  const std::uint64_t upper = committed_sequence_.load(std::memory_order_acquire);
  const std::uint64_t bound = std::min<std::uint64_t>(upper, kIndexVerificationCap);
  if (upper > bound) {
    report.index_consistent = false;
    report.findings.push_back(
        VerifyFinding{ErrorCode::kIndexInconsistent,
                      "index verification covers at most " + std::to_string(kIndexVerificationCap) +
                          " records; " + std::to_string(upper) + " are committed",
                      std::nullopt, std::nullopt, 0});
  }
  for (std::uint64_t sequence = 1; sequence <= bound; ++sequence) {
    auto envelope = read_committed(LedgerSequence(sequence));
    if (!envelope.has_value()) {
      report.index_consistent = false;
      report.findings.push_back(VerifyFinding{envelope.status().code(), envelope.status().message(),
                                              LedgerSequence(sequence), std::nullopt, 0});
      break;
    }
    if (auto found = index_.event_sequence(envelope->event_id());
        !found.has_value() || found->value() != sequence) {
      report.index_consistent = false;
      report.findings.push_back(VerifyFinding{ErrorCode::kIndexInconsistent,
                                              "the index does not map event identity " +
                                                  envelope->event_id().to_hex() + " to sequence " +
                                                  std::to_string(sequence),
                                              LedgerSequence(sequence), std::nullopt, 0});
      break;
    }
    const std::vector<IndexEntry> entries = index_entries_for(envelope.value());
    bool subject_posted = false;
    for (const IndexEntry& entry : entries) {
      if (entry.kind != IndexKind::kSubject) {
        continue;
      }
      const std::vector<Posting>* postings = index_.postings(entry.kind, entry.key);
      subject_posted = postings != nullptr &&
                       std::any_of(postings->begin(), postings->end(), [sequence](const Posting& posting) {
                         return posting.sequence == sequence;
                       });
      break;
    }
    if (!subject_posted) {
      report.index_consistent = false;
      report.findings.push_back(VerifyFinding{ErrorCode::kIndexInconsistent,
                                              "the index holds no subject posting for sequence " +
                                                  std::to_string(sequence),
                                              LedgerSequence(sequence), std::nullopt, 0});
      break;
    }
  }
  if (!report.index_consistent && report.status.is_ok()) {
    report.status = integrity_failure(ErrorCode::kIndexInconsistent,
                                      "the derived index disagrees with the authoritative log");
  }
  return report;
}

VerifyReport LedgerImpl::verify(const VerifyRequest& request) const {
  VerifyReport report;

  std::vector<std::shared_ptr<SegmentDescriptor>> snapshot;
  {
    std::shared_lock<std::shared_mutex> guard(segments_mutex_);
    snapshot = segments_;
  }
  if (snapshot.empty()) {
    report.status = integrity_failure(ErrorCode::kSegmentMissing, "the ledger holds no open segments");
    return report;
  }

  const std::uint64_t upper = committed_sequence_.load(std::memory_order_acquire);
  std::uint64_t expected_sequence = 1;
  std::uint64_t resume_offset = kSegmentHeaderSize;
  std::size_t begin = 0;
  Digest anchor;
  bool anchored = false;

  if (request.scope == VerifyScope::kManifestTail) {
    if (upper == 0) {
      return report;
    }
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
      if (snapshot[i]->index == manifest_.committed_segment_index) {
        begin = i;
        break;
      }
    }
    expected_sequence = snapshot[begin]->base_sequence;
  } else if (request.scope == VerifyScope::kFromCheckpoint) {
    if (!request.checkpoint_sequence.has_value()) {
      report.status = invalid_argument(ErrorCode::kMissingRequiredAttribute,
                                       "verification from a checkpoint must name the checkpoint sequence");
      return report;
    }
    const std::filesystem::path path = paths_.checkpoint(*request.checkpoint_sequence);
    auto present = path_exists(path);
    if (!present.has_value()) {
      report.status = present.status();
      return report;
    }
    if (!present.value()) {
      report.status = not_found(ErrorCode::kNotFound, "no checkpoint at ledger sequence " +
                                                          std::to_string(request.checkpoint_sequence->value()));
      return report;
    }
    auto file = File::open_read(path);
    if (!file.has_value()) {
      report.status = file.status();
      return report;
    }
    File handle = std::move(file).value();
    auto size = handle.size();
    if (!size.has_value()) {
      report.status = size.status();
      return report;
    }
    if (size.value() > (1u << 20)) {
      report.status =
          malformed_input(ErrorCode::kSizeOutOfRange, "the checkpoint file is implausibly large");
      return report;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.value()));
    auto read = handle.read_at(0, bytes);
    if (!read.has_value() || read.value() != bytes.size()) {
      report.status = integrity_failure(ErrorCode::kTruncatedInput, "the checkpoint file is truncated");
      return report;
    }
    auto checkpoint = decode_checkpoint(bytes);
    if (!checkpoint.has_value()) {
      report.status = checkpoint.status();
      return report;
    }
    if (!(checkpoint->ledger_id == manifest_.ledger_id)) {
      report.status = integrity_failure(ErrorCode::kLedgerIdentityMismatch,
                                        "the checkpoint belongs to a different ledger identity");
      return report;
    }
    if (checkpoint->sequence > upper) {
      report.status = integrity_failure(ErrorCode::kManifestAheadOfLog,
                                        "the checkpoint claims ledger sequence " +
                                            std::to_string(checkpoint->sequence) +
                                            " which is beyond the commit watermark");
      return report;
    }
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
      if (snapshot[i]->index == checkpoint->segment_index) {
        begin = i;
        break;
      }
    }
    expected_sequence = checkpoint->sequence + 1;
    anchor = checkpoint->chain_at_sequence;
    anchored = true;
    resume_offset = checkpoint->offset;
  }

  for (std::size_t i = begin; i < snapshot.size(); ++i) {
    const auto& descriptor = snapshot[i];
    const std::uint64_t stop = descriptor->end_offset.load(std::memory_order_acquire);
    if (stop <= kSegmentHeaderSize) {
      continue;
    }
    const std::uint64_t expect = (i == begin && anchored) ? expected_sequence : descriptor->base_sequence;
    if (i != begin && descriptor->base_sequence != expected_sequence) {
      report.status = integrity_failure(ErrorCode::kSequenceGap,
                                        "segment " + std::to_string(descriptor->index) +
                                            " begins at ledger sequence " +
                                            std::to_string(descriptor->base_sequence) +
                                            " but the previous segment ended at " +
                                            std::to_string(expected_sequence - 1));
      report.findings.push_back(VerifyFinding{report.status.code(), report.status.message(), std::nullopt,
                                              SegmentIndex(descriptor->index), 0});
      return report;
    }
    const Digest chain =
        (i == begin && anchored)
            ? anchor
            : segment_chain_seed(SegmentHeader{kSegmentFormatVersion, manifest_.ledger_id,
                                               descriptor->index, descriptor->base_sequence, 0});
    const WalkResult walked =
        walk_segment(*descriptor, (i == begin) ? resume_offset : kSegmentHeaderSize, expect, chain, stop,
                    request.max_records, false, nullptr);
    report.segments_verified += 1;
    report.records_verified += walked.records;
    report.bytes_verified += walked.bytes;
    if (walked.finding.has_value()) {
      report.status = integrity_failure(walked.finding->code, walked.finding->message);
      report.findings.push_back(*walked.finding);
      return report;
    }
    if (!report.first_sequence.has_value() && walked.records != 0) {
      report.first_sequence = LedgerSequence(expect);
    }
    if (walked.records != 0) {
      report.last_sequence = LedgerSequence(walked.last_sequence);
      expected_sequence = walked.last_sequence + 1;
    }
    report.chain_at_end = walked.chain;
    if (request.max_records != 0 && report.records_verified >= request.max_records) {
      break;
    }
  }

  if (upper != 0) {
    const std::uint64_t reached = report.last_sequence.has_value() ? report.last_sequence->value() : 0;
    if (reached != upper) {
      report.status = integrity_failure(ErrorCode::kCommittedRecordLost,
                                        "verification reached ledger sequence " + std::to_string(reached) +
                                            " but the commit watermark is " + std::to_string(upper));
      report.findings.push_back(VerifyFinding{report.status.code(), report.status.message(),
                                              report.last_sequence, std::nullopt, 0});
      return report;
    }
  }
  if (request.verify_index) {
    report = verify_index_consistency(report);
  }
  return report;
}

// -- Structural inspection ----------------------------------------------------

Result<std::vector<SegmentInfo>> LedgerImpl::segments() const {
  std::vector<SegmentInfo> result;
  std::vector<std::shared_ptr<SegmentDescriptor>> snapshot;
  {
    std::shared_lock<std::shared_mutex> guard(segments_mutex_);
    snapshot = segments_;
  }
  result.reserve(snapshot.size());
  for (const auto& descriptor : snapshot) {
    SegmentInfo info;
    info.index = SegmentIndex(descriptor->index);
    const std::uint64_t end = descriptor->end_sequence.load(std::memory_order_acquire);
    if (descriptor->base_sequence != 0) {
      info.first_sequence = LedgerSequence(descriptor->base_sequence);
    }
    if (end != 0) {
      info.last_sequence = LedgerSequence(end);
    }
    info.frame_count = descriptor->frame_count.load(std::memory_order_acquire);
    info.event_count = end == 0 ? 0 : end - descriptor->base_sequence + 1;
    // The committed extent, taken from the descriptor rather than re-queried
    // from the file: a second handle's view of a file's size is only guaranteed
    // coherent after a flush, so asking the operating system here could report a
    // stale value for a segment the writer is still extending.
    info.byte_size = descriptor->end_offset.load(std::memory_order_acquire);
    info.sealed = descriptor->sealed.load(std::memory_order_acquire);
    info.file_name = paths_.segment(SegmentIndex(descriptor->index)).filename().string();
    result.push_back(std::move(info));
  }
  return result;
}

Result<SegmentInfo> LedgerImpl::segment(SegmentIndex index) const {
  std::shared_ptr<SegmentDescriptor> descriptor;
  {
    std::shared_lock<std::shared_mutex> guard(segments_mutex_);
    for (const auto& candidate : segments_) {
      if (candidate->index == index.value()) {
        descriptor = candidate;
        break;
      }
    }
  }
  if (descriptor == nullptr) {
    return not_found(ErrorCode::kSegmentMissing,
                     "segment " + std::to_string(index.value()) + " is not part of this ledger");
  }
  const std::uint64_t stop = descriptor->end_offset.load(std::memory_order_acquire);
  const Digest seed =
      segment_chain_seed(SegmentHeader{kSegmentFormatVersion, manifest_.ledger_id, descriptor->index,
                                       descriptor->base_sequence, 0});
  SegmentInfo info;
  info.index = index;
  if (descriptor->base_sequence != 0) {
    info.first_sequence = LedgerSequence(descriptor->base_sequence);
  }
  const std::uint64_t end = descriptor->end_sequence.load(std::memory_order_acquire);
  if (end != 0) {
    info.last_sequence = LedgerSequence(end);
  }
  info.event_count = end == 0 ? 0 : end - descriptor->base_sequence + 1;
  info.sealed = descriptor->sealed.load(std::memory_order_acquire);
  info.file_name = paths_.segment(index).filename().string();
  auto size = const_cast<File&>(descriptor->file).size();
  info.byte_size = size.has_value() ? size.value() : 0;

  const WalkResult walked = walk_segment(*descriptor, kSegmentHeaderSize, descriptor->base_sequence, seed,
                                         stop, 0, false, nullptr);
  if (walked.finding.has_value()) {
    return integrity_failure(walked.finding->code, walked.finding->message);
  }
  info.frame_count = walked.records;
  info.chain = walked.chain;
  info.verified = true;
  return info;
}

Result<std::vector<CheckpointInfo>> LedgerImpl::checkpoints() const {
  std::vector<CheckpointInfo> result;
  std::error_code error;
  if (!std::filesystem::is_directory(paths_.checkpoints_directory(), error) || error) {
    return result;
  }
  auto names = list_directory(paths_.checkpoints_directory(), kMaxDirectoryEntries);
  if (!names.has_value()) {
    return names.status();
  }
  for (const std::string& name : names.value()) {
    LedgerSequence sequence{1};
    if (!Paths::parse_checkpoint_name(name, sequence)) {
      continue;
    }
    const std::filesystem::path path = paths_.checkpoints_directory() / name;
    auto file = File::open_read(path);
    if (!file.has_value()) {
      continue;
    }
    File handle = std::move(file).value();
    auto size = handle.size();
    if (!size.has_value() || size.value() > (1u << 20)) {
      continue;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.value()));
    auto read = handle.read_at(0, bytes);
    if (!read.has_value() || read.value() != bytes.size()) {
      continue;
    }
    auto checkpoint = decode_checkpoint(bytes);
    if (!checkpoint.has_value()) {
      continue;
    }
    if (!(checkpoint->ledger_id == manifest_.ledger_id)) {
      continue;
    }
    result.push_back(CheckpointInfo{LedgerSequence(checkpoint->sequence),
                                    LogicalTick(checkpoint->logical_tick),
                                    SegmentIndex(checkpoint->segment_index),
                                    checkpoint->offset,
                                    checkpoint->chain_at_sequence,
                                    checkpoint->event_count,
                                    checkpoint->created_unix_nanos,
                                    name});
  }
  std::sort(result.begin(), result.end(), [](const CheckpointInfo& lhs, const CheckpointInfo& rhs) {
    return lhs.sequence < rhs.sequence;
  });
  return result;
}

Result<CheckpointInfo> LedgerImpl::latest_checkpoint() const {
  auto all = checkpoints();
  if (!all.has_value()) {
    return all.status();
  }
  if (all.value().empty()) {
    return not_found(ErrorCode::kNotFound, "this ledger has no checkpoints");
  }
  return all.value().back();
}

Result<SubjectView> LedgerImpl::subject(const SubjectRef& reference) const {
  SubjectView view(reference);
  const std::vector<Posting>* postings = index_.postings(IndexKind::kSubject, subject_key(reference));
  if (postings == nullptr || postings->empty()) {
    return view;
  }
  view.registered = true;
  view.first_sequence = LedgerSequence(postings->front().sequence);
  view.last_sequence = LedgerSequence(postings->back().sequence);
  // Retirement is a lifecycle transition, not merely the most recent event about
  // the subject; the view must agree with what admission will decide.
  for (auto entry = postings->rbegin(); entry != postings->rend(); ++entry) {
    if (entry->extra == static_cast<std::uint64_t>(EventKind::kSubjectRetired)) {
      view.retired = true;
      break;
    }
    if (entry->extra == static_cast<std::uint64_t>(EventKind::kSubjectRegistered)) {
      break;
    }
  }
  view.event_count = postings->size();
  return view;
}

Result<std::vector<SubjectView>> LedgerImpl::subjects(std::size_t limit,
                                                      const std::optional<SubjectRef>& after) const {
  if (limit == 0) {
    return invalid_argument(ErrorCode::kLimitRequired, "a subject listing must supply a positive limit");
  }
  if (limit > options_.max_query_limit) {
    return capacity_exceeded(ErrorCode::kLimitOutOfRange,
                             "the requested limit exceeds the configured maximum");
  }
  std::vector<SubjectRef> references;
  for (const std::vector<std::uint8_t>& key : index_.keys(IndexKind::kSubject)) {
    ByteReader reader(key);
    std::optional<SubjectRef> reference;
    if (decode_subject_ref(reader, reference) && reference.has_value()) {
      references.push_back(std::move(reference).value());
    }
  }
  std::sort(references.begin(), references.end());
  std::vector<SubjectView> result;
  for (const SubjectRef& reference : references) {
    if (after.has_value() && !(*after < reference)) {
      continue;
    }
    if (result.size() >= limit) {
      break;
    }
    auto view = subject(reference);
    if (!view.has_value()) {
      return view.status();
    }
    result.push_back(std::move(view).value());
  }
  return result;
}

Result<std::vector<SourceView>> LedgerImpl::sources(std::size_t limit) const {
  if (limit == 0) {
    return invalid_argument(ErrorCode::kLimitRequired, "a source listing must supply a positive limit");
  }
  if (limit > options_.max_query_limit) {
    return capacity_exceeded(ErrorCode::kLimitOutOfRange,
                             "the requested limit exceeds the configured maximum");
  }
  std::vector<SourceComponentId> identifiers;
  for (const std::vector<std::uint8_t>& key : index_.keys(IndexKind::kSource)) {
    auto identifier =
        SourceComponentId::parse(std::string_view(reinterpret_cast<const char*>(key.data()), key.size()));
    if (identifier.has_value()) {
      identifiers.push_back(std::move(identifier).value());
    }
  }
  std::sort(identifiers.begin(), identifiers.end());
  std::vector<SourceView> result;
  for (const SourceComponentId& identifier : identifiers) {
    if (result.size() >= limit) {
      break;
    }
    const std::vector<std::uint8_t> key(identifier.view().begin(), identifier.view().end());
    const std::vector<Posting>* postings = index_.postings(IndexKind::kSource, key);
    if (postings == nullptr || postings->empty()) {
      continue;
    }
    SourceView view(identifier,
                    SourceGeneration(postings->back().extra == 0 ? 1 : postings->back().extra));
    if (postings->back().aux != 0) {
      view.last_source_sequence = SourceSequence(postings->back().aux);
    }
    view.last_sequence = LedgerSequence(postings->back().sequence);
    view.event_count = postings->size();
    result.push_back(std::move(view));
  }
  return result;
}

// -- Snapshots ----------------------------------------------------------------

Result<LedgerIdentity> LedgerImpl::identity() const {
  if (closed_.load(std::memory_order_acquire) && manifest_.ledger_id.is_nil()) {
    return closed(ErrorCode::kLedgerClosed, "the handle is closed");
  }
  LedgerIdentity identity;
  identity.id = manifest_.ledger_id;
  identity.segment_format_version = kSegmentFormatVersion;
  identity.manifest_format_version = kManifestFormatVersion;
  return identity;
}

Result<CommitWatermark> LedgerImpl::watermark() const {
  std::lock_guard<std::mutex> guard(mutex_);
  if (closed_.load(std::memory_order_acquire)) {
    return closed(ErrorCode::kLedgerClosed, "the handle is closed");
  }
  CommitWatermark watermark;
  if (manifest_.committed_sequence != 0) {
    watermark.sequence = LedgerSequence(manifest_.committed_sequence);
    watermark.logical_tick = LogicalTick(manifest_.committed_logical_tick);
    watermark.segment_index = SegmentIndex(manifest_.committed_segment_index);
  }
  watermark.offset = manifest_.committed_offset;
  watermark.chain = manifest_.committed_chain;
  watermark.event_count = manifest_.total_event_count;
  watermark.manifest_generation = manifest_generation_;
  return watermark;
}

Result<LedgerState> LedgerImpl::state() const {
  std::lock_guard<std::mutex> guard(mutex_);
  if (closed_.load(std::memory_order_acquire)) {
    return closed(ErrorCode::kLedgerClosed, "the handle is closed");
  }
  LedgerState state;
  state.identity.id = manifest_.ledger_id;
  state.identity.segment_format_version = kSegmentFormatVersion;
  state.identity.manifest_format_version = kManifestFormatVersion;
  if (manifest_.committed_sequence != 0) {
    state.watermark.sequence = LedgerSequence(manifest_.committed_sequence);
    state.watermark.logical_tick = LogicalTick(manifest_.committed_logical_tick);
    state.watermark.segment_index = SegmentIndex(manifest_.committed_segment_index);
  }
  state.watermark.offset = manifest_.committed_offset;
  state.watermark.chain = manifest_.committed_chain;
  state.watermark.event_count = manifest_.total_event_count;
  state.watermark.manifest_generation = manifest_generation_;
  state.facility_generation = FacilityGeneration(derived_.facility_generation);
  state.open_epoch = derived_.open_epoch_value();
  state.latest_epoch = derived_.latest_epoch_value();
  state.active_segment = SegmentIndex(manifest_.active_segment_index);
  state.segment_count = manifest_.segment_count;
  state.writer_incarnation = writer_incarnation_;
  state.writable = writable_;
  state.durable = durable_;
  state.closed = false;
  return state;
}

LedgerStats LedgerImpl::stats() const {
  std::lock_guard<std::mutex> guard(stats_mutex_);
  LedgerStats snapshot = stats_;
  snapshot.segment_count = manifest_.segment_count;
  snapshot.dedupe_entries = index_.identity_count();
  snapshot.index_postings = index_.posting_count();
  snapshot.index_postings_complete = index_.postings_complete();
  snapshot.subjects_known = index_.distinct_keys(IndexKind::kSubject);
  snapshot.sources_tracked = index_.distinct_keys(IndexKind::kSource);
  snapshot.committed_events = committed_sequence_.load(std::memory_order_acquire);
  snapshot.durable = durable_;
  snapshot.writable = writable_;
  return snapshot;
}

}  // namespace fsl::detail

// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>

#include "detail/ledger_impl.hpp"

/// \file ledger_locate.cpp
/// Mapping a ledger sequence to a byte offset, and reading one committed record.
///
/// The locate path is deliberately paranoid: a record whose declared sequence
/// disagrees with the position it was found at is reported as forgery and is
/// never returned to a caller.

namespace fsl::detail {

bool LedgerImpl::ensure_frame_offsets(const std::shared_ptr<SegmentDescriptor>& descriptor) const {
  {
    std::shared_lock<std::shared_mutex> guard(segments_mutex_);
    if (!descriptor->offsets_available) {
      return false;
    }
    if (!descriptor->frame_offsets.empty()) {
      return true;
    }
    if (descriptor->end_offset.load(std::memory_order_acquire) <= kSegmentHeaderSize) {
      return true;
    }
  }

  const std::uint64_t end = descriptor->end_offset.load(std::memory_order_acquire);
  std::vector<std::uint64_t> offsets;
  std::uint64_t offset = kSegmentHeaderSize;
  std::vector<std::uint8_t> buffer;
  std::uint64_t buffer_base = 0;
  std::size_t buffer_length = 0;
  while (offset < end) {
    const bool buffered = offset >= buffer_base && (offset - buffer_base) + kFrameOverhead <= buffer_length;
    if (!buffered) {
      const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(end - offset, 256u * 1024u));
      if (want < kFrameOverhead) {
        break;
      }
      buffer.assign(want, 0);
      auto read = descriptor->file.read_at(offset, buffer);
      if (!read.has_value() || read.value() != want) {
        return false;
      }
      buffer_base = offset;
      buffer_length = read.value();
    }
    const std::size_t within = static_cast<std::size_t>(offset - buffer_base);
    DecodedFrame frame;
    if (decode_frame(std::span<const std::uint8_t>(buffer.data() + within, buffer_length - within), frame) !=
        FrameDecodeOutcome::kOk) {
      return false;
    }
    offsets.push_back(offset);
    offset += frame.header.frame_size;
  }

  std::unique_lock<std::shared_mutex> guard(segments_mutex_);
  if (!descriptor->offsets_available) {
    return false;
  }
  if (descriptor->frame_offsets.empty()) {
    descriptor->frame_offsets = std::move(offsets);
    descriptor->frame_count.store(descriptor->frame_offsets.size(), std::memory_order_release);
    resident_frame_offsets_ += descriptor->frame_offsets.size();
  }
  enforce_frame_offset_budget();
  return descriptor->offsets_available;
}

void LedgerImpl::enforce_frame_offset_budget() const {
  // Called with segments_mutex_ held exclusively. The lowest-numbered segments
  // release their tables first; random access into them then costs one bounded
  // scan of that segment.
  if (resident_frame_offsets_ <= options_.max_cached_frame_offsets) {
    return;
  }
  for (const auto& descriptor : segments_) {
    if (resident_frame_offsets_ <= options_.max_cached_frame_offsets) {
      break;
    }
    if (descriptor->frame_offsets.empty()) {
      continue;
    }
    resident_frame_offsets_ -= descriptor->frame_offsets.size();
    descriptor->frame_offsets.clear();
    descriptor->frame_offsets.shrink_to_fit();
    descriptor->offsets_available = false;
  }
}

Result<LocatedFrame> LedgerImpl::locate(LedgerSequence sequence) const {
  const std::uint64_t upper = committed_sequence_.load(std::memory_order_acquire);
  if (sequence.value() == 0 || sequence.value() > upper) {
    return not_found(ErrorCode::kNotFound,
                     "ledger sequence " + std::to_string(sequence.value()) +
                         " is not committed; the commit watermark is " + std::to_string(upper));
  }
  auto descriptor = segment_for(sequence.value());
  if (descriptor == nullptr) {
    return integrity_failure(ErrorCode::kSegmentMissing,
                             "no open segment holds ledger sequence " + std::to_string(sequence.value()));
  }
  const std::uint64_t wanted = sequence.value() - descriptor->base_sequence;
  if (ensure_frame_offsets(descriptor)) {
    std::shared_lock<std::shared_mutex> guard(segments_mutex_);
    if (wanted >= descriptor->frame_offsets.size()) {
      return integrity_failure(ErrorCode::kCommittedRecordLost,
                               "segment " + std::to_string(descriptor->index) +
                                   " does not hold ledger sequence " + std::to_string(sequence.value()));
    }
    LocatedFrame located;
    located.segment_index = descriptor->index;
    located.offset = descriptor->frame_offsets[static_cast<std::size_t>(wanted)];
    return located;
  }

  // Fallback: one bounded walk of this segment.
  const std::uint64_t end = descriptor->end_offset.load(std::memory_order_acquire);
  std::uint64_t offset = kSegmentHeaderSize;
  std::uint64_t index = 0;
  std::vector<std::uint8_t> buffer;
  std::uint64_t buffer_base = 0;
  std::size_t buffer_length = 0;
  while (offset < end) {
    const bool buffered = offset >= buffer_base && (offset - buffer_base) + kFrameOverhead <= buffer_length;
    if (!buffered) {
      const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(end - offset, 256u * 1024u));
      if (want < kFrameOverhead) {
        break;
      }
      buffer.assign(want, 0);
      auto read = descriptor->file.read_at(offset, buffer);
      if (!read.has_value() || read.value() != want) {
        return integrity_failure(ErrorCode::kFileReadFailed,
                                 "cannot read segment " + std::to_string(descriptor->index));
      }
      buffer_base = offset;
      buffer_length = read.value();
    }
    const std::size_t within = static_cast<std::size_t>(offset - buffer_base);
    DecodedFrame frame;
    if (decode_frame(std::span<const std::uint8_t>(buffer.data() + within, buffer_length - within), frame) !=
        FrameDecodeOutcome::kOk) {
      return integrity_failure(ErrorCode::kMidFileCorruption,
                               "segment " + std::to_string(descriptor->index) +
                                   " is corrupt at offset " + std::to_string(offset));
    }
    if (index == wanted) {
      LocatedFrame located;
      located.segment_index = descriptor->index;
      located.offset = offset;
      located.frame_size = frame.header.frame_size;
      return located;
    }
    ++index;
    offset += frame.header.frame_size;
  }
  return integrity_failure(ErrorCode::kCommittedRecordLost,
                           "segment " + std::to_string(descriptor->index) +
                               " does not hold ledger sequence " + std::to_string(sequence.value()));
}

Result<EventEnvelope> LedgerImpl::read_committed(LedgerSequence sequence) const {
  if (closed_.load(std::memory_order_acquire)) {
    return closed(ErrorCode::kLedgerClosed, "the handle is closed");
  }
  auto located = locate(sequence);
  if (!located.has_value()) {
    return located.status();
  }
  auto envelope = read_event_at(located->segment_index, located->offset);
  if (!envelope.has_value()) {
    return envelope.status();
  }
  if (envelope->sequence().value() != sequence.value()) {
    return integrity_failure(ErrorCode::kSequenceForgery,
                             "the record at ledger sequence " + std::to_string(sequence.value()) +
                                 " declares sequence " + std::to_string(envelope->sequence().value()));
  }
  return envelope;
}

}  // namespace fsl::detail

// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>

#include "detail/codec.hpp"
#include "detail/ledger_impl.hpp"

/// \file replay.cpp
/// Deterministic replay: a forward-only, buffered walk of the immutable
/// committed prefix with a fixed upper bound.
///
/// Replay never re-reads the commit watermark, so a concurrent writer cannot
/// change what a stream will deliver. Events are delivered in strictly ascending
/// ledger sequence with no gaps in the walked range; a filter suppresses an
/// event but never advances past one silently.

namespace fsl {

/// Forward-only replay state. Defined here so that the public header only has to
/// name the type.
struct ReplayStream::Impl {
  std::shared_ptr<detail::LedgerImpl> ledger;
  ReplayRequest request;
  std::unique_ptr<detail::SequentialFrameReader> reader;
  Status error;
  std::optional<std::uint64_t> upper;
  std::optional<std::uint64_t> first;
  std::uint64_t delivered = 0;
  bool finished = false;
};

}  // namespace fsl


namespace fsl::detail {
namespace {

/// Bytes requested per buffered read. A frame larger than this is fetched with
/// a second, larger read rather than being mistaken for corruption.
constexpr std::size_t kReadChunk = 256u * 1024u;

}  // namespace

SequentialFrameReader::SequentialFrameReader(
    std::shared_ptr<const std::vector<std::shared_ptr<SegmentDescriptor>>> segments,
    std::uint64_t segment_index,
    std::uint64_t offset,
    std::uint64_t first_sequence,
    std::uint64_t last_sequence)
    : segments_(std::move(segments)),
      offset_(offset),
      next_sequence_(first_sequence),
      last_sequence_(last_sequence) {
  segment_position_ = segments_->size();
  for (std::size_t i = 0; i < segments_->size(); ++i) {
    if ((*segments_)[i]->index == segment_index) {
      segment_position_ = i;
      break;
    }
  }
  exhausted_ = segment_position_ >= segments_->size() || next_sequence_ > last_sequence_;
}

bool SequentialFrameReader::refill() {
  // Reads a whole chunk starting at the current offset, growing the request
  // until it can hold the next frame. A frame that straddles a chunk boundary
  // is therefore never mistaken for corruption.
  while (segment_position_ < segments_->size()) {
    const auto& descriptor = (*segments_)[segment_position_];
    const std::uint64_t end = descriptor->end_offset.load(std::memory_order_acquire);
    if (offset_ + kFrameOverhead > end) {
      ++segment_position_;
      if (segment_position_ < segments_->size()) {
        offset_ = kSegmentHeaderSize;
      }
      continue;
    }
    std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(end - offset_, kReadChunk));
    std::uint32_t declared = 0;
    while (true) {
      buffer_.assign(want, 0);
      auto read = descriptor->file.read_at(offset_, buffer_);
      if (!read.has_value() || read.value() != want) {
        return false;
      }
      buffer_used_ = read.value();
      buffer_position_ = 0;
      for (int i = 0; i < 4; ++i) {
        declared |= static_cast<std::uint32_t>(buffer_[static_cast<std::size_t>(i)]) << (8 * i);
      }
      if (declared <= buffer_used_ || static_cast<std::uint64_t>(want) >= end - offset_) {
        break;
      }
      if (declared > kMaxFrameSize) {
        return false;
      }
      // Grow only as far as the declared frame needs, bounded by the bytes that
      // actually remain, so a corrupt declared size cannot force a huge read.
      const std::uint64_t remaining = end - offset_;
      want = static_cast<std::size_t>(
          std::min<std::uint64_t>(remaining, std::max<std::uint64_t>(declared, kReadChunk)));
    }
    return true;
  }
  return false;
}

bool SequentialFrameReader::next(std::optional<EventEnvelope>& out, Status& error) {
  out.reset();
  while (!exhausted_) {
    if (next_sequence_ > last_sequence_) {
      exhausted_ = true;
      return false;
    }
    if (buffer_position_ + kFrameOverhead > buffer_used_) {
      if (!refill()) {
        error = integrity_failure(ErrorCode::kCommittedRecordLost,
                                  "the committed prefix ended before ledger sequence " +
                                      std::to_string(last_sequence_) + " during replay");
        exhausted_ = true;
        return false;
      }
    }
    const std::span<const std::uint8_t> available(buffer_.data() + buffer_position_,
                                                  buffer_used_ - buffer_position_);
    DecodedFrame frame;
    const FrameDecodeOutcome outcome = decode_frame(available, frame);
    if (outcome == FrameDecodeOutcome::kIncomplete) {
      // A frame that reaches past the end of this chunk. `offset_` already names
      // the start of that frame, so the retry re-reads from there with a buffer
      // large enough to hold it, without moving the read position.
      std::uint32_t need = 0;
      for (std::size_t i = 0; i < 4 && i < available.size(); ++i) {
        need |= static_cast<std::uint32_t>(available[i]) << (8 * i);
      }
      buffer_used_ = 0;
      buffer_position_ = 0;
      if (!refill() || need > buffer_used_) {
        error = integrity_failure(ErrorCode::kCommittedRecordLost,
                                  "the committed prefix ended inside a frame at offset " +
                                      std::to_string(offset_));
        exhausted_ = true;
        return false;
      }
      continue;
    }
    if (outcome != FrameDecodeOutcome::kOk) {
      error = integrity_failure(outcome == FrameDecodeOutcome::kUnsupportedVersion
                                    ? ErrorCode::kUnsupportedFormatVersion
                                    : ErrorCode::kMidFileCorruption,
                                "replay encountered an invalid frame at offset " +
                                    std::to_string(offset_ + buffer_position_));
      exhausted_ = true;
      return false;
    }
    if (frame.header.kind != FrameKind::kEvent) {
      // A segment seal frame ends the segment; the next event lives in the
      // segment that follows. Skipping it is not skipping a record.
      offset_ += frame.header.frame_size;
      ++segment_position_;
      buffer_used_ = 0;
      buffer_position_ = 0;
      if (segment_position_ < segments_->size()) {
        offset_ = kSegmentHeaderSize;
      }
      continue;
    }
    if (frame.header.ledger_sequence != next_sequence_) {
      error = integrity_failure(ErrorCode::kSequenceForgery,
                                "replay expected ledger sequence " + std::to_string(next_sequence_) +
                                    " but found " + std::to_string(frame.header.ledger_sequence));
      exhausted_ = true;
      return false;
    }
    auto envelope = decode_envelope(frame.body);
    if (!envelope.has_value()) {
      error = envelope.status();
      exhausted_ = true;
      return false;
    }
    buffer_position_ += frame.header.frame_size;
    offset_ += frame.header.frame_size;
    ++next_sequence_;
    out = std::move(envelope).value();
    return true;
  }
  return false;
}

Result<ReplayStream> LedgerImpl::make_replay(const ReplayRequest& request) const {
  const std::uint64_t upper = committed_sequence_.load(std::memory_order_acquire);
  if (upper == 0) {
    return not_found(ErrorCode::kNotFound, "the ledger holds no committed events to replay");
  }
  if (request.from.has_value() && request.to.has_value() && request.from->value() > request.to->value()) {
    return invalid_argument(ErrorCode::kRangeInvalid,
                            "the replay range starts after it ends: " +
                                std::to_string(request.from->value()) + " > " +
                                std::to_string(request.to->value()));
  }
  const std::uint64_t from = request.from.has_value() ? request.from->value() : 1;
  const std::uint64_t to = request.to.has_value() ? std::min(request.to->value(), upper) : upper;
  if (from == 0) {
    return invalid_argument(ErrorCode::kRangeInvalid, "a replay cannot start at ledger sequence zero");
  }
  if (from > to || from > upper) {
    return invalid_argument(ErrorCode::kRangeInvalid,
                            "the replay range begins at " + std::to_string(from) +
                                " which is beyond its end or the commit watermark");
  }

  auto located = locate(LedgerSequence(from));
  if (!located.has_value()) {
    return located.status();
  }
  auto descriptor = segment_for(from);
  if (descriptor == nullptr) {
    return integrity_failure(ErrorCode::kSegmentMissing, "the replay start segment is not open");
  }

  auto impl = std::make_unique<ReplayStream::Impl>();
  impl->ledger = std::const_pointer_cast<LedgerImpl>(shared_from_this());
  impl->request = request;
  impl->upper = to;
  impl->first = from;
  impl->reader = std::make_unique<SequentialFrameReader>(segment_snapshot(), descriptor->index,
                                                         located->offset, from, to);
  return ReplayStream(std::move(impl));
}

Result<std::uint64_t> LedgerImpl::replay_each(const ReplayRequest& request,
                                              const ReplayVisitor& visitor) const {
  auto stream = make_replay(request);
  if (!stream.has_value()) {
    return stream.status();
  }
  std::uint64_t delivered = 0;
  std::optional<EventEnvelope> envelope;
  while (true) {
    const ReplayStep step = stream.value().next(envelope);
    if (step == ReplayStep::kEnd) {
      return delivered;
    }
    if (step == ReplayStep::kError) {
      return stream.value().error();
    }
    ++delivered;
    if (!visitor(*envelope)) {
      return delivered;
    }
  }
}

}  // namespace fsl::detail

namespace fsl {

ReplayStream::ReplayStream(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ReplayStream::ReplayStream(ReplayStream&& other) noexcept : impl_(std::move(other.impl_)) {}

ReplayStream& ReplayStream::operator=(ReplayStream&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
  }
  return *this;
}

ReplayStream::~ReplayStream() = default;

ReplayStep ReplayStream::next(std::optional<EventEnvelope>& out) {
  out.reset();
  if (impl_ == nullptr) {
    return ReplayStep::kEnd;
  }
  if (impl_->finished) {
    return ReplayStep::kEnd;
  }
  while (true) {
    if (impl_->request.max_events != 0 && impl_->delivered >= impl_->request.max_events) {
      impl_->finished = true;
      return ReplayStep::kEnd;
    }
    std::optional<EventEnvelope> envelope;
    Status status = Status::ok();
    if (!impl_->reader->next(envelope, status)) {
      impl_->finished = true;
      if (status.is_error()) {
        impl_->error = status;
        return ReplayStep::kError;
      }
      return ReplayStep::kEnd;
    }
    const EventEnvelope& candidate = *envelope;
    if (impl_->request.kind.has_value() && candidate.kind() != *impl_->request.kind) {
      continue;
    }
    if (impl_->request.subject.has_value() && !(candidate.subject() == *impl_->request.subject)) {
      continue;
    }
    if (impl_->request.source.has_value() && !(candidate.provenance().source() == *impl_->request.source)) {
      continue;
    }
    ++impl_->delivered;
    out = std::move(envelope);
    return ReplayStep::kEvent;
  }
}

const Status& ReplayStream::error() const noexcept {
  static const Status kNoError = Status::ok();
  if (impl_ == nullptr) {
    return kNoError;
  }
  return impl_->error;
}

std::optional<LedgerSequence> ReplayStream::next_sequence() const noexcept {
  if (impl_ == nullptr || impl_->finished || !impl_->reader) {
    return std::nullopt;
  }
  const std::uint64_t next = impl_->reader->next_sequence();
  if (impl_->upper.has_value() && next > *impl_->upper) {
    return std::nullopt;
  }
  return LedgerSequence(next);
}

std::optional<LedgerSequence> ReplayStream::upper_bound() const noexcept {
  if (impl_ == nullptr || !impl_->upper.has_value()) {
    return std::nullopt;
  }
  return LedgerSequence(*impl_->upper);
}

std::uint64_t ReplayStream::delivered() const noexcept {
  return impl_ == nullptr ? 0 : impl_->delivered;
}

}  // namespace fsl

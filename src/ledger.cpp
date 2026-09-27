// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fsl/ledger.hpp"

#include <utility>

#include "detail/ledger_impl.hpp"

/// \file ledger.cpp
/// The public handle. Every method forwards to the shared implementation, which
/// outlives the handle when a replay stream still refers to it.

namespace fsl {

Ledger::Ledger(std::shared_ptr<detail::LedgerImpl> impl) noexcept : impl_(std::move(impl)) {}

Ledger::Ledger(Ledger&& other) noexcept : impl_(std::move(other.impl_)) {}

Ledger& Ledger::operator=(Ledger&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
  }
  return *this;
}

Ledger::~Ledger() {
  if (impl_ != nullptr) {
    // A handle that is destroyed without an explicit close still releases the
    // writer lock and flushes derived state. A failure here cannot be reported,
    // which is exactly why close() exists and is documented.
    (void)impl_->close();
  }
}

Result<Ledger> Ledger::open(const std::filesystem::path& directory, OpenMode mode, LedgerOptions options) {
  auto impl = detail::LedgerImpl::open(directory, mode, std::move(options));
  if (!impl.has_value()) {
    return impl.status();
  }
  return Ledger(std::move(impl).value());
}

Result<Ledger> Ledger::create(const std::filesystem::path& directory, LedgerOptions options) {
  return open(directory, OpenMode::kCreate, std::move(options));
}

Result<LedgerIdentity> Ledger::identity() const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->identity();
}

Result<LedgerState> Ledger::state() const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->state();
}

Result<CommitWatermark> Ledger::watermark() const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->watermark();
}

LedgerStats Ledger::stats() const {
  if (impl_ == nullptr) {
    return LedgerStats{};
  }
  return impl_->stats();
}

const OpenReport& Ledger::open_report() const noexcept {
  static const OpenReport kEmpty{};
  if (impl_ == nullptr) {
    return kEmpty;
  }
  return impl_->open_report();
}

bool Ledger::is_open() const noexcept { return impl_ != nullptr && impl_->is_open(); }

bool Ledger::is_writable() const noexcept { return impl_ != nullptr && impl_->is_writable(); }

bool Ledger::is_durable() const noexcept { return impl_ != nullptr && impl_->is_durable(); }

std::filesystem::path Ledger::directory() const {
  return impl_ == nullptr ? std::filesystem::path{} : impl_->root();
}

Result<AppendOutcome> Ledger::append(const SubmittedObservation& observation) {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->append(observation);
}

Result<BatchOutcome> Ledger::append_batch(std::span<const SubmittedObservation> observations) {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->append_batch(observations);
}

Result<void> Ledger::rotate_segment() {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->rotate_segment();
}

Result<CheckpointInfo> Ledger::create_checkpoint() {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->create_checkpoint();
}

Result<void> Ledger::flush() {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->flush();
}

Result<void> Ledger::rebuild_index() {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->rebuild_index();
}

RecoveryReport Ledger::recover(const RecoveryRequest& request) {
  if (impl_ == nullptr) {
    RecoveryReport report;
    report.status = closed(ErrorCode::kLedgerClosed, "the handle is not open");
    return report;
  }
  return impl_->recover(request);
}

Result<EventEnvelope> Ledger::read(LedgerSequence sequence) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->read(sequence);
}

Result<EventEnvelope> Ledger::find_event(EventId event_id) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->find_event(event_id);
}

Result<EventEnvelope> Ledger::find_idempotency(IdempotencyToken token) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->find_idempotency(token);
}

Result<QueryPage> Ledger::query(const Query& request) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->query(request);
}

Result<ReplayStream> Ledger::replay(const ReplayRequest& request) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->make_replay(request);
}

Result<std::uint64_t> Ledger::replay_each(const ReplayRequest& request, const ReplayVisitor& visitor) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->replay_each(request, visitor);
}

VerifyReport Ledger::verify(const VerifyRequest& request) const {
  if (impl_ == nullptr) {
    VerifyReport report;
    report.status = closed(ErrorCode::kLedgerClosed, "the handle is not open");
    return report;
  }
  return impl_->verify(request);
}

Result<std::vector<SegmentInfo>> Ledger::segments() const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->segments();
}

Result<SegmentInfo> Ledger::segment(SegmentIndex index) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->segment(index);
}

Result<std::vector<CheckpointInfo>> Ledger::checkpoints() const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->checkpoints();
}

Result<CheckpointInfo> Ledger::latest_checkpoint() const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->latest_checkpoint();
}

Result<SubjectView> Ledger::subject(const SubjectRef& reference) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->subject(reference);
}

Result<std::vector<SubjectView>> Ledger::subjects(std::size_t limit,
                                                  const std::optional<SubjectRef>& after) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->subjects(limit, after);
}

Result<std::vector<SourceView>> Ledger::sources(std::size_t limit) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->sources(limit);
}

Result<void> Ledger::export_audit(const AuditExportRequest& request, IAuditSink& sink) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->export_audit(request, sink);
}

Result<std::string> Ledger::export_audit_json(const AuditExportRequest& request) const {
  if (impl_ == nullptr) {
    return closed(ErrorCode::kLedgerClosed, "the handle is not open");
  }
  return impl_->export_audit_json(request);
}

Result<void> Ledger::close() {
  if (impl_ == nullptr) {
    return Status::ok();
  }
  return impl_->close();
}

}  // namespace fsl

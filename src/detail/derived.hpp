// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "detail/frame.hpp"
#include "fsl/counter.hpp"
#include "fsl/ids.hpp"
#include "fsl/options.hpp"
#include "fsl/status.hpp"

/// \file derived.hpp
/// The derived index: postings and duplicate-detection identities.
///
/// Everything in this file is derived from the authoritative log and is rebuilt
/// from it. Nothing here is authoritative: a posting is used only to narrow a
/// search, and the record a posting names is always re-read and re-validated
/// from the ledger before it is returned to a caller. That is what makes it
/// impossible for an index to contradict the log.

namespace fsl::detail {

/// One index posting: where an indexed property was observed.
struct Posting {
  std::uint64_t sequence = 0;
  /// Event-kind dependent: the EventKind for subject/epoch/generation/event-kind
  /// postings, the SourceGeneration for source postings.
  std::uint64_t extra = 0;
  /// SourceSequence for source postings; zero elsewhere.
  std::uint64_t aux = 0;
};

/// How an index mutation was refused.
struct IndexReject {
  ErrorCode code = ErrorCode::kOk;
  std::string message;
};

/// Postings and identity maps rebuilt from the authoritative log.
///
/// Bounds are enforced before insertion. Exceeding the posting budget stops
/// adding postings and marks the index incomplete, which makes affected queries
/// fall back to a bounded linear scan: a query is slower, never wrong.
/// Exceeding the identity budget is reported to the caller, which rejects the
/// append, because forgetting an identity would silently weaken duplicate
/// detection.
class DerivedIndex {
 public:
  explicit DerivedIndex(const LedgerOptions& options);

  [[nodiscard]] std::optional<IndexReject> add_posting(IndexKind kind,
                                                       std::span<const std::uint8_t> key,
                                                       const Posting& posting);
  [[nodiscard]] std::optional<IndexReject> add_identity(const EventId& identity, LedgerSequence sequence);
  [[nodiscard]] std::optional<IndexReject> add_token(const IdempotencyToken& token, LedgerSequence sequence);

  /// Capacity probes used while planning a commit, before anything is written.
  [[nodiscard]] std::optional<IndexReject> check_posting_capacity(IndexKind kind,
                                                                  std::span<const std::uint8_t> key) const;
  [[nodiscard]] std::optional<IndexReject> check_event_identity(const EventId& identity) const;
  [[nodiscard]] std::optional<IndexReject> check_token_identity(const IdempotencyToken& token) const;

  [[nodiscard]] const std::vector<Posting>* postings(IndexKind kind, std::span<const std::uint8_t> key) const;
  [[nodiscard]] std::optional<LedgerSequence> event_sequence(const EventId& identity) const;
  [[nodiscard]] std::optional<LedgerSequence> token_sequence(const IdempotencyToken& token) const;

  [[nodiscard]] std::size_t posting_count() const noexcept { return posting_count_; }
  [[nodiscard]] std::size_t identity_count() const noexcept { return identities_.size() + tokens_.size(); }
  [[nodiscard]] std::size_t distinct_keys(IndexKind kind) const noexcept;
  /// Every key currently held for one kind, in unspecified order.
  [[nodiscard]] std::vector<std::vector<std::uint8_t>> keys(IndexKind kind) const;
  [[nodiscard]] bool postings_complete() const noexcept { return postings_complete_; }
  [[nodiscard]] std::size_t posting_budget() const noexcept { return posting_budget_; }
  [[nodiscard]] std::size_t identity_budget() const noexcept { return identity_budget_; }

  void clear();
  /// Forgets postings for one kind from `from_sequence` upwards. Used when the
  /// index is found to be ahead of the commit watermark.
  void drop_postings_above(std::uint64_t sequence);

 private:
  struct PostingKey {
    IndexKind kind = IndexKind::kEventId;
    std::vector<std::uint8_t> bytes;
  };
  struct PostingKeyHash {
    [[nodiscard]] std::size_t operator()(const PostingKey& key) const noexcept;
  };
  struct PostingKeyEqual {
    [[nodiscard]] bool operator()(const PostingKey& lhs, const PostingKey& rhs) const noexcept;
  };

  [[nodiscard]] static std::size_t kind_index(IndexKind kind) noexcept;

  std::unordered_map<PostingKey, std::vector<Posting>, PostingKeyHash, PostingKeyEqual> postings_;
  std::unordered_map<EventId, std::uint64_t> identities_;
  std::unordered_map<IdempotencyToken, std::uint64_t> tokens_;
  std::array<std::size_t, 9> key_counts_{};
  std::array<std::size_t, 9> key_limits_{};
  std::size_t posting_count_ = 0;
  std::size_t posting_budget_ = 0;
  std::size_t identity_budget_ = 0;
  bool postings_complete_ = true;
};

/// Derived admission state that is constant-size and folded in commit order.
struct DerivedState {
  std::uint64_t facility_generation = 1;
  /// 0 encodes "no epoch is open"; the public API models absence with optional.
  std::uint64_t open_epoch = 0;
  std::uint64_t latest_epoch = 0;

  [[nodiscard]] std::optional<FacilityEpoch> open_epoch_value() const noexcept;
  [[nodiscard]] std::optional<FacilityEpoch> latest_epoch_value() const noexcept;
};

}  // namespace fsl::detail

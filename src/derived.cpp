// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "detail/derived.hpp"

#include <algorithm>

namespace fsl::detail {
namespace {

/// Per-kind ceilings derived from the caller's options. The subject, source and
/// relationship key spaces are the ones that grow with the facility model, so
/// they carry explicit limits; the remaining kinds are bounded by the posting
/// budget alone.
[[nodiscard]] std::array<std::size_t, 9> limits_from(const LedgerOptions& options) {
  std::array<std::size_t, 9> limits{};
  limits.fill(options.max_index_postings);
  limits[static_cast<std::size_t>(IndexKind::kSubject)] = options.max_subjects;
  limits[static_cast<std::size_t>(IndexKind::kSource)] = options.max_sources;
  return limits;
}

}  // namespace

std::size_t DerivedIndex::kind_index(IndexKind kind) noexcept {
  const auto raw = static_cast<std::size_t>(kind);
  return raw < 9 ? raw : 0;
}

std::size_t DerivedIndex::PostingKeyHash::operator()(const PostingKey& key) const noexcept {
  // FNV-1a over the discriminant and the key bytes. The hash is never persisted
  // and never compared across runs.
  std::uint64_t mixed = 1469598103934665603ULL;
  mixed ^= static_cast<std::uint64_t>(key.kind);
  mixed *= 1099511628211ULL;
  for (const std::uint8_t byte : key.bytes) {
    mixed ^= static_cast<std::uint64_t>(byte);
    mixed *= 1099511628211ULL;
  }
  return static_cast<std::size_t>(mixed ^ (mixed >> 32));
}

bool DerivedIndex::PostingKeyEqual::operator()(const PostingKey& lhs, const PostingKey& rhs) const noexcept {
  return lhs.kind == rhs.kind && lhs.bytes == rhs.bytes;
}

DerivedIndex::DerivedIndex(const LedgerOptions& options)
    : key_limits_(limits_from(options)),
      posting_budget_(options.max_index_postings),
      identity_budget_(options.max_dedupe_entries) {}

std::size_t DerivedIndex::distinct_keys(IndexKind kind) const noexcept {
  return key_counts_[kind_index(kind)];
}

std::optional<IndexReject> DerivedIndex::add_posting(IndexKind kind,
                                                     std::span<const std::uint8_t> key,
                                                     const Posting& posting) {
  if (posting.sequence == 0) {
    return IndexReject{ErrorCode::kHeaderFieldInvalid, "an index posting must name a ledger sequence"};
  }
  PostingKey lookup;
  lookup.kind = kind;
  lookup.bytes.assign(key.begin(), key.end());
  const auto found = postings_.find(lookup);
  if (found == postings_.end()) {
    if (key_counts_[kind_index(kind)] >= key_limits_[kind_index(kind)]) {
      return IndexReject{ErrorCode::kCapacityLimitExceeded,
                         "the derived index already tracks the configured maximum number of " +
                             std::string(to_string(kind)) + " keys"};
    }
  }
  if (posting_count_ >= posting_budget_) {
    // A posting is a locator, not authoritative state. Dropping it makes bounded
    // queries fall back to a linear scan and is reported through the statistics;
    // it can never change an answer.
    postings_complete_ = false;
    return std::nullopt;
  }
  if (found == postings_.end()) {
    ++key_counts_[kind_index(kind)];
    std::vector<Posting> fresh;
    fresh.push_back(posting);
    postings_.emplace(std::move(lookup), std::move(fresh));
    ++posting_count_;
    return std::nullopt;
  }
  std::vector<Posting>& list = found->second;
  if (!list.empty() && list.back().sequence == posting.sequence) {
    // The same property observed twice for one event: keep the first, ignore the
    // repeat. This happens when a rebuilt index overlaps a partially written one.
    return std::nullopt;
  }
  list.push_back(posting);
  ++posting_count_;
  return std::nullopt;
}

std::optional<IndexReject> DerivedIndex::add_identity(const EventId& identity, LedgerSequence sequence) {
  const auto found = identities_.find(identity);
  if (found != identities_.end()) {
    return std::nullopt;
  }
  if (identity_count() >= identity_budget_) {
    return IndexReject{ErrorCode::kCapacityLimitExceeded,
                       "duplicate detection is at capacity: raise max_dedupe_entries to record more "
                       "event identities"};
  }
  identities_.emplace(identity, sequence.value());
  return std::nullopt;
}

std::optional<IndexReject> DerivedIndex::add_token(const IdempotencyToken& token, LedgerSequence sequence) {
  const auto found = tokens_.find(token);
  if (found != tokens_.end()) {
    return std::nullopt;
  }
  if (identity_count() >= identity_budget_) {
    return IndexReject{ErrorCode::kCapacityLimitExceeded,
                       "duplicate detection is at capacity: raise max_dedupe_entries to record more "
                       "idempotency tokens"};
  }
  tokens_.emplace(token, sequence.value());
  return std::nullopt;
}

std::optional<IndexReject> DerivedIndex::check_posting_capacity(IndexKind kind,
                                                                std::span<const std::uint8_t> key) const {
  PostingKey lookup;
  lookup.kind = kind;
  lookup.bytes.assign(key.begin(), key.end());
  if (postings_.find(lookup) != postings_.end()) {
    return std::nullopt;
  }
  if (key_counts_[kind_index(kind)] >= key_limits_[kind_index(kind)]) {
    return IndexReject{ErrorCode::kCapacityLimitExceeded,
                       "the derived index already tracks the configured maximum number of " +
                           std::string(to_string(kind)) + " keys"};
  }
  return std::nullopt;
}

std::optional<IndexReject> DerivedIndex::check_event_identity(const EventId& identity) const {
  if (identities_.find(identity) != identities_.end()) {
    return std::nullopt;
  }
  if (identity_count() >= identity_budget_) {
    return IndexReject{ErrorCode::kCapacityLimitExceeded,
                       "duplicate detection is at capacity: raise max_dedupe_entries to record more "
                       "event identities"};
  }
  return std::nullopt;
}

std::optional<IndexReject> DerivedIndex::check_token_identity(const IdempotencyToken& token) const {
  if (tokens_.find(token) != tokens_.end()) {
    return std::nullopt;
  }
  if (identity_count() >= identity_budget_) {
    return IndexReject{ErrorCode::kCapacityLimitExceeded,
                       "duplicate detection is at capacity: raise max_dedupe_entries to record more "
                       "idempotency tokens"};
  }
  return std::nullopt;
}

const std::vector<Posting>* DerivedIndex::postings(IndexKind kind, std::span<const std::uint8_t> key) const {
  PostingKey lookup;
  lookup.kind = kind;
  lookup.bytes.assign(key.begin(), key.end());
  const auto found = postings_.find(lookup);
  if (found == postings_.end()) {
    return nullptr;
  }
  return &found->second;
}

std::optional<LedgerSequence> DerivedIndex::event_sequence(const EventId& identity) const {
  const auto found = identities_.find(identity);
  if (found == identities_.end()) {
    return std::nullopt;
  }
  return LedgerSequence(found->second);
}

std::optional<LedgerSequence> DerivedIndex::token_sequence(const IdempotencyToken& token) const {
  const auto found = tokens_.find(token);
  if (found == tokens_.end()) {
    return std::nullopt;
  }
  return LedgerSequence(found->second);
}

std::vector<std::vector<std::uint8_t>> DerivedIndex::keys(IndexKind kind) const {
  std::vector<std::vector<std::uint8_t>> result;
  result.reserve(key_counts_[kind_index(kind)]);
  for (const auto& entry : postings_) {
    if (entry.first.kind == kind) {
      result.push_back(entry.first.bytes);
    }
  }
  return result;
}

void DerivedIndex::clear() {
  postings_.clear();
  identities_.clear();
  tokens_.clear();
  key_counts_.fill(0);
  posting_count_ = 0;
  postings_complete_ = true;
}

void DerivedIndex::drop_postings_above(std::uint64_t sequence) {
  for (auto entry = postings_.begin(); entry != postings_.end();) {
    std::vector<Posting>& list = entry->second;
    const auto first_above =
        std::find_if(list.begin(), list.end(), [sequence](const Posting& posting) {
          return posting.sequence > sequence;
        });
    if (first_above == list.end()) {
      ++entry;
      continue;
    }
    const std::size_t removed = static_cast<std::size_t>(std::distance(first_above, list.end()));
    list.erase(first_above, list.end());
    posting_count_ -= std::min(posting_count_, removed);
    if (list.empty()) {
      --key_counts_[kind_index(entry->first.kind)];
      entry = postings_.erase(entry);
      continue;
    }
    ++entry;
  }
  for (auto entry = identities_.begin(); entry != identities_.end();) {
    if (entry->second > sequence) {
      entry = identities_.erase(entry);
      continue;
    }
    ++entry;
  }
  for (auto entry = tokens_.begin(); entry != tokens_.end();) {
    if (entry->second > sequence) {
      entry = tokens_.erase(entry);
      continue;
    }
    ++entry;
  }
}

std::optional<FacilityEpoch> DerivedState::open_epoch_value() const noexcept {
  if (open_epoch == 0) {
    return std::nullopt;
  }
  return FacilityEpoch(open_epoch);
}

std::optional<FacilityEpoch> DerivedState::latest_epoch_value() const noexcept {
  if (latest_epoch == 0) {
    return std::nullopt;
  }
  return FacilityEpoch(latest_epoch);
}

}  // namespace fsl::detail

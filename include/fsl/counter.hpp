// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>

/// \file counter.hpp
/// Strongly typed monotonic counters.
///
/// Ledger sequences, facility generations, facility epochs, source generations,
/// logical ticks, segment indexes and manifest generations all share the same
/// representation but are unrelated concepts. Instantiating Counter with a
/// distinct tag makes them distinct types with no implicit conversion between
/// them, which is what keeps a generation from being passed where an epoch is
/// expected. There is no default constructor: absence is modelled with
/// std::optional, never with a zero sentinel.

namespace fsl {

/// A monotonic counter of `Rep`, distinguished from every other counter by `Tag`.
template <class Tag, class Rep = std::uint64_t>
class Counter {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr explicit Counter(Rep value) noexcept : value_(value) {}

  /// The smallest value this counter is ever assigned by the ledger.
  [[nodiscard]] static constexpr Counter first() noexcept { return Counter(static_cast<Rep>(1)); }

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == static_cast<Rep>(0); }

  /// True when `next()` can be represented without overflow.
  [[nodiscard]] constexpr bool can_advance() const noexcept {
    return value_ < std::numeric_limits<Rep>::max();
  }

  /// Successor, or nothing when the counter is exhausted. Callers must use this
  /// rather than assuming an unlimited counter.
  [[nodiscard]] constexpr std::optional<Counter> try_next() const noexcept {
    if (!can_advance()) {
      return std::nullopt;
    }
    return Counter(static_cast<Rep>(value_ + static_cast<Rep>(1)));
  }

  /// Distance from `lower` to `*this`. Precondition: `lower <= *this`.
  [[nodiscard]] constexpr Rep distance_from(const Counter& lower) const noexcept {
    return static_cast<Rep>(value_ - lower.value_);
  }

  [[nodiscard]] friend constexpr bool operator==(const Counter&, const Counter&) noexcept = default;
  [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const Counter& lhs,
                                                                  const Counter& rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  Rep value_;
};

struct LedgerSequenceTag;
struct FacilityGenerationTag;
struct FacilityEpochTag;
struct SourceGenerationTag;
struct SourceSequenceTag;
struct LogicalTickTag;
struct RecordIndexTag;
struct SegmentIndexTag;
struct WriterIncarnationTag;
struct ManifestGenerationTag;
struct ReplayCursorTag;

/// Position of a committed event in the authoritative ledger order. The only
/// ordering this library guarantees is ascending LedgerSequence.
using LedgerSequence = Counter<LedgerSequenceTag>;
/// Monotonic generation of the authoritative facility model.
using FacilityGeneration = Counter<FacilityGenerationTag>;
/// Monotonic epoch within which mutation authority is held.
using FacilityEpoch = Counter<FacilityEpochTag>;
/// Incarnation of one producing component; a restart normally advances it.
using SourceGeneration = Counter<SourceGenerationTag>;
/// Optional producer-asserted ordering within one (source, source generation).
using SourceSequence = Counter<SourceSequenceTag>;
/// Ledger-assigned logical tick; increases by exactly one per accepted event.
using LogicalTick = Counter<LogicalTickTag>;
/// 1-based index of a frame within its segment.
using RecordIndex = Counter<RecordIndexTag>;
/// 1-based index of a segment within the ledger.
using SegmentIndex = Counter<SegmentIndexTag>;
/// Monotonic identifier of a writable handle's ownership of a ledger directory.
using WriterIncarnation = Counter<WriterIncarnationTag>;
/// Monotonic identifier of the published manifest publication.
using ManifestGeneration = Counter<ManifestGenerationTag>;

/// Version of a payload schema, independent of the segment format version.
using SchemaVersion = Counter<struct SchemaVersionTag, std::uint32_t>;

}  // namespace fsl

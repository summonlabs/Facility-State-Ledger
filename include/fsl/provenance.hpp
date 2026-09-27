// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fsl/counter.hpp"
#include "fsl/ids.hpp"
#include "fsl/status.hpp"

/// \file provenance.hpp
/// Attribution of an accepted event to the component that produced it.
///
/// Provenance is recorded verbatim and re-emitted by audit export. Nothing here
/// is trusted: a source can claim anything about its own clock or its own
/// ordering, and the ledger records the claim while ordering exclusively by its
/// own commit sequence.

namespace fsl {

/// One bounded, validated attributable key/value pair.
struct ProvenanceAttribute {
  std::string key;
  std::string value;

  static constexpr std::size_t kMaxKeyLength = 32;
  static constexpr std::size_t kMaxValueLength = 256;

  [[nodiscard]] friend bool operator==(const ProvenanceAttribute&, const ProvenanceAttribute&) noexcept = default;
  [[nodiscard]] friend bool operator<(const ProvenanceAttribute& lhs, const ProvenanceAttribute& rhs) noexcept {
    return lhs.key < rhs.key;
  }
};

/// Who produced an event, on which incarnation, and at which claimed position.
class ProvenanceRecord {
 public:
  static constexpr std::size_t kMaxAttributes = 16;

  /// Validates and canonicalises a provenance record.
  ///
  /// `attributes` are sorted by key; duplicate keys, out-of-domain keys, values
  /// longer than kMaxValueLength, invalid UTF-8 and control characters are
  /// rejected. Neither the source identity nor the source generation is
  /// interpreted here: admission policy in the ledger decides whether the
  /// claimed generation is still current.
  [[nodiscard]] static Result<ProvenanceRecord> create(SourceComponentId source,
                                                       SourceGeneration source_generation,
                                                       std::optional<SourceSequence> source_sequence,
                                                       std::optional<std::uint64_t> source_wall_clock_unix_nanos,
                                                       std::vector<ProvenanceAttribute> attributes);

  [[nodiscard]] const SourceComponentId& source() const noexcept { return source_; }
  [[nodiscard]] SourceGeneration source_generation() const noexcept { return source_generation_; }
  [[nodiscard]] const std::optional<SourceSequence>& source_sequence() const noexcept { return source_sequence_; }
  /// Caller-asserted wall clock. Never used for ordering, never trusted.
  [[nodiscard]] const std::optional<std::uint64_t>& source_wall_clock_unix_nanos() const noexcept {
    return source_wall_clock_unix_nanos_;
  }
  /// Attributes in ascending key order.
  [[nodiscard]] const std::vector<ProvenanceAttribute>& attributes() const noexcept { return attributes_; }

  [[nodiscard]] const std::string* find_attribute(std::string_view key) const noexcept;

 private:
  ProvenanceRecord(SourceComponentId source,
                   SourceGeneration source_generation,
                   std::optional<SourceSequence> source_sequence,
                   std::optional<std::uint64_t> source_wall_clock_unix_nanos,
                   std::vector<ProvenanceAttribute> attributes)
      : source_(std::move(source)),
        source_generation_(source_generation),
        source_sequence_(source_sequence),
        source_wall_clock_unix_nanos_(source_wall_clock_unix_nanos),
        attributes_(std::move(attributes)) {}

  SourceComponentId source_;
  SourceGeneration source_generation_;
  std::optional<SourceSequence> source_sequence_;
  std::optional<std::uint64_t> source_wall_clock_unix_nanos_;
  std::vector<ProvenanceAttribute> attributes_;
};

}  // namespace fsl

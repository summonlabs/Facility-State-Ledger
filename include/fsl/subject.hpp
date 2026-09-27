// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "fsl/status.hpp"

/// \file subject.hpp
/// The subject namespace of the ledger.
///
/// A subject is the facility-model entity a ledger event is *about*. The kinds
/// below are the physical and compositional entities this repository owns the
/// history of, plus opaque handles for objects owned by adjacent DCCP layers.
/// The ledger records that such objects were referenced; it does not implement
/// their semantics.

namespace fsl {

/// Kind of facility-model entity a subject reference designates.
enum class SubjectKind : std::uint8_t {
  /// Physical or logical enclosure that holds assets.
  kRack = 1,
  /// A managed physical asset (server, PDU, switch chassis, sensor package...).
  kAsset = 2,
  /// A named position in the facility: site, hall, row, tile, slot.
  kLocation = 3,
  /// A logical facility node in the facility composition graph.
  kFacilityNode = 4,
  /// A power distribution domain (feed, bus, branch, outlet group).
  kPowerDomain = 5,
  /// A cooling distribution domain (loop, manifold, zone).
  kCoolingDomain = 6,
  /// Opaque handle for an object owned by Accelerated Systems Infrastructure.
  kExternalAsi = 7,
  /// Opaque handle for an object owned by Distributed Fabric Infrastructure.
  kExternalDfi = 8,
  /// The ledger itself, used for ledger-owned lifecycle records.
  kLedger = 9,
};

[[nodiscard]] std::string_view to_string(SubjectKind kind) noexcept;
[[nodiscard]] std::optional<SubjectKind> subject_kind_from_string(std::string_view name) noexcept;

/// True when a subject of this kind must carry a location reference when it is
/// registered. Physical entities have a position; domains and opaque external
/// handles do not.
[[nodiscard]] bool subject_kind_requires_location(SubjectKind kind) noexcept;

/// Structural relationship kinds the ledger records between subjects. These are
/// assertions supplied by producers; the ledger validates that both endpoints
/// are known subjects and nothing more.
enum class RelationshipKind : std::uint8_t {
  kContainedIn = 1,
  kPoweredBy = 2,
  kCooledBy = 3,
  kConnectedTo = 4,
  kDependsOn = 5,
};

[[nodiscard]] std::string_view to_string(RelationshipKind kind) noexcept;
[[nodiscard]] std::optional<RelationshipKind> relationship_kind_from_string(std::string_view name) noexcept;

/// Validated subject key: 1..128 characters, lower-case canonical form.
///
/// The key never contains `..`, `\`, a path separator at either end, a control
/// character, or non-ASCII text, so a key is always safe to place in a file
/// name, a URL path segment, or an audit record without escape logic.
class SubjectKey {
 public:
  static constexpr std::size_t kMaxLength = 128;

  [[nodiscard]] static Result<SubjectKey> parse(std::string_view text);
  [[nodiscard]] static bool is_valid(std::string_view text) noexcept;

  [[nodiscard]] const std::string& str() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }

  [[nodiscard]] friend bool operator==(const SubjectKey&, const SubjectKey&) noexcept = default;
  [[nodiscard]] friend bool operator<(const SubjectKey& lhs, const SubjectKey& rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }

 private:
  SubjectKey() = default;
  explicit SubjectKey(std::string value) : value_(std::move(value)) {}
  std::string value_;
};

/// A typed reference to one facility-model subject.
class SubjectRef {
 public:
  [[nodiscard]] static Result<SubjectRef> create(SubjectKind kind, std::string_view key);
  [[nodiscard]] static Result<SubjectRef> parse(std::string_view text);

  [[nodiscard]] SubjectKind kind() const noexcept { return kind_; }
  [[nodiscard]] const SubjectKey& key() const noexcept { return key_; }

  /// Canonical textual form: `<kind>/<key>`, for example `rack/hall-a.row-3`.
  [[nodiscard]] std::string to_string() const;

  /// Total order used everywhere iteration order is public or serialized.
  [[nodiscard]] friend bool operator==(const SubjectRef&, const SubjectRef&) noexcept = default;
  [[nodiscard]] friend bool operator<(const SubjectRef& lhs, const SubjectRef& rhs) noexcept {
    if (lhs.kind_ != rhs.kind_) {
      return static_cast<std::uint8_t>(lhs.kind_) < static_cast<std::uint8_t>(rhs.kind_);
    }
    return lhs.key_ < rhs.key_;
  }

 private:
  SubjectRef(SubjectKind kind, SubjectKey key) : kind_(kind), key_(std::move(key)) {}

  SubjectKind kind_ = SubjectKind::kRack;
  SubjectKey key_;
};

}  // namespace fsl

namespace std {

template <>
struct hash<fsl::SubjectKey> {
  [[nodiscard]] std::size_t operator()(const fsl::SubjectKey& key) const noexcept {
    return hash<string_view>{}(key.view());
  }
};

template <>
struct hash<fsl::SubjectRef> {
  [[nodiscard]] std::size_t operator()(const fsl::SubjectRef& ref) const noexcept {
    std::size_t seed = static_cast<std::size_t>(ref.kind());
    seed ^= hash<string_view>{}(ref.key().view()) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
  }
};

}  // namespace std

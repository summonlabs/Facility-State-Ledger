// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "fsl/status.hpp"
#include "fsl/text.hpp"

/// \file ids.hpp
/// Stable identities used across the ledger trust boundary.
///
/// All identities are value types with construction-time validation. Nothing in
/// this file converts implicitly to a string, a byte buffer, or another
/// identity type: an EventId can never be passed where a LedgerId is expected.

namespace fsl {

/// A 128-bit opaque identity. `Tag` makes each instantiation a distinct type.
///
/// The default-constructed value is the *nil* identity. The ledger never
/// assigns it, every admission path rejects it, and it exists only so that
/// containers and value members can be declared.
template <class Tag>
class BasicId128 {
 public:
  static constexpr std::size_t kByteSize = 16;
  static constexpr std::size_t kHexSize = 32;

  constexpr BasicId128() noexcept = default;

  [[nodiscard]] static constexpr BasicId128 from_words(std::uint64_t high, std::uint64_t low) noexcept {
    BasicId128 id;
    for (std::size_t i = 0; i < 8; ++i) {
      id.bytes_[i] = static_cast<std::uint8_t>((high >> ((7u - i) * 8u)) & 0xFFu);
      id.bytes_[i + 8] = static_cast<std::uint8_t>((low >> ((7u - i) * 8u)) & 0xFFu);
    }
    return id;
  }

  /// Copies exactly kByteSize bytes from `data`.
  [[nodiscard]] static BasicId128 from_bytes(const std::uint8_t* data) noexcept {
    BasicId128 id;
    for (std::size_t i = 0; i < kByteSize; ++i) {
      id.bytes_[i] = data[i];
    }
    return id;
  }

  /// Copies exactly kByteSize bytes.
  [[nodiscard]] static BasicId128 from_span(std::span<const std::uint8_t> bytes) {
    BasicId128 id;
    if (bytes.size() != kByteSize) {
      return id;
    }
    for (std::size_t i = 0; i < kByteSize; ++i) {
      id.bytes_[i] = bytes[i];
    }
    return id;
  }

  /// Strictly parses exactly 32 hexadecimal characters (either case).
  [[nodiscard]] static std::optional<BasicId128> from_hex(std::string_view text) noexcept {
    if (text.size() != kHexSize) {
      return std::nullopt;
    }
    BasicId128 id;
    std::span<std::uint8_t> target(id.bytes_.data(), kByteSize);
    if (!from_hex_into(text, target)) {
      return std::nullopt;
    }
    return id;
  }

  [[nodiscard]] std::string to_hex() const { return fsl::to_hex(std::span<const std::uint8_t>(bytes_)); }
  [[nodiscard]] const std::array<std::uint8_t, kByteSize>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes_.data(); }
  [[nodiscard]] static constexpr std::size_t size() noexcept { return kByteSize; }

  /// The high 64 bits, in the order from_words accepts them.
  [[nodiscard]] std::uint64_t high() const noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      value = (value << 8) | static_cast<std::uint64_t>(bytes_[i]);
    }
    return value;
  }

  /// The low 64 bits, in the order from_words accepts them.
  [[nodiscard]] std::uint64_t low() const noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 8; i < kByteSize; ++i) {
      value = (value << 8) | static_cast<std::uint64_t>(bytes_[i]);
    }
    return value;
  }
  [[nodiscard]] bool is_nil() const noexcept {
    for (const std::uint8_t byte : bytes_) {
      if (byte != 0) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] friend constexpr bool operator==(const BasicId128&, const BasicId128&) noexcept = default;
  [[nodiscard]] friend bool operator<(const BasicId128& lhs, const BasicId128& rhs) noexcept {
    return lhs.bytes_ < rhs.bytes_;
  }

 private:
  std::array<std::uint8_t, kByteSize> bytes_{};
};

struct EventIdTag;
struct LedgerIdTag;
struct IdempotencyTokenTag;

/// Caller-chosen identity of a submitted event. Reuse with different content is
/// a conflict, not a duplicate.
using EventId = BasicId128<EventIdTag>;
/// Identity of one ledger directory, assigned once at creation.
using LedgerId = BasicId128<LedgerIdTag>;
/// Optional caller-chosen retry token, independent of EventId.
using IdempotencyToken = BasicId128<IdempotencyTokenTag>;

/// Generates a fresh random LedgerId from the operating system entropy source.
[[nodiscard]] Result<LedgerId> generate_ledger_id();

/// A canonical lower-case component identity, 1..96 characters of `[a-z0-9._-]`.
class SourceComponentId {
 public:
  static constexpr std::size_t kMaxLength = 96;

  [[nodiscard]] static Result<SourceComponentId> parse(std::string_view text);
  [[nodiscard]] static bool is_valid(std::string_view text) noexcept;

  [[nodiscard]] const std::string& str() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }

  [[nodiscard]] friend bool operator==(const SourceComponentId&, const SourceComponentId&) noexcept = default;
  [[nodiscard]] friend bool operator<(const SourceComponentId& lhs, const SourceComponentId& rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }

 private:
  SourceComponentId() = default;
  explicit SourceComponentId(std::string value) : value_(std::move(value)) {}
  std::string value_;
};

/// A canonical lower-case payload schema identity, 1..64 characters.
class SchemaId {
 public:
  static constexpr std::size_t kMaxLength = 64;

  [[nodiscard]] static Result<SchemaId> parse(std::string_view text);
  [[nodiscard]] static bool is_valid(std::string_view text) noexcept;

  [[nodiscard]] const std::string& str() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }

  [[nodiscard]] friend bool operator==(const SchemaId&, const SchemaId&) noexcept = default;
  [[nodiscard]] friend bool operator<(const SchemaId& lhs, const SchemaId& rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }

 private:
  SchemaId() = default;
  explicit SchemaId(std::string value) : value_(std::move(value)) {}
  std::string value_;
};

}  // namespace fsl

namespace std {

template <class Tag>
struct hash<fsl::BasicId128<Tag>> {
  [[nodiscard]] std::size_t operator()(const fsl::BasicId128<Tag>& id) const noexcept {
    // FNV-1a over the raw identity bytes: cheap, allocation-free, and stable for
    // a given process. Hash values are never persisted or compared across runs.
    std::uint64_t mixed = 1469598103934665603ULL;
    for (std::size_t i = 0; i < fsl::BasicId128<Tag>::kByteSize; ++i) {
      mixed ^= static_cast<std::uint64_t>(id.data()[i]);
      mixed *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(mixed ^ (mixed >> 32));
  }
};

template <>
struct hash<fsl::SourceComponentId> {
  [[nodiscard]] std::size_t operator()(const fsl::SourceComponentId& id) const noexcept {
    return hash<string_view>{}(id.view());
  }
};

template <>
struct hash<fsl::SchemaId> {
  [[nodiscard]] std::size_t operator()(const fsl::SchemaId& id) const noexcept {
    return hash<string_view>{}(id.view());
  }
};

}  // namespace std

// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fsl/hash.hpp"
#include "fsl/ids.hpp"

/// \file bytes.hpp
/// Canonical little-endian encoding primitives.
///
/// Every persisted or hashed byte sequence in this library is produced and
/// consumed through these two types, so byte order, width, and length framing
/// are defined in exactly one place. The reader never trusts a length: it
/// checks remaining input and a caller-supplied ceiling before it looks at a
/// single payload byte.

namespace fsl {

/// Append-only little-endian writer over a caller-owned buffer.
class ByteWriter {
 public:
  explicit ByteWriter(std::vector<std::uint8_t>& out) noexcept : out_(&out) {}

  void u8(std::uint8_t value) { out_->push_back(value); }
  void boolean(bool value) { u8(value ? 1u : 0u); }

  void u16(std::uint16_t value) {
    for (int shift = 0; shift < 16; shift += 8) {
      out_->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }
  void u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      out_->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }
  void u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      out_->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }

  void bytes(std::span<const std::uint8_t> value) { out_->insert(out_->end(), value.begin(), value.end()); }
  void bytes(std::string_view value) {
    for (char c : value) {
      out_->push_back(static_cast<std::uint8_t>(c));
    }
  }

  /// kMaxSizedField-byte length prefix followed by the bytes.
  void sized_bytes(std::span<const std::uint8_t> value) {
    u32(static_cast<std::uint32_t>(value.size()));
    bytes(value);
  }
  void sized_string(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    bytes(value);
  }

  void optional_u64(const std::optional<std::uint64_t>& value) {
    boolean(value.has_value());
    if (value.has_value()) {
      u64(*value);
    }
  }

  void digest(const Digest& value) { bytes(value.bytes()); }

  template <class Tag>
  void id128(const BasicId128<Tag>& value) {
    bytes(value.bytes());
  }

  [[nodiscard]] std::size_t position() const noexcept { return out_->size(); }

 private:
  std::vector<std::uint8_t>* out_;
};

/// Bounds-checked little-endian reader over a caller-owned buffer.
///
/// Every accessor reports failure by returning false. On failure the reader is
/// left untouched and the cursor does not advance, so a caller can decide
/// between rejecting the input and recovering at a frame boundary.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  /// Largest single length-prefixed field this reader will materialise.
  static constexpr std::uint32_t kMaxSizedField = 64u * 1024u * 1024u;

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }

  [[nodiscard]] bool u8(std::uint8_t& out) noexcept {
    if (remaining() < 1) {
      return false;
    }
    out = data_[offset_];
    offset_ += 1;
    return true;
  }

  [[nodiscard]] bool boolean(bool& out) noexcept {
    std::uint8_t raw = 0;
    if (!u8(raw)) {
      return false;
    }
    if (raw > 1) {
      return false;
    }
    out = raw == 1;
    return true;
  }

  [[nodiscard]] bool u16(std::uint16_t& out) noexcept { return read_le(out, 2); }
  [[nodiscard]] bool u32(std::uint32_t& out) noexcept { return read_le(out, 4); }
  [[nodiscard]] bool u64(std::uint64_t& out) noexcept { return read_le(out, 8); }

  [[nodiscard]] bool take(std::size_t count, std::span<const std::uint8_t>& out) noexcept {
    if (count > remaining()) {
      return false;
    }
    out = data_.subspan(offset_, count);
    offset_ += count;
    return true;
  }

  /// Reads a u32 length prefix and then that many bytes. `maximum` bounds the
  /// accepted length independently of the remaining input.
  [[nodiscard]] bool sized_bytes(std::span<const std::uint8_t>& out, std::uint32_t maximum) noexcept {
    std::uint32_t length = 0;
    if (!u32(length)) {
      return false;
    }
    if (length > maximum) {
      return false;
    }
    return take(length, out);
  }

  [[nodiscard]] bool sized_string(std::string& out, std::uint32_t maximum) {
    std::span<const std::uint8_t> raw;
    if (!sized_bytes(raw, maximum)) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    return true;
  }

  [[nodiscard]] bool optional_u64(std::optional<std::uint64_t>& out) noexcept {
    bool present = false;
    if (!boolean(present)) {
      return false;
    }
    if (!present) {
      out.reset();
      return true;
    }
    std::uint64_t value = 0;
    if (!u64(value)) {
      return false;
    }
    out = value;
    return true;
  }

  [[nodiscard]] bool digest(Digest& out) noexcept {
    std::span<const std::uint8_t> raw;
    if (!take(Digest::kSize, raw)) {
      return false;
    }
    out = Digest::from_bytes(raw.data());
    return true;
  }

  template <class Tag>
  [[nodiscard]] bool id128(BasicId128<Tag>& out) noexcept {
    std::span<const std::uint8_t> raw;
    if (!take(BasicId128<Tag>::kByteSize, raw)) {
      return false;
    }
    out = BasicId128<Tag>::from_span(raw);
    return true;
  }

 private:
  template <class T>
  [[nodiscard]] bool read_le(T& out, std::size_t width) noexcept {
    if (remaining() < width) {
      return false;
    }
    T value = 0;
    for (std::size_t i = 0; i < width; ++i) {
      value |= static_cast<T>(static_cast<T>(data_[offset_ + i]) << (8u * i));
    }
    offset_ += width;
    out = value;
    return true;
  }

  std::span<const std::uint8_t> data_;
  std::size_t offset_ = 0;
};

}  // namespace fsl

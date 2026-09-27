// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// \file hash.hpp
/// Integrity primitives for the Facility State Ledger.
///
/// Two independent primitives are provided:
///
///  * Sha256 - FIPS 180-4 SHA-256, one-shot and streaming, allocation free.
///    SHA-256 is the record-chain primitive: every record digest commits to its
///    predecessor's digest, so the chain is the authoritative integrity witness
///    for a ledger segment.
///  * crc32c - CRC-32C (Castagnoli), a cheap non-cryptographic checksum used to
///    detect accidental corruption in headers and index framing. CRC-32C is
///    never an authenticity mechanism; it detects accidents, not adversaries.
///
/// All entry points are deterministic, allocate nothing, and are safe to call
/// concurrently on distinct objects. There is no shared mutable state other than
/// the CRC lookup table, which is initialized once in a thread-safe manner.

namespace fsl {

/// A 32-byte SHA-256 digest. Value type; equality is byte-wise.
class Digest {
 public:
  static constexpr std::size_t kSize = 32;

  /// The all-zero digest. Documented as the chain seed for the first record of
  /// a ledger segment; it is never the digest of any real byte string that this
  /// library hashes in practice, and is not a "missing value" sentinel.
  constexpr Digest() noexcept = default;

  /// Copies exactly kSize bytes out of `data`. A null pointer yields the
  /// all-zero digest rather than reading through it.
  static Digest from_bytes(const std::uint8_t* data) noexcept;

  /// Parses exactly 64 hexadecimal characters, either case. Returns nullopt for
  /// any other length or for a non-hexadecimal character.
  static std::optional<Digest> from_hex(std::string_view hex) noexcept;

  /// 64 lowercase hexadecimal characters.
  [[nodiscard]] std::string to_hex() const;

  /// The raw big-endian digest bytes.
  [[nodiscard]] const std::array<std::uint8_t, kSize>& bytes() const noexcept;

  /// Pointer to the first of kSize raw digest bytes.
  [[nodiscard]] const std::uint8_t* data() const noexcept;

  /// True when every byte is zero, i.e. this is the documented chain seed.
  [[nodiscard]] bool is_zero() const noexcept;

  friend bool operator==(const Digest& lhs, const Digest& rhs) noexcept;
  friend bool operator!=(const Digest& lhs, const Digest& rhs) noexcept;
  /// Lexicographic byte order. Used to order digests deterministically.
  friend bool operator<(const Digest& lhs, const Digest& rhs) noexcept;

 private:
  std::array<std::uint8_t, kSize> bytes_{};
};

/// Streaming SHA-256 (FIPS 180-4). Deterministic, no allocation.
///
/// Usage: construct, call update() any number of times with arbitrary chunk
/// boundaries, then call finish() once to obtain the digest. The result is
/// independent of how the input was split across update() calls.
///
/// finish() is idempotent: it returns the same digest on every call and leaves
/// the object usable. update() after finish() is a defined no-op, so a finished
/// hasher cannot silently absorb more input.
class Sha256 {
 public:
  Sha256() noexcept;

  /// Absorbs `size` bytes. A null pointer or a zero size is a no-op.
  void update(const std::uint8_t* data, std::size_t size) noexcept;

  /// Absorbs the bytes of `text`. Absorbs nothing when `text` is empty.
  void update(std::string_view text) noexcept;

  /// Finalizes and returns the digest. Idempotent: returns the same digest if
  /// called twice.
  [[nodiscard]] Digest finish() noexcept;

  /// One-shot digest of `size` bytes.
  [[nodiscard]] static Digest hash(const std::uint8_t* data, std::size_t size) noexcept;

  /// One-shot digest of `text`.
  [[nodiscard]] static Digest hash(std::string_view text) noexcept;

 private:
  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_;
  std::uint64_t total_bytes_ = 0;
  std::size_t buffer_used_ = 0;
  bool finished_ = false;
  Digest result_;  // only meaningful once finished_
};

/// CRC-32C (Castagnoli, polynomial 0x1EDC6F41 reflected, init 0xFFFFFFFF, xorout 0xFFFFFFFF).
///
/// A null pointer or a zero size yields the checksum of the empty input, 0.
[[nodiscard]] std::uint32_t crc32c(const std::uint8_t* data, std::size_t size) noexcept;

/// Continues a CRC-32C computation: `seed` is a previously returned crc32c value.
///
/// The seed is a finished checksum, not a raw internal register, so
/// `crc32c_extend(crc32c(a), b) == crc32c(a||b)` for any split of a byte string.
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t seed, const std::uint8_t* data,
                                          std::size_t size) noexcept;

}  // namespace fsl

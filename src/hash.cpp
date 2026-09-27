// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "fsl/hash.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// \file hash.cpp
/// SHA-256 (FIPS 180-4) and CRC-32C (Castagnoli) implementations.
///
/// Both primitives are written to be portable and free of undefined behavior:
/// every multibyte value is assembled or split with byte-wise shifts, so no
/// load or store ever depends on alignment, endianness, or type punning.

namespace fsl {
namespace {

// ---------------------------------------------------------------------------
// Shared byte helpers
// ---------------------------------------------------------------------------

constexpr std::size_t kBlockSize = 64;
constexpr std::size_t kLengthFieldOffset = kBlockSize - sizeof(std::uint64_t);  // 56
constexpr unsigned int kWordBits = 32;

/// Rotate right. Every call site passes a shift in [1, 31], never 0 or 32.
[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned int shift) noexcept {
  return (value >> shift) | (value << (kWordBits - shift));
}

/// Big-endian load of four bytes; no alignment requirement.
[[nodiscard]] constexpr std::uint32_t load_be32(const std::uint8_t* bytes, std::size_t offset) noexcept {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

/// Big-endian store of four bytes; no alignment requirement.
constexpr void store_be32(std::uint8_t* bytes, std::size_t offset, std::uint32_t value) noexcept {
  bytes[offset] = static_cast<std::uint8_t>(value >> 24);
  bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
  bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
  bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4, section 4.2.2 and section 6.2)
// ---------------------------------------------------------------------------

/// First 32 bits of the fractional parts of the cube roots of the first 64 primes.
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

/// First 32 bits of the fractional parts of the square roots of the first 8 primes.
constexpr std::uint32_t kInitialState[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

[[nodiscard]] constexpr std::uint32_t ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
  return (x & y) ^ (~x & z);
}

[[nodiscard]] constexpr std::uint32_t maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
  return (x & y) ^ (x & z) ^ (y & z);
}

[[nodiscard]] constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept {
  return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

[[nodiscard]] constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept {
  return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

[[nodiscard]] constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept {
  return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

[[nodiscard]] constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept {
  return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

/// Compresses one 64-byte block into the eight-word chaining state.
/// All arithmetic is on std::uint32_t, whose overflow is defined (modulo 2^32).
void compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t i = 0; i < 16; ++i) {
    schedule[i] = load_be32(block, i * 4);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    schedule[i] = small_sigma1(schedule[i - 2]) + schedule[i - 7] + small_sigma0(schedule[i - 15]) +
                  schedule[i - 16];
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t t1 = h + big_sigma1(e) + ch(e, f, g) + kRoundConstants[i] + schedule[i];
    const std::uint32_t t2 = big_sigma0(a) + maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

// ---------------------------------------------------------------------------
// CRC-32C (Castagnoli)
// ---------------------------------------------------------------------------

/// 0x1EDC6F41 with its bits reversed, the form used by a reflected LSB-first loop.
constexpr std::uint32_t kCrc32cReflectedPolynomial = 0x82F63B78u;

constexpr std::uint32_t kCrc32cInit = 0xFFFFFFFFu;
constexpr std::uint32_t kCrc32cXorOut = 0xFFFFFFFFu;

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256u; ++index) {
    std::uint32_t remainder = index;
    for (unsigned int bit = 0; bit < 8u; ++bit) {
      remainder = ((remainder & 1u) != 0u) ? ((remainder >> 1) ^ kCrc32cReflectedPolynomial)
                                           : (remainder >> 1);
    }
    table[index] = remainder;
  }
  return table;
}

/// Lazily initialized, thread-safe (C++11 magic static) lookup table. This is the
/// only object with static storage duration that the primitives rely on, and it
/// is immutable after construction.
[[nodiscard]] const std::array<std::uint32_t, 256>& crc32c_table() noexcept {
  static const std::array<std::uint32_t, 256> table = make_crc32c_table();
  return table;
}

// ---------------------------------------------------------------------------
// Hexadecimal helpers
// ---------------------------------------------------------------------------

/// Value of a hexadecimal digit, or -1 when `character` is not one.
[[nodiscard]] constexpr int hex_digit_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// Digest
// ---------------------------------------------------------------------------

Digest Digest::from_bytes(const std::uint8_t* data) noexcept {
  Digest digest;
  if (data != nullptr) {
    for (std::size_t i = 0; i < kSize; ++i) {
      digest.bytes_[i] = data[i];
    }
  }
  return digest;
}

std::optional<Digest> Digest::from_hex(std::string_view hex) noexcept {
  if (hex.size() != kSize * 2) {
    return std::nullopt;
  }
  Digest digest;
  for (std::size_t i = 0; i < kSize; ++i) {
    const int high = hex_digit_value(hex[2 * i]);
    const int low = hex_digit_value(hex[2 * i + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    digest.bytes_[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return digest;
}

std::string Digest::to_hex() const {
  constexpr std::string_view kHexDigits = "0123456789abcdef";
  std::string text(kSize * 2, '0');
  for (std::size_t i = 0; i < kSize; ++i) {
    const std::uint8_t byte = bytes_[i];
    text[2 * i] = kHexDigits[static_cast<std::size_t>(byte >> 4)];
    text[2 * i + 1] = kHexDigits[static_cast<std::size_t>(byte & 0x0Fu)];
  }
  return text;
}

const std::array<std::uint8_t, Digest::kSize>& Digest::bytes() const noexcept { return bytes_; }

const std::uint8_t* Digest::data() const noexcept { return bytes_.data(); }

bool Digest::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

bool operator==(const Digest& lhs, const Digest& rhs) noexcept {
  for (std::size_t i = 0; i < Digest::kSize; ++i) {
    if (lhs.bytes_[i] != rhs.bytes_[i]) {
      return false;
    }
  }
  return true;
}

bool operator!=(const Digest& lhs, const Digest& rhs) noexcept { return !(lhs == rhs); }

bool operator<(const Digest& lhs, const Digest& rhs) noexcept {
  for (std::size_t i = 0; i < Digest::kSize; ++i) {
    if (lhs.bytes_[i] != rhs.bytes_[i]) {
      return lhs.bytes_[i] < rhs.bytes_[i];
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Sha256
// ---------------------------------------------------------------------------

Sha256::Sha256() noexcept : state_{} {
  for (std::size_t i = 0; i < state_.size(); ++i) {
    state_[i] = kInitialState[i];
  }
  buffer_.fill(0);
}

void Sha256::update(const std::uint8_t* data, std::size_t size) noexcept {
  // A finalized hasher absorbs nothing: the digest is already committed.
  // A null pointer or an empty range is a defined no-op.
  if (finished_ || data == nullptr || size == 0) {
    return;
  }

  total_bytes_ += static_cast<std::uint64_t>(size);

  std::size_t offset = 0;

  // Top up a partially filled block first so that the main loop can consume
  // whole blocks straight out of the caller's buffer without copying.
  if (buffer_used_ != 0) {
    while (offset < size && buffer_used_ < buffer_.size()) {
      buffer_[buffer_used_] = data[offset];
      ++buffer_used_;
      ++offset;
    }
    if (buffer_used_ == buffer_.size()) {
      compress(state_, buffer_.data());
      buffer_used_ = 0;
    }
  }

  while (size - offset >= buffer_.size()) {
    compress(state_, data + offset);
    offset += buffer_.size();
  }

  while (offset < size) {
    buffer_[buffer_used_] = data[offset];
    ++buffer_used_;
    ++offset;
  }
}

void Sha256::update(std::string_view text) noexcept {
  if (text.empty()) {
    return;
  }
  update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

Digest Sha256::finish() noexcept {
  if (finished_) {
    return result_;
  }

  // FIPS 180-4 padding: 0x80, then zeros until 8 bytes remain in the block,
  // then the message length in bits as a 64-bit big-endian integer. The bit
  // length is 64-bit, so inputs at or beyond 2^32 bytes are encoded correctly.
  const std::uint64_t bit_length = total_bytes_ * 8u;

  buffer_[buffer_used_] = 0x80u;
  ++buffer_used_;

  if (buffer_used_ > kLengthFieldOffset) {
    while (buffer_used_ < kBlockSize) {
      buffer_[buffer_used_] = 0;
      ++buffer_used_;
    }
    compress(state_, buffer_.data());
    buffer_used_ = 0;
  }

  while (buffer_used_ < kLengthFieldOffset) {
    buffer_[buffer_used_] = 0;
    ++buffer_used_;
  }

  store_be32(buffer_.data(), kLengthFieldOffset, static_cast<std::uint32_t>(bit_length >> 32));
  store_be32(buffer_.data(), kLengthFieldOffset + 4, static_cast<std::uint32_t>(bit_length));

  compress(state_, buffer_.data());

  std::array<std::uint8_t, Digest::kSize> packed{};
  for (std::size_t i = 0; i < state_.size(); ++i) {
    store_be32(packed.data(), i * 4, state_[i]);
  }

  result_ = Digest::from_bytes(packed.data());
  finished_ = true;
  return result_;
}

Digest Sha256::hash(const std::uint8_t* data, std::size_t size) noexcept {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

Digest Sha256::hash(std::string_view text) noexcept {
  Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

// ---------------------------------------------------------------------------
// CRC-32C
// ---------------------------------------------------------------------------

std::uint32_t crc32c_extend(std::uint32_t seed, const std::uint8_t* data, std::size_t size) noexcept {
  const std::array<std::uint32_t, 256>& table = crc32c_table();

  // `seed` is a previously *returned* checksum, i.e. the internal register
  // already xor'ed with the final mask. Undo that mask to resume the register,
  // and reapply it on the way out; the caller's seed is never treated as an
  // unwrapped register and never has the initial value forced into it. This is
  // what makes crc32c_extend(crc32c(a), b) == crc32c(a||b).
  std::uint32_t remainder = seed ^ kCrc32cXorOut;

  if (data != nullptr) {
    for (std::size_t i = 0; i < size; ++i) {
      remainder = table[(remainder ^ data[i]) & 0xFFu] ^ (remainder >> 8);
    }
  }

  return remainder ^ kCrc32cXorOut;
}

std::uint32_t crc32c(const std::uint8_t* data, std::size_t size) noexcept {
  // In the returned-checksum domain a fresh computation starts from the checksum
  // of the empty input, which is zero: crc32c_extend(0, ...) unwraps to the
  // standard init register 0xFFFFFFFF and re-applies the final xor. An empty
  // input therefore checksums to zero, and crc32c(empty) is a usable seed.
  static_assert((kCrc32cInit ^ kCrc32cXorOut) == 0u,
                "a fresh CRC-32C computation is a zero seed in the returned-checksum domain");
  return crc32c_extend(kCrc32cInit ^ kCrc32cXorOut, data, size);
}

}  // namespace fsl

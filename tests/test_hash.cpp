// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

// Standalone conformance tests for fsl/hash.hpp.
//
// No test framework, no third-party code, no timeouts: every check records a
// failure and execution continues, so one run reports every problem it finds.
// Exit code is 0 when every check passed and 1 otherwise.
//
//   stdout: "test_hash: PASS (<n> checks)" or "test_hash: FAIL (<n> failures)"
//   stderr: per-failure detail plus a one-line failure summary
//
// Known-answer vectors are limited to values published in FIPS 180-4 / NIST
// example sets and the standard CRC-32C (Castagnoli) check values. Behavior at
// the padding boundaries and for multi-megabyte inputs that has no published
// vector is verified by cross-checking independent code paths: one-shot versus
// streaming, and different chunk splittings of the same bytes.

#include "fsl/hash.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void record(bool passed, const char* what, int line) noexcept {
  ++g_checks;
  if (!passed) {
    ++g_failures;
    std::fprintf(stderr, "test_hash: FAIL (line %d): %s\n", line, what);
  }
}

void record_digest(const fsl::Digest& actual, std::string_view expected_hex, const char* what, int line) {
  ++g_checks;
  const std::string actual_hex = actual.to_hex();
  if (actual_hex != std::string(expected_hex)) {
    ++g_failures;
    std::fprintf(stderr, "test_hash: FAIL (line %d): %s\n    expected %s\n    actual   %s\n", line, what,
                 std::string(expected_hex).c_str(), actual_hex.c_str());
  }
}

void record_equal_digest(const fsl::Digest& actual, const fsl::Digest& expected, const char* what, int line) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::fprintf(stderr, "test_hash: FAIL (line %d): %s\n    expected %s\n    actual   %s\n", line, what,
                 expected.to_hex().c_str(), actual.to_hex().c_str());
  }
}

void record_u32(std::uint32_t actual, std::uint32_t expected, const char* what, int line) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::fprintf(stderr, "test_hash: FAIL (line %d): %s\n    expected 0x%08X\n    actual   0x%08X\n", line, what,
                 static_cast<unsigned int>(expected), static_cast<unsigned int>(actual));
  }
}

#define CHECK(condition) ::record((condition), #condition, __LINE__)
#define CHECK_DIGEST(actual, expected_hex) \
  ::record_digest((actual), (expected_hex), #actual " equals " expected_hex, __LINE__)
#define CHECK_SAME_DIGEST(actual, expected) ::record_equal_digest((actual), (expected), #actual " == " #expected, __LINE__)
#define CHECK_U32(actual, expected) ::record_u32((actual), (expected), #actual, __LINE__)

// ---------------------------------------------------------------------------
// Deterministic generators
// ---------------------------------------------------------------------------

/// Fully specified engine: identical byte streams on every standard library.
std::uint64_t next_u64(std::mt19937_64& rng) noexcept { return rng(); }

std::uint8_t next_byte(std::mt19937_64& rng) noexcept {
  return static_cast<std::uint8_t>(next_u64(rng) & 0xFFu);
}

std::vector<std::uint8_t> make_bytes(std::size_t size, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = next_byte(rng);
  }
  return bytes;
}

std::vector<std::uint8_t> make_pattern(std::size_t size) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<std::uint8_t>(i % 251u);
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// Hashing helpers
// ---------------------------------------------------------------------------

fsl::Digest hash_in_chunks(const std::uint8_t* data, std::size_t size, std::size_t chunk) {
  fsl::Sha256 hasher;
  std::size_t offset = 0;
  while (offset < size) {
    const std::size_t count = std::min(chunk, size - offset);
    hasher.update(data + offset, count);
    offset += count;
  }
  return hasher.finish();
}

fsl::Digest hash_in_varied_chunks(const std::uint8_t* data, std::size_t size, std::mt19937_64& rng) {
  fsl::Sha256 hasher;
  std::size_t offset = 0;
  while (offset < size) {
    const std::size_t chunk = static_cast<std::size_t>(next_u64(rng) % 130u) + 1u;
    const std::size_t count = std::min(chunk, size - offset);
    hasher.update(data + offset, count);
    offset += count;
  }
  return hasher.finish();
}

/// A chunk split that walks every block-boundary alignment: 1, 2, 3, ... 70.
fsl::Digest hash_in_walking_chunks(const std::uint8_t* data, std::size_t size) {
  fsl::Sha256 hasher;
  std::size_t offset = 0;
  std::size_t chunk = 1;
  while (offset < size) {
    const std::size_t count = std::min(chunk, size - offset);
    hasher.update(data + offset, count);
    offset += count;
    ++chunk;
    if (chunk > 70) {
      chunk = 1;
    }
  }
  return hasher.finish();
}

fsl::Digest sha256_of(std::string_view text) { return fsl::Sha256::hash(text); }

/// Explicit byte copy: the library hashes bytes, text is only a convenience.
std::vector<std::uint8_t> to_bytes(std::string_view text) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(text.size());
  for (const char character : text) {
    bytes.push_back(static_cast<std::uint8_t>(character));
  }
  return bytes;
}

fsl::Digest sha256_of_bytes(const std::vector<std::uint8_t>& bytes) {
  return fsl::Sha256::hash(bytes.data(), bytes.size());
}

std::uint32_t crc32c_of(const std::vector<std::uint8_t>& bytes) {
  return fsl::crc32c(bytes.data(), bytes.size());
}

// ---------------------------------------------------------------------------
// SHA-256 known-answer vectors (FIPS 180-4 / NIST examples)
// ---------------------------------------------------------------------------

void test_sha256_known_answers() {
  const std::string_view empty = "";
  const std::string_view abc = "abc";
  const std::string_view one_block =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 56 bytes, 448 bits
  const std::string_view two_block =
      "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
      "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";  // 112 bytes, 896 bits

  // The literals above are only meaningful if they have the documented lengths.
  CHECK(empty.size() == 0);
  CHECK(one_block.size() == 56);
  CHECK(two_block.size() == 112);
  CHECK(std::string_view("The quick brown fox jumps over the lazy dog").size() == 43);

  CHECK_DIGEST(sha256_of(empty), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK_DIGEST(sha256_of("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK_DIGEST(sha256_of(one_block), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK_DIGEST(sha256_of(two_block), "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
  CHECK_DIGEST(sha256_of("The quick brown fox jumps over the lazy dog"),
               "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
  CHECK_DIGEST(sha256_of("The quick brown fox jumps over the lazy dog."),
               "ef537f25c895bfa782526529a9b63d97aa631564d5d789c2b765448c8635fb6c");

  // FIPS 180-4: one million repetitions of 'a'.
  const std::string million_a(1000000, 'a');
  CHECK_DIGEST(sha256_of(million_a), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  CHECK_DIGEST(hash_in_chunks(reinterpret_cast<const std::uint8_t*>(million_a.data()), million_a.size(), 9973),
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  CHECK_DIGEST(hash_in_chunks(reinterpret_cast<const std::uint8_t*>(million_a.data()), million_a.size(), 1),
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  // The pointer/size and string_view entry points must agree.
  const std::vector<std::uint8_t> abc_bytes = to_bytes(abc);
  CHECK_SAME_DIGEST(fsl::Sha256::hash(abc_bytes.data(), abc_bytes.size()), fsl::Sha256::hash(abc));
  CHECK_SAME_DIGEST(sha256_of_bytes(abc_bytes), fsl::Sha256::hash(abc));
  CHECK_SAME_DIGEST(fsl::Sha256::hash(std::string(abc)), fsl::Sha256::hash(abc));

  // An empty input must not collide with the documented all-zero chain seed.
  CHECK(fsl::Sha256::hash(empty) != fsl::Digest{});
  CHECK(!fsl::Sha256::hash(empty).is_zero());
}

// ---------------------------------------------------------------------------
// Padding boundaries and streaming equivalence
// ---------------------------------------------------------------------------

void test_sha256_padding_boundaries() {
  // 55 -> 0x80 and length fit in one block; 56/57/63/64/65 -> an extra block is
  // required; 119/120/127/128/129 -> the same boundary one block later.
  const std::size_t lengths[] = {0,  1,  2,  3,  54,  55,  56,  57,  58,  62,   63,   64,   65,
                                 66, 67, 118, 119, 120, 121, 127, 128, 129, 191, 192, 1000, 1024};

  for (const std::size_t length : lengths) {
    const std::vector<std::uint8_t> bytes = make_pattern(length + 8);
    const std::uint8_t* data = bytes.data();

    const fsl::Digest one_shot = fsl::Sha256::hash(data, length);
    CHECK_SAME_DIGEST(hash_in_chunks(data, length, 1), one_shot);
    CHECK_SAME_DIGEST(hash_in_chunks(data, length, 63), one_shot);
    CHECK_SAME_DIGEST(hash_in_chunks(data, length, 64), one_shot);
    CHECK_SAME_DIGEST(hash_in_chunks(data, length, 65), one_shot);
    CHECK_SAME_DIGEST(hash_in_chunks(data, length, 1024), one_shot);
    CHECK_SAME_DIGEST(hash_in_walking_chunks(data, length), one_shot);

    std::mt19937_64 rng(20260101u + static_cast<std::uint64_t>(length));
    CHECK_SAME_DIGEST(hash_in_varied_chunks(data, length, rng), one_shot);
  }

  // The 56-byte NIST message hashed out of a buffer that is exactly 56 bytes
  // long: the padding has to spill into a second block.
  const std::string_view one_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  const std::vector<std::uint8_t> exact = to_bytes(one_block);
  CHECK(exact.size() == 56);
  CHECK_DIGEST(fsl::Sha256::hash(exact.data(), exact.size()),
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK_DIGEST(hash_in_chunks(exact.data(), exact.size(), 1),
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

void test_sha256_large_streaming() {
  // Several megabytes: thousands of 64-byte blocks plus an odd tail, so the
  // 64-bit length field and the block loop are exercised well past 2^32 bits
  // of accumulated state.
  const std::size_t large_size = (3u * 1024u * 1024u) + 17u;
  const std::vector<std::uint8_t> large = make_bytes(large_size, 20260101u);

  const fsl::Digest one_shot = fsl::Sha256::hash(large.data(), large.size());
  CHECK_SAME_DIGEST(sha256_of_bytes(large), one_shot);
  CHECK_SAME_DIGEST(hash_in_chunks(large.data(), large.size(), 1), one_shot);
  CHECK_SAME_DIGEST(hash_in_chunks(large.data(), large.size(), 4093), one_shot);
  CHECK_SAME_DIGEST(hash_in_chunks(large.data(), large.size(), 4096), one_shot);
  CHECK_SAME_DIGEST(hash_in_chunks(large.data(), large.size(), 65536), one_shot);
  CHECK_SAME_DIGEST(hash_in_walking_chunks(large.data(), large.size()), one_shot);

  std::mt19937_64 rng(424242u);
  CHECK_SAME_DIGEST(hash_in_varied_chunks(large.data(), large.size(), rng), one_shot);

  // Bigger than one megabyte is genuinely different data than the small cases.
  const std::vector<std::uint8_t> other = make_bytes(large_size, 20260102u);
  CHECK(fsl::Sha256::hash(other.data(), other.size()) != one_shot);

  // Streaming the same bytes through the string_view overload agrees.
  const std::string as_text(reinterpret_cast<const char*>(large.data()), large.size());
  CHECK_SAME_DIGEST(fsl::Sha256::hash(as_text), one_shot);
}

// ---------------------------------------------------------------------------
// State machine: finish idempotency, no-op updates
// ---------------------------------------------------------------------------

void test_sha256_state_semantics() {
  const fsl::Digest abc = sha256_of("abc");

  // finish() twice returns equal digests and does not disturb the object.
  fsl::Sha256 hasher;
  hasher.update("abc");
  const fsl::Digest first = hasher.finish();
  const fsl::Digest second = hasher.finish();
  const fsl::Digest third = hasher.finish();
  CHECK_SAME_DIGEST(first, abc);
  CHECK_SAME_DIGEST(second, abc);
  CHECK_SAME_DIGEST(third, abc);

  // update() after finish() is a defined no-op, including through the
  // string_view overload, and cannot change the committed digest.
  hasher.update("def");
  hasher.update(std::string_view("def"));
  hasher.update(nullptr, 0);
  CHECK_SAME_DIGEST(hasher.finish(), abc);

  std::uint8_t extra[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  hasher.update(extra, sizeof(extra));
  CHECK_SAME_DIGEST(hasher.finish(), abc);
  CHECK_SAME_DIGEST(hasher.finish(), first);

  // update(nullptr, 0) is a safe no-op on a fresh and on a partial hasher.
  fsl::Sha256 fresh;
  fresh.update(nullptr, 0);
  CHECK_SAME_DIGEST(fresh.finish(), fsl::Sha256::hash(""));

  fsl::Sha256 partial;
  partial.update("ab");
  partial.update(nullptr, 0);
  partial.update(nullptr, 100);
  partial.update("c");
  partial.update(nullptr, 0);
  CHECK_SAME_DIGEST(partial.finish(), abc);

  // Zero-length updates of any shape leave the digest untouched.
  std::mt19937_64 rng(777u);
  const std::vector<std::uint8_t> bytes = make_bytes(300, 999u);
  fsl::Sha256 piecewise;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    piecewise.update(nullptr, static_cast<std::size_t>(next_u64(rng) % 7u));  // no-op of varying "size"
    const std::size_t count = std::min<std::size_t>(next_u64(rng) % 40u + 1u, bytes.size() - offset);
    piecewise.update(bytes.data() + offset, count);
    offset += count;
  }
  CHECK_SAME_DIGEST(piecewise.finish(), fsl::Sha256::hash(bytes.data(), bytes.size()));

  // A finalized hasher kept alive alongside other work stays stable.
  fsl::Sha256 keeper;
  keeper.update("The quick brown fox jumps over the lazy dog");
  const fsl::Digest keeper_digest = keeper.finish();
  fsl::Sha256 worker;
  worker.update(bytes.data(), bytes.size());
  (void)worker.finish();
  CHECK_SAME_DIGEST(keeper.finish(), keeper_digest);
}

// ---------------------------------------------------------------------------
// Digest value semantics
// ---------------------------------------------------------------------------

void test_digest_value_semantics() {
  const fsl::Digest zero;
  CHECK(zero.is_zero());
  CHECK(zero.to_hex() == std::string(64, '0'));
  CHECK(zero.to_hex().size() == fsl::Digest::kSize * 2);
  CHECK(fsl::Digest::from_bytes(nullptr) == zero);

  std::mt19937_64 rng(20260101u);

  for (int iteration = 0; iteration < 256; ++iteration) {
    std::array<std::uint8_t, fsl::Digest::kSize> raw{};
    for (std::size_t i = 0; i < raw.size(); ++i) {
      raw[i] = next_byte(rng);
    }

    const fsl::Digest digest = fsl::Digest::from_bytes(raw.data());
    CHECK(!digest.is_zero());
    CHECK(std::equal(raw.begin(), raw.end(), digest.bytes().begin()));
    CHECK(digest.data() == digest.bytes().data());

    // Hex round-trip, lowercase output.
    const std::string hex = digest.to_hex();
    CHECK(hex.size() == 64);
    CHECK(hex.find_first_not_of("0123456789abcdef") == std::string::npos);
    const std::optional<fsl::Digest> parsed = fsl::Digest::from_hex(hex);
    CHECK(parsed.has_value());
    if (parsed.has_value()) {
      CHECK(parsed.value() == digest);
      CHECK(!(parsed.value() != digest));
      CHECK(parsed.value().to_hex() == hex);
    }

    // Uppercase is accepted and denotes the same digest.
    std::string upper = hex;
    for (char& character : upper) {
      if (character >= 'a' && character <= 'f') {
        character = static_cast<char>(character - 'a' + 'A');
      }
    }
    const std::optional<fsl::Digest> parsed_upper = fsl::Digest::from_hex(upper);
    CHECK(parsed_upper.has_value());
    if (parsed_upper.has_value()) {
      CHECK(parsed_upper.value() == digest);
      CHECK(parsed_upper.value().to_hex() == hex);  // output stays lowercase
    }

    // Mixed case is accepted too.
    std::string mixed = hex;
    for (std::size_t i = 0; i < mixed.size(); i += 2) {
      mixed[i] = upper[i];
    }
    const std::optional<fsl::Digest> parsed_mixed = fsl::Digest::from_hex(mixed);
    CHECK(parsed_mixed.has_value());
    if (parsed_mixed.has_value()) {
      CHECK(parsed_mixed.value() == digest);
    }

    // A digest different in exactly one bit must not compare equal.
    std::array<std::uint8_t, fsl::Digest::kSize> tweaked = raw;
    const std::size_t tap = static_cast<std::size_t>(iteration) % fsl::Digest::kSize;
    tweaked[tap] ^= 0x01u;
    CHECK(fsl::Digest::from_bytes(tweaked.data()) != digest);
  }

  // Rejections: wrong lengths.
  const std::string valid = sha256_of("abc").to_hex();
  CHECK(valid.size() == 64);
  CHECK(!fsl::Digest::from_hex("").has_value());
  CHECK(!fsl::Digest::from_hex(valid.substr(0, 63)).has_value());
  CHECK(!fsl::Digest::from_hex(valid.substr(0, 62)).has_value());
  CHECK(!fsl::Digest::from_hex(valid + "0").has_value());
  CHECK(!fsl::Digest::from_hex(valid + valid).has_value());
  CHECK(!fsl::Digest::from_hex("0x" + valid.substr(0, 62)).has_value());

  // Rejections: non-hex characters, including whitespace and sign characters.
  CHECK(!fsl::Digest::from_hex(std::string(64, 'g')).has_value());
  CHECK(!fsl::Digest::from_hex(std::string(64, 'z')).has_value());
  CHECK(!fsl::Digest::from_hex(std::string(64, ' ')).has_value());
  CHECK(!fsl::Digest::from_hex(std::string(64, '+')).has_value());
  CHECK(!fsl::Digest::from_hex(std::string(64, '/')).has_value());
  {
    std::string one_bad = valid;
    one_bad[0] = 'G';
    CHECK(!fsl::Digest::from_hex(one_bad).has_value());
    one_bad[0] = ' ';
    CHECK(!fsl::Digest::from_hex(one_bad).has_value());
    one_bad[0] = '\n';
    CHECK(!fsl::Digest::from_hex(one_bad).has_value());
    one_bad = valid;
    one_bad[63] = ':';
    CHECK(!fsl::Digest::from_hex(one_bad).has_value());
    one_bad[63] = '`';
    CHECK(!fsl::Digest::from_hex(one_bad).has_value());
  }

  // Lexicographic byte order, cross-checked against a manual comparison.
  for (int iteration = 0; iteration < 256; ++iteration) {
    std::array<std::uint8_t, fsl::Digest::kSize> left_raw{};
    std::array<std::uint8_t, fsl::Digest::kSize> right_raw{};
    for (std::size_t i = 0; i < left_raw.size(); ++i) {
      left_raw[i] = next_byte(rng);
      right_raw[i] = next_byte(rng);
    }
    if (iteration % 3 == 0) {
      right_raw = left_raw;  // exercise the equal case
    }

    const fsl::Digest left = fsl::Digest::from_bytes(left_raw.data());
    const fsl::Digest right = fsl::Digest::from_bytes(right_raw.data());

    bool expected_less = false;
    bool expected_equal = true;
    for (std::size_t i = 0; i < left_raw.size(); ++i) {
      if (left_raw[i] != right_raw[i]) {
        expected_less = left_raw[i] < right_raw[i];
        expected_equal = false;
        break;
      }
    }
    CHECK((left < right) == expected_less);
    CHECK((left == right) == expected_equal);
    CHECK((left != right) == !expected_equal);
  }

  // Ordering is a strict weak ordering on the shared zero digest.
  const fsl::Digest one = fsl::Digest::from_hex(std::string(63, '0') + "1").value();
  CHECK(zero < one);
  CHECK(!(one < zero));
  CHECK(!(zero < zero));
  CHECK(fsl::Digest::from_hex(std::string(64, '0')).value() == zero);
}

// ---------------------------------------------------------------------------
// CRC-32C
// ---------------------------------------------------------------------------

void test_crc32c_known_answers() {
  const std::string_view check_string = "123456789";
  CHECK_U32(fsl::crc32c(reinterpret_cast<const std::uint8_t*>(check_string.data()), check_string.size()),
           0xE3069283u);

  const std::vector<std::uint8_t> zeros(32, 0x00u);
  const std::vector<std::uint8_t> ones(32, 0xFFu);
  std::vector<std::uint8_t> incrementing(32);
  for (std::size_t i = 0; i < incrementing.size(); ++i) {
    incrementing[i] = static_cast<std::uint8_t>(i);
  }

  CHECK_U32(crc32c_of(zeros), 0x8A9136AAu);
  CHECK_U32(crc32c_of(ones), 0x62A8AB43u);
  CHECK_U32(crc32c_of(incrementing), 0x46DD794Eu);

  // Empty input, null or not, is the empty checksum.
  CHECK_U32(fsl::crc32c(nullptr, 0), 0u);
  CHECK_U32(fsl::crc32c(nullptr, 100), 0u);
  CHECK_U32(fsl::crc32c(zeros.data(), 0), 0u);
  CHECK_U32(crc32c_of({}), 0u);

  // A single byte extended with nothing is unchanged.
  const std::uint8_t single = 0x5Au;
  const std::uint32_t single_crc = fsl::crc32c(&single, 1);
  CHECK_U32(fsl::crc32c_extend(single_crc, nullptr, 0), single_crc);
  CHECK_U32(fsl::crc32c_extend(single_crc, &single, 0), single_crc);
  CHECK_U32(fsl::crc32c_extend(0u, nullptr, 0), 0u);

  // Extending with a real zero byte is not the same as not extending at all.
  const std::uint8_t zero_byte = 0x00u;
  const std::uint8_t pair[2] = {single, 0x00u};
  CHECK_U32(fsl::crc32c_extend(single_crc, &zero_byte, 1), fsl::crc32c(pair, 2));

  // The empty checksum is a valid seed: extend(0, x) == crc32c(x).
  CHECK_U32(fsl::crc32c_extend(0u, incrementing.data(), incrementing.size()), 0x46DD794Eu);
}

void test_crc32c_extension_equivalence() {
  std::mt19937_64 rng(20260101u);

  for (std::size_t size : {0u, 1u, 2u, 3u, 31u, 32u, 33u, 63u, 64u, 65u, 1000u, 65536u}) {
    const std::vector<std::uint8_t> bytes = make_bytes(size, 20260101u + size);
    const std::uint32_t whole = crc32c_of(bytes);

    // Every possible split point of a short buffer, and sampled splits of a long one.
    const std::size_t step = size > 64 ? size / 37u + 1u : 1u;
    for (std::size_t split = 0; split <= size; split += step) {
      const std::uint32_t first = fsl::crc32c(bytes.data(), split);
      const std::uint32_t joined =
          fsl::crc32c_extend(first, bytes.data() + split, size - split);
      CHECK_U32(joined, whole);
    }

    // The same equivalence through many small pieces, including empty pieces.
    std::uint32_t running = 0u;
    std::size_t offset = 0;
    while (offset < size) {
      const std::size_t count =
          std::min<std::size_t>(static_cast<std::size_t>(next_u64(rng) % 17u), size - offset);
      running = fsl::crc32c_extend(running, bytes.data() + offset, count);
      offset += count;
    }
    running = fsl::crc32c_extend(running, nullptr, 0);
    running = fsl::crc32c_extend(running, bytes.data() + size, 0);
    CHECK_U32(running, whole);

    // Byte-at-a-time extension.
    std::uint32_t byte_wise = 0u;
    for (std::size_t i = 0; i < size; ++i) {
      byte_wise = fsl::crc32c_extend(byte_wise, bytes.data() + i, 1);
    }
    CHECK_U32(byte_wise, whole);

    // Repeating the same bytes must give the same checksum (no hidden state).
    CHECK_U32(crc32c_of(bytes), whole);
  }

  // Distinct inputs must not collapse to the same checksum in these samples.
  CHECK(fsl::crc32c(reinterpret_cast<const std::uint8_t*>("123456789"), 9) !=
        fsl::crc32c(reinterpret_cast<const std::uint8_t*>("123456780"), 9));
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

int run_all() {
  test_sha256_known_answers();
  test_sha256_padding_boundaries();
  test_sha256_large_streaming();
  test_sha256_state_semantics();
  test_digest_value_semantics();
  test_crc32c_known_answers();
  test_crc32c_extension_equivalence();

  if (g_failures != 0) {
    std::fprintf(stderr, "test_hash: %d of %d checks failed\n", g_failures, g_checks);
    return g_failures;
  }
  return 0;
}

}  // namespace

int main() {
  const int failures = run_all();
  if (failures != 0) {
    std::printf("test_hash: FAIL (%d failures)\n", failures);
    std::fflush(stdout);
    return 1;
  }
  std::printf("test_hash: PASS (%d checks)\n", g_checks);
  std::fflush(stdout);
  return 0;
}

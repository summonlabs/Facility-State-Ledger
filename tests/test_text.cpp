// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include "fsl/text.hpp"
#include "support/test_support.hpp"

/// \file test_text.cpp
/// Validation of untrusted text at every trust boundary.

namespace {

FSL_TEST(hex_round_trip) {
  const std::vector<std::uint8_t> bytes{0x00, 0x01, 0x7F, 0x80, 0xFF, 0xAB};
  const std::string encoded = fsl::to_hex(bytes);
  FSL_CHECK_EQ(encoded, std::string("00017f80ffab"));
  auto decoded = fsl::from_hex(encoded);
  FSL_REQUIRE(decoded.has_value());
  FSL_CHECK(decoded.value() == bytes);
}

FSL_TEST(hex_accepts_upper_case_but_emits_lower) {
  auto decoded = fsl::from_hex("DEADBEEF");
  FSL_REQUIRE(decoded.has_value());
  FSL_CHECK_EQ(decoded->size(), std::size_t{4});
  FSL_CHECK_EQ(fsl::to_hex(decoded.value()), std::string("deadbeef"));
}

FSL_TEST(hex_rejects_malformed_input) {
  FSL_CHECK(!fsl::from_hex("abc").has_value());
  FSL_CHECK(!fsl::from_hex("zz").has_value());
  FSL_CHECK(!fsl::from_hex("0g").has_value());
  FSL_CHECK(!fsl::from_hex("  ").has_value());
}

FSL_TEST(hex_respects_the_length_bound_before_allocating) {
  const std::string large(200, 'a');
  FSL_CHECK(!fsl::from_hex(large, 4).has_value());
  FSL_CHECK(fsl::from_hex(large, 100).has_value());
}

FSL_TEST(strict_utf8_accepts_valid_sequences) {
  FSL_CHECK(fsl::is_valid_utf8(""));
  FSL_CHECK(fsl::is_valid_utf8("plain ascii"));
  FSL_CHECK(fsl::is_valid_utf8("\xC2\xA9"));          // U+00A9
  FSL_CHECK(fsl::is_valid_utf8("\xE2\x82\xAC"));      // U+20AC
  FSL_CHECK(fsl::is_valid_utf8("\xF0\x9F\x98\x80"));  // U+1F600
}

FSL_TEST(strict_utf8_rejects_overlong_surrogate_and_truncated_forms) {
  FSL_CHECK(!fsl::is_valid_utf8("\xC0\xAF"));          // overlong solidus
  FSL_CHECK(!fsl::is_valid_utf8("\xE0\x80\xAF"));      // overlong
  FSL_CHECK(!fsl::is_valid_utf8("\xF0\x80\x80\xAF"));  // overlong
  FSL_CHECK(!fsl::is_valid_utf8("\xED\xA0\x80"));      // UTF-16 surrogate half
  FSL_CHECK(!fsl::is_valid_utf8("\xED\xBF\xBF"));      // surrogate half
  FSL_CHECK(!fsl::is_valid_utf8("\xF4\x90\x80\x80"));  // above U+10FFFF
  FSL_CHECK(!fsl::is_valid_utf8("\xC2"));              // truncated
  FSL_CHECK(!fsl::is_valid_utf8("\xE2\x82"));          // truncated
  FSL_CHECK(!fsl::is_valid_utf8("\xFF"));              // never valid
  FSL_CHECK(!fsl::is_valid_utf8("\x80"));              // stray continuation
}

FSL_TEST(control_characters_are_rejected) {
  FSL_CHECK(fsl::has_no_control_characters("normal text"));
  FSL_CHECK(!fsl::has_no_control_characters(std::string("nul\0here", 8)));
  FSL_CHECK(!fsl::has_no_control_characters("line\nbreak"));
  FSL_CHECK(!fsl::has_no_control_characters("del\x7F"));
}

FSL_TEST(identifier_syntax_is_lower_case_canonical) {
  FSL_CHECK(fsl::is_valid_identifier("a", 8));
  FSL_CHECK(fsl::is_valid_identifier("dccp.facility-topology_1", 64));
  FSL_CHECK(!fsl::is_valid_identifier("", 8));
  FSL_CHECK(!fsl::is_valid_identifier("Uppercase", 64));
  FSL_CHECK(!fsl::is_valid_identifier(".leading", 64));
  FSL_CHECK(!fsl::is_valid_identifier("has space", 64));
  FSL_CHECK(!fsl::is_valid_identifier("slash/inside", 64));
  FSL_CHECK(!fsl::is_valid_identifier("toolong", 3));
}

FSL_TEST(subject_key_rejects_path_traversal_and_separators) {
  FSL_CHECK(fsl::is_valid_subject_key("rack-1", 128));
  FSL_CHECK(fsl::is_valid_subject_key("site-a/hall-1/row-3", 128));
  FSL_CHECK(!fsl::is_valid_subject_key("..", 128));
  FSL_CHECK(!fsl::is_valid_subject_key("../etc/passwd", 128));
  FSL_CHECK(!fsl::is_valid_subject_key("a..b", 128));
  FSL_CHECK(!fsl::is_valid_subject_key("a\\b", 128));
  FSL_CHECK(!fsl::is_valid_subject_key("/leading", 128));
  FSL_CHECK(!fsl::is_valid_subject_key("trailing/", 128));
  FSL_CHECK(!fsl::is_valid_subject_key("", 128));
  FSL_CHECK(!fsl::is_valid_subject_key(std::string("embedded\0nul", 12), 128));
}

}  // namespace

FSL_TEST_MAIN("test_text")

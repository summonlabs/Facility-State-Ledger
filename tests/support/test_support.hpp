// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "fsl/clock.hpp"
#include "fsl/event.hpp"
#include "fsl/ledger.hpp"
#include "fsl/payload.hpp"
#include "fsl/provenance.hpp"

/// \file test_support.hpp
/// A minimal, dependency-free test harness plus fixtures shared by the suites.
///
/// Every check records a failure and continues; only an explicit REQUIRE stops
/// the current case. There are no timeouts anywhere: a hanging test is a defect
/// in the library, not something to kill.

namespace fsl_test {

struct Failure {
  std::string location;
  std::string message;
};

class Context {
 public:
  static Context& instance();

  void add_case(const std::string& name, std::function<void()> body);
  [[nodiscard]] int run_all(const char* suite);

  void check(bool condition, const char* expression, const char* file, int line);
  [[nodiscard]] bool require(bool condition, const char* expression, const char* file, int line);
  void note(const std::string& message);

  [[nodiscard]] std::uint64_t checks() const noexcept { return checks_; }
  [[nodiscard]] std::uint64_t failures() const noexcept { return failures_; }

 private:
  struct Case {
    std::string name;
    std::function<void()> body;
  };

  std::vector<Case> cases_;
  std::vector<Failure> current_failures_;
  std::string current_case_;
  std::uint64_t checks_ = 0;
  std::uint64_t failures_ = 0;
};

struct Registrar {
  Registrar(const char* name, std::function<void()> body);
};

/// A unique directory that removes itself, including on an early return.
class TempDirectory {
 public:
  explicit TempDirectory(const std::string& label);
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  ~TempDirectory();

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path child(const std::string& name) const;

 private:
  std::filesystem::path path_;
};

/// Deterministic ledger options used by most suites.
[[nodiscard]] fsl::LedgerOptions deterministic_options(std::uint64_t pinned_timestamp = 1767225600000000000ULL);

/// Builds a validated submission with a deterministic identity.
struct SubmissionBuilder {
  explicit SubmissionBuilder(fsl::EventKind kind);

  SubmissionBuilder& event_id(std::uint64_t high, std::uint64_t low);
  SubmissionBuilder& token(std::uint64_t high, std::uint64_t low);
  SubmissionBuilder& generation(std::uint64_t value);
  SubmissionBuilder& epoch(std::uint64_t value);
  SubmissionBuilder& subject(const std::string& text);
  SubmissionBuilder& subject(fsl::SubjectKind kind, const std::string& key);
  SubmissionBuilder& subject(const fsl::SubjectRef& reference);
  SubmissionBuilder& correction_target(std::uint64_t high, std::uint64_t low);
  SubmissionBuilder& payload_schema(const std::string& text);
  SubmissionBuilder& payload_schema_version(std::uint32_t value);
  SubmissionBuilder& payload(std::vector<std::uint8_t> bytes);
  SubmissionBuilder& source(const std::string& text);
  SubmissionBuilder& source_generation(std::uint64_t value);
  SubmissionBuilder& source_sequence(std::uint64_t value);

  [[nodiscard]] fsl::SubmittedObservationFields fields() const;
  [[nodiscard]] fsl::Result<fsl::SubmittedObservation> build() const;

 private:
  fsl::SubmittedObservationFields fields_;
};

[[nodiscard]] fsl::EventId event_id_from(std::uint64_t index);
[[nodiscard]] fsl::SubjectRef subject_ref(fsl::SubjectKind kind, const std::string& key);

}  // namespace fsl_test

#define FSL_TEST(name)                                                                    \
  static void fsl_test_body_##name();                                                     \
  static const ::fsl_test::Registrar fsl_test_registrar_##name(#name, &fsl_test_body_##name); \
  static void fsl_test_body_##name()

#define FSL_CHECK(expression) \
  ::fsl_test::Context::instance().check((expression), #expression, __FILE__, __LINE__)

#define FSL_CHECK_EQ(lhs, rhs)                                                              \
  do {                                                                                      \
    const auto& check_lhs = (lhs);                                                          \
    const auto& check_rhs = (rhs);                                                          \
    ::fsl_test::Context::instance().check(check_lhs == check_rhs, #lhs " == " #rhs, __FILE__, \
                                          __LINE__);                                        \
  } while (false)

#define FSL_REQUIRE(expression)                                                              \
  do {                                                                                       \
    if (!::fsl_test::Context::instance().require((expression), #expression, __FILE__, __LINE__)) { \
      return;                                                                                \
    }                                                                                        \
  } while (false)

/// Requires that a Result holds a value; on failure it records the status text.
#define FSL_REQUIRE_OK(result)                                                              \
  do {                                                                                      \
    auto&& check_result = (result);                                                         \
    if (!check_result.has_value()) {                                                        \
      ::fsl_test::Context::instance().note(std::string(#result) + " failed: " +             \
                                           check_result.status().to_string());              \
      ::fsl_test::Context::instance().check(false, #result " is ok", __FILE__, __LINE__);   \
      return;                                                                               \
    }                                                                                       \
  } while (false)

/// Requires that a Result is an error with exactly this code.
#define FSL_REQUIRE_ERROR(result, expected_code)                                            \
  do {                                                                                      \
    auto&& check_result = (result);                                                         \
    if (check_result.has_value()) {                                                         \
      ::fsl_test::Context::instance().check(false, #result " fails with " #expected_code,   \
                                             __FILE__, __LINE__);                           \
      return;                                                                               \
    }                                                                                       \
    if (check_result.status().code() != (expected_code)) {                                  \
      ::fsl_test::Context::instance().note(std::string(#result) + " reported " +            \
                                           check_result.status().to_string());              \
      ::fsl_test::Context::instance().check(false, #result " fails with " #expected_code,   \
                                             __FILE__, __LINE__);                           \
      return;                                                                               \
    }                                                                                       \
    ::fsl_test::Context::instance().check(true, #result " fails with " #expected_code,      \
                                           __FILE__, __LINE__);                             \
  } while (false)

#define FSL_TEST_MAIN(suite)                       \
  int main() { return ::fsl_test::Context::instance().run_all(suite); }

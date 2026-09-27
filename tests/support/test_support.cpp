// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "support/test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace fsl_test {
namespace {

std::atomic<std::uint64_t> g_directory_counter{0};

[[nodiscard]] std::string render(const char* file, int line) {
  const std::string path(file);
  const std::size_t slash = path.find_last_of("/\\");
  return (slash == std::string::npos ? path : path.substr(slash + 1)) + ":" + std::to_string(line);
}

}  // namespace

Context& Context::instance() {
  static Context context;
  return context;
}

void Context::add_case(const std::string& name, std::function<void()> body) {
  cases_.push_back(Case{name, std::move(body)});
}

void Context::note(const std::string& message) {
  current_failures_.push_back(Failure{current_case_, message});
}

void Context::check(bool condition, const char* expression, const char* file, int line) {
  ++checks_;
  if (condition) {
    return;
  }
  ++failures_;
  std::string message = render(file, line);
  message.append(": ");
  message.append(expression);
  current_failures_.push_back(Failure{current_case_, std::move(message)});
}

bool Context::require(bool condition, const char* expression, const char* file, int line) {
  check(condition, expression, file, line);
  return condition;
}

int Context::run_all(const char* suite) {
  std::uint64_t case_failures = 0;
  for (const Case& item : cases_) {
    current_case_ = item.name;
    const std::uint64_t before = failures_;
    const std::size_t failures_before = current_failures_.size();
    try {
      item.body();
    } catch (const std::exception& error) {
      ++failures_;
      current_failures_.push_back(
          Failure{item.name, std::string("unexpected exception: ") + error.what()});
    } catch (...) {
      ++failures_;
      current_failures_.push_back(Failure{item.name, "unexpected non-standard exception"});
    }
    if (failures_ != before) {
      ++case_failures;
      std::fprintf(stderr, "FAIL %s\n", item.name.c_str());
      for (std::size_t i = failures_before; i < current_failures_.size(); ++i) {
        std::fprintf(stderr, "  %s\n", current_failures_[i].message.c_str());
      }
    }
  }
  const bool ok = failures_ == 0;
  std::printf("%s: %s (%llu checks in %zu cases)\n", suite, ok ? "PASS" : "FAIL",
              static_cast<unsigned long long>(checks_), cases_.size());
  if (!ok) {
    std::printf("%s: %llu failing checks in %llu cases\n", suite,
                static_cast<unsigned long long>(failures_),
                static_cast<unsigned long long>(case_failures));
  }
  return ok ? 0 : 1;
}

Registrar::Registrar(const char* name, std::function<void()> body) {
  Context::instance().add_case(name, std::move(body));
}

TempDirectory::TempDirectory(const std::string& label) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto counter = g_directory_counter.fetch_add(1);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%s_%lld_%llu", label.c_str(),
                static_cast<long long>(stamp), static_cast<unsigned long long>(counter));
  path_ = std::filesystem::temp_directory_path() / "fsl_tests" / buffer;
  std::error_code error;
  std::filesystem::remove_all(path_, error);
  std::filesystem::create_directories(path_, error);
}

TempDirectory::~TempDirectory() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::filesystem::path TempDirectory::child(const std::string& name) const { return path_ / name; }

fsl::LedgerOptions deterministic_options(std::uint64_t pinned_timestamp) {
  fsl::LedgerOptions options;
  options.segment_timestamp_unix_nanos = pinned_timestamp;
  return options;
}

SubmissionBuilder::SubmissionBuilder(fsl::EventKind kind) {
  fields_.kind = kind;
  fields_.payload_schema = fsl::payload::required_schema(kind);
  fields_.payload_schema_version = fsl::payload::required_schema_version(kind);
  fields_.facility_generation = fsl::FacilityGeneration(1);
  if (kind != fsl::EventKind::kLedgerOpened && kind != fsl::EventKind::kGenerationAdvanced) {
    fields_.epoch = fsl::FacilityEpoch(1);
  }
  fields_.event_id = fsl::EventId::from_words(0xF51ULL, 0x0001ULL);
  fields_.subject = subject_ref(fsl::SubjectKind::kLedger, "facility-state-ledger");
  auto source = fsl::SourceComponentId::parse("test.harness");
  auto provenance = fsl::ProvenanceRecord::create(std::move(source).value(), fsl::SourceGeneration::first(),
                                                  std::nullopt, std::nullopt, {});
  fields_.provenance = std::move(provenance).value();
  fields_.payload = {};
}

SubmissionBuilder& SubmissionBuilder::event_id(std::uint64_t high, std::uint64_t low) {
  fields_.event_id = fsl::EventId::from_words(high, low);
  return *this;
}

SubmissionBuilder& SubmissionBuilder::token(std::uint64_t high, std::uint64_t low) {
  fields_.idempotency_token = fsl::IdempotencyToken::from_words(high, low);
  return *this;
}

SubmissionBuilder& SubmissionBuilder::generation(std::uint64_t value) {
  fields_.facility_generation = fsl::FacilityGeneration(value);
  return *this;
}

SubmissionBuilder& SubmissionBuilder::epoch(std::uint64_t value) {
  fields_.epoch = fsl::FacilityEpoch(value);
  return *this;
}

SubmissionBuilder& SubmissionBuilder::subject(const std::string& text) {
  auto reference = fsl::SubjectRef::parse(text);
  fields_.subject = std::move(reference).value();
  return *this;
}

SubmissionBuilder& SubmissionBuilder::subject(fsl::SubjectKind kind, const std::string& key) {
  fields_.subject = subject_ref(kind, key);
  return *this;
}

SubmissionBuilder& SubmissionBuilder::subject(const fsl::SubjectRef& reference) {
  fields_.subject = reference;
  return *this;
}

SubmissionBuilder& SubmissionBuilder::correction_target(std::uint64_t high, std::uint64_t low) {
  fields_.correction_target = fsl::EventId::from_words(high, low);
  return *this;
}

SubmissionBuilder& SubmissionBuilder::payload_schema(const std::string& text) {
  fields_.payload_schema = fsl::SchemaId::parse(text).value();
  return *this;
}

SubmissionBuilder& SubmissionBuilder::payload_schema_version(std::uint32_t value) {
  fields_.payload_schema_version = fsl::SchemaVersion(value);
  return *this;
}

SubmissionBuilder& SubmissionBuilder::payload(std::vector<std::uint8_t> bytes) {
  fields_.payload = std::move(bytes);
  return *this;
}

SubmissionBuilder& SubmissionBuilder::source(const std::string& text) {
  auto identifier = fsl::SourceComponentId::parse(text);
  auto existing = fields_.provenance.value();
  auto provenance = fsl::ProvenanceRecord::create(std::move(identifier).value(),
                                                  existing.source_generation(),
                                                  existing.source_sequence(),
                                                  std::nullopt, {});
  fields_.provenance = std::move(provenance).value();
  return *this;
}

SubmissionBuilder& SubmissionBuilder::source_generation(std::uint64_t value) {
  const fsl::ProvenanceRecord& existing = fields_.provenance.value();
  auto identifier = fsl::SourceComponentId::parse(existing.source().view());
  auto provenance = fsl::ProvenanceRecord::create(std::move(identifier).value(),
                                                  fsl::SourceGeneration(value),
                                                  existing.source_sequence(), std::nullopt, {});
  fields_.provenance = std::move(provenance).value();
  return *this;
}

SubmissionBuilder& SubmissionBuilder::source_sequence(std::uint64_t value) {
  const fsl::ProvenanceRecord& existing = fields_.provenance.value();
  auto identifier = fsl::SourceComponentId::parse(existing.source().view());
  auto provenance = fsl::ProvenanceRecord::create(std::move(identifier).value(),
                                                  existing.source_generation(),
                                                  fsl::SourceSequence(value), std::nullopt, {});
  fields_.provenance = std::move(provenance).value();
  return *this;
}

fsl::SubmittedObservationFields SubmissionBuilder::fields() const { return fields_; }

fsl::Result<fsl::SubmittedObservation> SubmissionBuilder::build() const {
  return fsl::SubmittedObservation::create(fields_);
}

fsl::EventId event_id_from(std::uint64_t index) { return fsl::EventId::from_words(0xA5A5A5A5ULL, index); }

fsl::SubjectRef subject_ref(fsl::SubjectKind kind, const std::string& key) {
  return std::move(fsl::SubjectRef::create(kind, key)).value();
}

}  // namespace fsl_test

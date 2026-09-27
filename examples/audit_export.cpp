// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "fsl/ledger.hpp"

/// \file audit_export.cpp
/// Streams the canonical audit export of a ledger to stdout and reports, on
/// stderr, what was exported and whether the ledger verifies.
///
/// Usage: fsl_example_audit_export <ledger-directory>

namespace {

class CountingSink final : public fsl::IAuditSink {
 public:
  bool write(std::string_view record) override {
    const std::size_t written = std::fwrite(record.data(), 1, record.size(), stdout);
    if (written != record.size()) {
      failed_ = true;
      return false;
    }
    bytes_ += written;
    if (!record.empty() && record.front() == '{' && record.find("\"record\":\"header\"") == std::string_view::npos &&
        record.find("\"record\":\"trailer\"") == std::string_view::npos) {
      ++records_;
    }
    return true;
  }

  [[nodiscard]] std::uint64_t records() const noexcept { return records_; }
  [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }

 private:
  std::uint64_t records_ = 0;
  std::uint64_t bytes_ = 0;
  bool failed_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: fsl_example_audit_export <ledger-directory>\n");
    return 1;
  }
  const std::filesystem::path directory = argv[1];

  // The export is a read-only operation and never takes the writer role.
  auto opened = fsl::Ledger::open(directory, fsl::OpenMode::kReadOnly);
  if (!opened.has_value()) {
    std::fprintf(stderr, "open failed: %s\n", opened.status().to_string().c_str());
    return 1;
  }
  fsl::Ledger ledger = std::move(opened).value();

  const auto watermark = ledger.watermark();
  if (!watermark.has_value()) {
    std::fprintf(stderr, "watermark failed: %s\n", watermark.status().to_string().c_str());
    return 1;
  }

  CountingSink sink;
  const fsl::AuditExportRequest request;
  const fsl::Status exported = ledger.export_audit(request, sink);
  if (exported.is_error()) {
    std::fprintf(stderr, "export failed: %s\n", exported.to_string().c_str());
    return 1;
  }

  std::string last_integrity = "none";
  if (watermark->sequence.has_value()) {
    auto last = ledger.read(*watermark->sequence);
    if (!last.has_value()) {
      std::fprintf(stderr, "read failed: %s\n", last.status().to_string().c_str());
      return 1;
    }
    last_integrity = last->integrity().to_hex();
  }

  const fsl::VerifyReport report = ledger.verify();
  std::fprintf(stderr, "records=%llu bytes=%llu last_sequence=%s last_integrity=%s verify=%s\n",
               static_cast<unsigned long long>(sink.records()),
               static_cast<unsigned long long>(sink.bytes()),
               watermark->sequence.has_value()
                   ? std::to_string(watermark->sequence->value()).c_str()
                   : "none",
               last_integrity.c_str(), report.status.to_string().c_str());

  const auto closed = ledger.close();
  if (!closed.has_value()) {
    std::fprintf(stderr, "close failed: %s\n", closed.status().to_string().c_str());
    return 1;
  }
  return (report.ok() && !sink.failed()) ? 0 : 1;
}

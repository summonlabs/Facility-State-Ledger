// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

/// \file fsl_bench.cpp
/// The Facility State Ledger benchmark executable.
///
/// Every number this program prints is SYNTHETIC: it characterises one build of
/// this library on one machine, never production hardware.
///
/// The benchmark measures completed work only. There is no submission, queueing
/// or enqueue latency anywhere in the library, so there is none here: an append
/// is timed from the call to its return. For a durable handle that return
/// happens only after the segment flush and after the manifest publication have
/// completed, which is exactly what a durable measurement must include. The
/// report says so on every scenario that depends on it and prints the handle's
/// own flush-call counters as evidence.
///
/// The workload is deterministic: event identities come from counters combined
/// with EventId::from_words, the recorded monotonic value comes from a
/// ManualClock advanced by a fixed step per submitted event, the observed
/// subject is chosen by a fixed-seed SplitMix64 generator, and the segment
/// header timestamp is pinned. The same command line therefore produces the
/// same workload on every run.
///
/// Subject lifecycle
/// -----------------
/// A subject's lifecycle is changed only by its registration and its retirement;
/// every other event about a subject leaves it live. The workload therefore
/// registers its bounded pool once and records every observation against that
/// pool without further lifecycle traffic.
///
/// The program owns the ledger directory it is given. It removes that tree
/// before creating the ledger, so a stale directory can never make the
/// deterministic workload collide with committed identities, and removes it
/// again before exiting, including when a scenario fails.
///
/// There are no timeouts and no watchdog logic: a hang would be a defect in the
/// library, not something this program hides by killing it.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "fsl/clock.hpp"
#include "fsl/counter.hpp"
#include "fsl/event.hpp"
#include "fsl/ids.hpp"
#include "fsl/ledger.hpp"
#include "fsl/options.hpp"
#include "fsl/payload.hpp"
#include "fsl/provenance.hpp"
#include "fsl/query.hpp"
#include "fsl/state.hpp"
#include "fsl/status.hpp"
#include "fsl/subject.hpp"
#include "fsl/version.hpp"

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

/// Seed of the fixed-seed workloads. Changing it changes the workload, never the
/// shape of the report.
constexpr std::uint64_t kWorkloadSeed = 0x5EEDF00DCAFEBABEULL;
/// Recorded monotonic nanoseconds added per submitted event.
constexpr std::uint64_t kMonotonicStepNanos = 1000ULL;
/// Pinned informational wall clock written into segment headers: 2026-01-01Z.
constexpr std::uint64_t kPinnedUnixNanos = 1767225600000000000ULL;
/// Identity spaces. Control events (lifecycle and registrations) and observed
/// events never share one, and the low word is a ledger-global counter inside
/// each space, so a producer's per-source sequence is strictly increasing across
/// every scenario.
constexpr std::uint64_t kControlIdentityHigh = 0x0100ULL;
constexpr std::uint64_t kSetupIdentityHigh = 0x0200ULL;
constexpr std::uint64_t kSingleIdentityHigh = 0x0300ULL;
constexpr std::uint64_t kBatchIdentityHigh = 0x0400ULL;
constexpr std::uint64_t kVolatileIdentityHigh = 0x0500ULL;
/// Provenance sources.
constexpr const char* kControlSource = "fsl.bench.control";
constexpr const char* kProducerSource = "fsl.bench.producer";
/// LedgerOptions::max_batch_events and max_query_limit defaults.
constexpr std::size_t kMaxBatchEvents = 1024;
constexpr std::size_t kMaxQueryLimit = 4096;
/// LedgerOptions::max_payload_bytes default, minus the framing the encoder adds.
constexpr std::size_t kMaxPayloadBytes = 65'000;
/// Upper bound on the number of operations one query measurement performs.
constexpr std::uint64_t kMaxQueryOperations = 1000;
/// Largest latency-sample reservation, so a huge --events cannot ask for a
/// pathological up-front allocation.
constexpr std::size_t kMaxLatencyReserve = 1'000'000;
/// Smallest segment size worth configuring.
constexpr std::size_t kMinSegmentBytes = 4096;

constexpr const char* kRule =
    "================================================================================\n";
constexpr const char* kThinRule =
    "--------------------------------------------------------------------------------\n";

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

/// Narrow a path to UTF-8 text for printing and for CSV.
[[nodiscard]] std::string narrow(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  std::string out;
  out.reserve(text.size());
  for (const char8_t unit : text) {
    out.push_back(static_cast<char>(unit));
  }
  return out;
}

/// One decimal place at most, and never a fabricated value.
[[nodiscard]] std::string fixed1(double value) {
  if (!std::isfinite(value)) {
    return "n/a";
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.1f", value);
  return std::string(buffer);
}

[[nodiscard]] std::string compiler_description() {
#if defined(__clang__)
  return std::string("Clang ") + __clang_version__;
#elif defined(_MSC_VER)
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "MSVC %d.%d (full version %d)",
                static_cast<int>(_MSC_VER / 100), static_cast<int>(_MSC_VER % 100),
                static_cast<int>(_MSC_FULL_VER));
  return std::string(buffer);
#elif defined(__GNUC__)
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "GCC %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
  return std::string(buffer);
#else
  return "unknown compiler";
#endif
}

[[nodiscard]] std::string build_description() {
#if defined(NDEBUG)
  return "NDEBUG defined (asserts off)";
#else
  return "NDEBUG not defined (asserts on)";
#endif
}

[[nodiscard]] std::string platform_description() {
#if defined(_WIN32)
  return "Windows (_WIN32=1)";
#else
  return "POSIX (_WIN32=0)";
#endif
}

[[nodiscard]] std::string platform_flag() {
#if defined(_WIN32)
  return "_WIN32=1";
#else
  return "_WIN32=0";
#endif
}

[[nodiscard]] std::string ndebug_flag() {
#if defined(NDEBUG)
  return "NDEBUG=1";
#else
  return "NDEBUG=0";
#endif
}

[[nodiscard]] unsigned long current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<unsigned long>(::_getpid());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

[[nodiscard]] double seconds_between(Clock::time_point start, Clock::time_point finish) {
  return std::chrono::duration<double>(finish - start).count();
}

[[nodiscard]] double micros_between(Clock::time_point start, Clock::time_point finish) {
  return std::chrono::duration<double, std::micro>(finish - start).count();
}

[[nodiscard]] std::string csv_field(std::string_view text) {
  bool needs_quotes = false;
  for (const char unit : text) {
    if (unit == ',' || unit == '"' || unit == '\n' || unit == '\r') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    return std::string(text);
  }
  std::string out = "\"";
  for (const char unit : text) {
    if (unit == '"') {
      out += "\"\"";
    } else {
      out += unit;
    }
  }
  out += "\"";
  return out;
}

// ---------------------------------------------------------------------------
// Latency statistics
// ---------------------------------------------------------------------------

struct LatencyStats {
  std::uint64_t samples = 0;
  double mean_us = 0.0;
  double p50_us = 0.0;
  double p99_us = 0.0;
  double max_us = 0.0;
};

/// Nearest-rank percentile: the smallest index whose cumulative fraction reaches
/// `percentile`. Never interpolates, so no precision is invented.
[[nodiscard]] std::size_t percentile_index(std::size_t count, double percentile) {
  if (count == 0) {
    return 0;
  }
  double ceiling = std::ceil(percentile * static_cast<double>(count));
  if (!(ceiling >= 1.0)) {
    ceiling = 1.0;
  }
  if (ceiling > static_cast<double>(count)) {
    ceiling = static_cast<double>(count);
  }
  return static_cast<std::size_t>(ceiling) - 1;
}

[[nodiscard]] LatencyStats summarize(std::vector<double> samples) {
  LatencyStats stats;
  stats.samples = static_cast<std::uint64_t>(samples.size());
  if (samples.empty()) {
    return stats;
  }
  double total = 0.0;
  for (const double value : samples) {
    total += value;
  }
  stats.mean_us = total / static_cast<double>(samples.size());
  std::sort(samples.begin(), samples.end());
  stats.p50_us = samples[percentile_index(samples.size(), 0.50)];
  stats.p99_us = samples[percentile_index(samples.size(), 0.99)];
  stats.max_us = samples.back();
  return stats;
}

// ---------------------------------------------------------------------------
// Report model
// ---------------------------------------------------------------------------

struct Metric {
  std::string name;
  std::string value;
  std::string unit;
};

/// Everything needed to reproduce one reported scenario.
struct Context {
  std::string scenario;
  std::uint64_t events = 0;
  std::size_t batch = 0;
  std::size_t repeat = 0;
  std::size_t query_limit = 0;
  std::size_t subjects = 0;
  std::size_t max_segment_bytes = 0;
  bool durable_commits = false;
  bool run_durable = false;
};

struct Section {
  Context context;
  std::vector<std::string> notes;
  std::vector<Metric> metrics;
  bool passed = true;
};

void add_metric(Section& section, std::string name, std::string value, std::string unit) {
  section.metrics.push_back(Metric{std::move(name), std::move(value), std::move(unit)});
}

void add_latency_metrics(Section& section, const std::string& prefix, const LatencyStats& stats) {
  add_metric(section, prefix + "_samples", std::to_string(stats.samples), "count");
  add_metric(section, prefix + "_mean_us", fixed1(stats.mean_us), "us");
  add_metric(section, prefix + "_p50_us", fixed1(stats.p50_us), "us");
  add_metric(section, prefix + "_p99_us", fixed1(stats.p99_us), "us");
  add_metric(section, prefix + "_max_us", fixed1(stats.max_us), "us");
}

class Report {
 public:
  void set_ledger_directory(std::string directory) { ledger_directory_ = std::move(directory); }
  void set_build_summary(std::string summary) { build_summary_ = std::move(summary); }
  void set_run_context(Context context) { run_context_ = std::move(context); }
  void set_environment(std::vector<Metric> metrics) { environment_ = std::move(metrics); }
  void set_run_configuration(std::vector<Metric> metrics) { run_configuration_ = std::move(metrics); }
  void add_section(Section section) { sections_.push_back(std::move(section)); }

  [[nodiscard]] std::size_t scenario_count() const noexcept { return sections_.size(); }

  [[nodiscard]] std::size_t failed_scenario_count() const noexcept {
    std::size_t failed = 0;
    for (const Section& section : sections_) {
      if (!section.passed) {
        ++failed;
      }
    }
    return failed;
  }

  void print() const {
    std::fputs(kRule, stdout);
    std::fputs("Facility State Ledger benchmark\n", stdout);
    std::fputs("Every numeric result below is SYNTHETIC: it characterises this build on this\n", stdout);
    std::fputs("machine. It is not a measurement of production hardware.\n", stdout);
    std::fputs("Completed work only: an append is timed from the call to its return, and on a\n", stdout);
    std::fputs("durable handle that return is after the segment flush and the manifest\n", stdout);
    std::fputs("publication. Every durable duration therefore includes those flushes.\n", stdout);
    std::fputs(kRule, stdout);

    print_block("[environment]", environment_);
    print_block("[run configuration]", run_configuration_);

    for (const Section& section : sections_) {
      std::fputs(kThinRule, stdout);
      std::printf("[scenario %s]\n", section.context.scenario.c_str());
      std::printf("  build    : %s\n", build_summary_.c_str());
      std::printf("  context  : %s\n", context_line(section.context).c_str());
      std::printf("  ledger   : %s\n", ledger_directory_.c_str());
      for (const std::string& note : section.notes) {
        std::printf("  note     : %s\n", note.c_str());
      }
      for (const Metric& metric : section.metrics) {
        print_metric("SYNTHETIC", metric);
      }
      std::printf("  INTEGRITY  scenario_result            : %s\n", section.passed ? "PASS" : "FAIL");
    }
    std::fputs(kRule, stdout);
  }

  [[nodiscard]] bool write_csv(const std::filesystem::path& path, std::string& error) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "cannot open " + narrow(path) + " for writing";
      return false;
    }
    out << "scenario,metric,value,unit,events,batch_size,repeat,query_limit,subjects,"
           "max_segment_bytes,durable_commits,run_durable,ledger_directory,status\n";
    for (const Metric& metric : environment_) {
      write_row(out, run_context_, metric, "context");
    }
    for (const Metric& metric : run_configuration_) {
      write_row(out, run_context_, metric, "context");
    }
    for (const Section& section : sections_) {
      for (const Metric& metric : section.metrics) {
        write_row(out, section.context, metric, section.passed ? "pass" : "fail");
      }
    }
    out.flush();
    if (!out) {
      error = "failed while writing " + narrow(path);
      return false;
    }
    return true;
  }

 private:
  static void print_padded(const char* prefix, const std::string& name, const Metric& metric) {
    std::string padded = name;
    while (padded.size() < 32) {
      padded.push_back(' ');
    }
    if (metric.unit.empty()) {
      std::printf("%s  %s : %s\n", prefix, padded.c_str(), metric.value.c_str());
    } else {
      std::printf("%s  %s : %s %s\n", prefix, padded.c_str(), metric.value.c_str(), metric.unit.c_str());
    }
  }

  static void print_metric(const char* label, const Metric& metric) {
    std::string prefix = "  ";
    prefix += label;
    while (prefix.size() < 11) {
      prefix.push_back(' ');
    }
    print_padded(prefix.c_str(), metric.name, metric);
  }

  static void print_block(const char* title, const std::vector<Metric>& metrics) {
    std::printf("%s\n", title);
    for (const Metric& metric : metrics) {
      print_padded("  ", metric.name, metric);
    }
    std::fputs("\n", stdout);
  }

  [[nodiscard]] static std::string context_line(const Context& context) {
    return "events=" + std::to_string(context.events) + " batch=" + std::to_string(context.batch) +
           " repeat=" + std::to_string(context.repeat) +
           " query_limit=" + std::to_string(context.query_limit) +
           " subjects=" + std::to_string(context.subjects) +
           " max_segment_bytes=" + std::to_string(context.max_segment_bytes) +
           " durable_commits=" + (context.durable_commits ? "true" : "false") +
           " run_durable=" + (context.run_durable ? "yes" : "no");
  }

  void write_row(std::ofstream& out,
                 const Context& context,
                 const Metric& metric,
                 const char* status) const {
    out << csv_field(context.scenario) << ',' << csv_field(metric.name) << ',' << csv_field(metric.value)
        << ',' << csv_field(metric.unit) << ',' << context.events << ',' << context.batch << ','
        << context.repeat << ',' << context.query_limit << ',' << context.subjects << ','
        << context.max_segment_bytes << ',' << (context.durable_commits ? "true" : "false") << ','
        << (context.run_durable ? "yes" : "no") << ',' << csv_field(ledger_directory_) << ',' << status
        << '\n';
  }

  std::string ledger_directory_;
  std::string build_summary_;
  Context run_context_;
  std::vector<Metric> environment_;
  std::vector<Metric> run_configuration_;
  std::vector<Section> sections_;
};

// ---------------------------------------------------------------------------
// Integrity checks
// ---------------------------------------------------------------------------

class Checker {
 public:
  void check(bool condition, std::string description) {
    ++checks_;
    if (condition) {
      return;
    }
    ++failure_count_;
    failure_messages_.push_back(std::move(description));
  }

  [[nodiscard]] std::uint64_t checks() const noexcept { return checks_; }
  [[nodiscard]] std::uint64_t failure_count() const noexcept { return failure_count_; }
  [[nodiscard]] const std::vector<std::string>& failures() const noexcept { return failure_messages_; }

 private:
  std::uint64_t checks_ = 0;
  std::uint64_t failure_count_ = 0;
  std::vector<std::string> failure_messages_;
};

/// Per-scenario view of the checker, so that one scenario's PASS/FAIL cannot be
/// confused with the outcome of an earlier one.
class CheckMark {
 public:
  explicit CheckMark(const Checker& checker) noexcept
      : checker_(checker), checks_before_(checker.checks()), failures_before_(checker.failure_count()) {}

  [[nodiscard]] std::uint64_t checks() const noexcept { return checker_.checks() - checks_before_; }
  [[nodiscard]] bool passed() const noexcept { return checker_.failure_count() == failures_before_; }

 private:
  const Checker& checker_;
  std::uint64_t checks_before_ = 0;
  std::uint64_t failures_before_ = 0;
};

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct Options {
  std::string scenario = "all";
  std::uint64_t events = 20000;
  std::size_t batch = 64;
  std::size_t repeat = 1;
  std::size_t limit = 64;
  std::size_t subjects = 32;
  std::size_t payload_bytes = 32;
  std::size_t segment_bytes = 64u * 1024u * 1024u;
  std::filesystem::path directory;
  std::optional<std::filesystem::path> csv;
};

struct CommandLine {
  enum class Kind { kRun, kHelp, kError };
  Kind kind = Kind::kRun;
  std::string message;
  Options options;
};

void print_usage(std::FILE* stream) {
  std::fputs("usage: fsl_bench [options]\n", stream);
  std::fputs("\n", stream);
  std::fputs("  --scenario NAME     append-single | append-batch | append-volatile | replay |\n", stream);
  std::fputs("                      verify | query | reopen | all        (default: all)\n", stream);
  std::fputs("  --events N          events in the workload                (default: 20000)\n", stream);
  std::fputs("  --batch N           events per atomic batch append        (default: 64)\n", stream);
  std::fputs("  --repeat N          measurement rounds                    (default: 1)\n", stream);
  std::fputs("  --limit N           page limit for bounded queries        (default: 64)\n", stream);
  std::fputs("  --subjects N        rack subjects registered up front     (default: 32)\n", stream);
  std::fputs("  --payload-bytes N   observation body bytes                (default: 32)\n", stream);
  std::fputs("  --segment-bytes N   LedgerOptions::max_segment_bytes      (default: 67108864)\n", stream);
  std::fputs("  --dir PATH          ledger directory (removed before and after the run)\n", stream);
  std::fputs("                      (default: <system temp>/fsl_bench_<pid>)\n", stream);
  std::fputs("  --csv PATH          write the same results as CSV\n", stream);
  std::fputs("  --help              print this text\n", stream);
  std::fputs("\n", stream);
  std::fputs("Append scenarios aggregate every round; read-only scenarios keep the best round.\n", stream);
}

[[nodiscard]] bool parse_unsigned(std::string_view text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char unit : text) {
    if (unit < '0' || unit > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(unit - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10ULL) {
      return false;
    }
    value = value * 10ULL + digit;
  }
  out = value;
  return true;
}

[[nodiscard]] bool is_known_scenario(std::string_view name) {
  return name == "append-single" || name == "append-batch" || name == "append-volatile" ||
         name == "replay" || name == "verify" || name == "query" || name == "reopen" || name == "all";
}

[[nodiscard]] bool is_filesystem_root(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(path, error);
  if (error) {
    return true;
  }
  return absolute == absolute.root_path();
}

struct NumericFlag {
  std::string_view name;
  std::uint64_t minimum;
  std::uint64_t maximum;
};

[[nodiscard]] std::size_t numeric_flag_slot(std::string_view argument,
                                            const NumericFlag* flags,
                                            std::size_t count) {
  for (std::size_t slot = 0; slot < count; ++slot) {
    if (argument == flags[slot].name) {
      return slot;
    }
  }
  return count;
}

[[nodiscard]] CommandLine parse_command_line(int argc, char** argv) {
  CommandLine result;
  Options options;
  std::optional<std::filesystem::path> directory;
  bool help = false;

  const NumericFlag numeric_flags[] = {
      {"--events", 1, std::numeric_limits<std::uint64_t>::max()},
      {"--batch", 1, static_cast<std::uint64_t>(kMaxBatchEvents)},
      {"--repeat", 1, 1000},
      {"--limit", 1, static_cast<std::uint64_t>(kMaxQueryLimit)},
      {"--subjects", 1, 4096},
      {"--payload-bytes", 0, static_cast<std::uint64_t>(kMaxPayloadBytes)},
      {"--segment-bytes", static_cast<std::uint64_t>(kMinSegmentBytes),
       std::numeric_limits<std::uint64_t>::max()},
  };
  constexpr std::size_t kFlagCount = sizeof(numeric_flags) / sizeof(numeric_flags[0]);

  for (int index = 1; index < argc; ++index) {
    std::string argument = argv[index];
    std::string value;
    bool has_value = false;
    const std::size_t equals = argument.find('=');
    if (equals != std::string::npos) {
      value = argument.substr(equals + 1);
      argument = argument.substr(0, equals);
      has_value = true;
    }
    const auto take_value = [&](std::string& target) -> bool {
      if (has_value) {
        target = value;
        return true;
      }
      if (index + 1 >= argc) {
        return false;
      }
      ++index;
      target = argv[index];
      return true;
    };

    if (argument == "--help" || argument == "-h") {
      help = true;
      continue;
    }
    if (argument == "--scenario") {
      std::string text;
      if (!take_value(text) || !is_known_scenario(text)) {
        result.kind = CommandLine::Kind::kError;
        result.message =
            "--scenario must be append-single, append-batch, append-volatile, replay, verify, "
            "query, reopen or all";
        return result;
      }
      options.scenario = text;
      continue;
    }
    if (argument == "--dir") {
      std::string text;
      if (!take_value(text) || text.empty()) {
        result.kind = CommandLine::Kind::kError;
        result.message = "--dir requires a non-empty path";
        return result;
      }
      directory = std::filesystem::path(text);
      continue;
    }
    if (argument == "--csv") {
      std::string text;
      if (!take_value(text) || text.empty()) {
        result.kind = CommandLine::Kind::kError;
        result.message = "--csv requires a non-empty path";
        return result;
      }
      options.csv = std::filesystem::path(text);
      continue;
    }

    const std::size_t slot = numeric_flag_slot(argument, numeric_flags, kFlagCount);
    if (slot < kFlagCount) {
      std::uint64_t numeric_value = 0;
      std::string text;
      const bool in_range = take_value(text) && parse_unsigned(text, numeric_value) &&
                            numeric_value >= numeric_flags[slot].minimum &&
                            numeric_value <= numeric_flags[slot].maximum;
      if (!in_range) {
        result.kind = CommandLine::Kind::kError;
        result.message = std::string(numeric_flags[slot].name) + " requires an integer in [" +
                         std::to_string(numeric_flags[slot].minimum) + ", " +
                         std::to_string(numeric_flags[slot].maximum) + "]";
        return result;
      }
      switch (slot) {
        case 0:
          options.events = numeric_value;
          break;
        case 1:
          options.batch = static_cast<std::size_t>(numeric_value);
          break;
        case 2:
          options.repeat = static_cast<std::size_t>(numeric_value);
          break;
        case 3:
          options.limit = static_cast<std::size_t>(numeric_value);
          break;
        case 4:
          options.subjects = static_cast<std::size_t>(numeric_value);
          break;
        case 5:
          options.payload_bytes = static_cast<std::size_t>(numeric_value);
          break;
        default:
          options.segment_bytes = static_cast<std::size_t>(numeric_value);
          break;
      }
      continue;
    }

    result.kind = CommandLine::Kind::kError;
    result.message = "unknown argument '" + argument + "'";
    return result;
  }

  if (help) {
    result.kind = CommandLine::Kind::kHelp;
    return result;
  }

  if (directory.has_value()) {
    std::error_code error;
    options.directory = std::filesystem::absolute(*directory, error);
    if (error) {
      result.kind = CommandLine::Kind::kError;
      result.message = "--dir could not be resolved: " + narrow(*directory);
      return result;
    }
  } else {
    std::error_code error;
    std::filesystem::path temp = std::filesystem::temp_directory_path(error);
    if (error) {
      result.kind = CommandLine::Kind::kError;
      result.message = "the system temporary directory is unavailable";
      return result;
    }
    temp /= "fsl_bench_" + std::to_string(current_process_id());
    options.directory = temp;
  }
  if (is_filesystem_root(options.directory)) {
    result.kind = CommandLine::Kind::kError;
    result.message = "refusing to use a filesystem root as --dir: " + narrow(options.directory);
    return result;
  }
  result.options = std::move(options);
  return result;
}

// ---------------------------------------------------------------------------
// Directory guard
// ---------------------------------------------------------------------------

/// Owns the ledger directory. Removal happens on every exit path, including an
/// early return after a failed scenario.
class DirectoryGuard {
 public:
  explicit DirectoryGuard(std::filesystem::path path) : path_(std::move(path)) {}
  DirectoryGuard(const DirectoryGuard&) = delete;
  DirectoryGuard& operator=(const DirectoryGuard&) = delete;
  ~DirectoryGuard() { remove_now(); }

  void remove_now() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] bool exists() const {
    std::error_code error;
    return std::filesystem::exists(path_, error) && !error;
  }

 private:
  std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// Deterministic workload
// ---------------------------------------------------------------------------

/// SplitMix64. Fixed seed, so every run replays exactly the same subject pattern.
class DeterministicRandom {
 public:
  explicit DeterministicRandom(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
  }

  [[nodiscard]] std::size_t below(std::size_t bound) noexcept {
    if (bound == 0) {
      return 0;
    }
    return static_cast<std::size_t>(next() % static_cast<std::uint64_t>(bound));
  }

 private:
  std::uint64_t state_;
};

[[nodiscard]] fsl::Result<fsl::ProvenanceRecord> make_provenance(std::string_view source,
                                                                 std::uint64_t sequence) {
  auto identifier = fsl::SourceComponentId::parse(source);
  if (!identifier.has_value()) {
    return identifier.status();
  }
  return fsl::ProvenanceRecord::create(std::move(identifier).value(), fsl::SourceGeneration::first(),
                                       fsl::SourceSequence(sequence),
                                       kPinnedUnixNanos + sequence * 1000ULL, {});
}

/// Builds the kObservationAccepted submissions every append scenario uses.
class ObservationFactory {
 public:
  ObservationFactory(fsl::SourceComponentId source,
                     fsl::FacilityEpoch epoch,
                     fsl::FacilityGeneration generation,
                     std::size_t payload_bytes)
      : source_(std::move(source)), epoch_(epoch), generation_(generation), payload_bytes_(payload_bytes) {}

  [[nodiscard]] fsl::Result<fsl::SubmittedObservation> build(std::uint64_t identity_high,
                                                             std::uint64_t sequence,
                                                             const fsl::SubjectRef& subject) const {
    fsl::payload::ObservationAccepted observation;
    observation.observation_ref = "obs-" + std::to_string(sequence);
    observation.body.resize(payload_bytes_);
    for (std::size_t offset = 0; offset < payload_bytes_; ++offset) {
      const std::uint64_t mixed =
          sequence * 31ULL + static_cast<std::uint64_t>(offset) * 17ULL + identity_high;
      observation.body[offset] = static_cast<std::uint8_t>(mixed & 0xFFULL);
    }
    auto provenance = fsl::ProvenanceRecord::create(source_, fsl::SourceGeneration::first(),
                                                    fsl::SourceSequence(sequence),
                                                    kPinnedUnixNanos + sequence * 1000ULL, {});
    if (!provenance.has_value()) {
      return provenance.status();
    }
    fsl::SubmittedObservationFields fields;
    fields.event_id = fsl::EventId::from_words(identity_high, sequence);
    fields.facility_generation = generation_;
    fields.epoch = epoch_;
    fields.kind = fsl::EventKind::kObservationAccepted;
    fields.subject = subject;
    fields.payload_schema = fsl::payload::required_schema(fsl::EventKind::kObservationAccepted);
    fields.payload_schema_version =
        fsl::payload::required_schema_version(fsl::EventKind::kObservationAccepted);
    fields.payload = fsl::payload::encode(observation);
    fields.provenance = std::move(provenance).value();
    return fsl::SubmittedObservation::create(std::move(fields));
  }

 private:
  fsl::SourceComponentId source_;
  fsl::FacilityEpoch epoch_{1};
  fsl::FacilityGeneration generation_{1};
  std::size_t payload_bytes_ = 0;
};

/// The facility the workload writes against: one open epoch, a bounded set of
/// location subjects, and a bounded pool of rack subjects that observations are
/// recorded against.
struct Facility {
  std::vector<fsl::SubjectRef> racks;
  fsl::FacilityEpoch epoch{1};
  fsl::FacilityGeneration generation{1};
};

/// Latency samples for one measurement, plus how many completed operations and
/// events they cover.
struct AppendMeasure {
  std::vector<double> operation_micros;
  std::uint64_t operations = 0;
  std::uint64_t events = 0;
};

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

/// One ledger directory plus the handle the scenarios open against it.
///
/// `clock_` is declared before `ledger_` so that the handle is destroyed first
/// and never outlives the clock it was given.
class Session {
 public:
  Session(std::filesystem::path directory, const Options& options)
      : directory_(std::move(directory)), clock_(0, kPinnedUnixNanos) {
    durable_options_.max_segment_bytes = options.segment_bytes;
    durable_options_.durable_commits = true;
    durable_options_.clock = &clock_;
    durable_options_.segment_timestamp_unix_nanos = kPinnedUnixNanos;

    volatile_options_.max_segment_bytes = options.segment_bytes;
    volatile_options_.durable_commits = false;
    volatile_options_.clock = &clock_;
    volatile_options_.segment_timestamp_unix_nanos = kPinnedUnixNanos;
  }

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  [[nodiscard]] fsl::ManualClock& clock() noexcept { return clock_; }
  [[nodiscard]] fsl::Ledger& ledger() { return *ledger_; }

  [[nodiscard]] fsl::Status create(bool durable) {
    auto created = fsl::Ledger::create(directory_, durable ? durable_options_ : volatile_options_);
    if (!created.has_value()) {
      return created.status();
    }
    ledger_.emplace(std::move(created).value());
    return fsl::Status::ok();
  }

  [[nodiscard]] fsl::Status open(bool durable) {
    auto opened =
        fsl::Ledger::open(directory_, fsl::OpenMode::kReadWrite, durable ? durable_options_
                                                                         : volatile_options_);
    if (!opened.has_value()) {
      return opened.status();
    }
    ledger_.emplace(std::move(opened).value());
    return fsl::Status::ok();
  }

  [[nodiscard]] fsl::Status close() {
    if (!ledger_.has_value()) {
      return fsl::Status::ok();
    }
    auto closed = ledger_->close();
    ledger_.reset();
    if (!closed.has_value()) {
      return closed.status();
    }
    return fsl::Status::ok();
  }

 private:
  std::filesystem::path directory_;
  fsl::ManualClock clock_;
  fsl::LedgerOptions durable_options_;
  fsl::LedgerOptions volatile_options_;
  std::optional<fsl::Ledger> ledger_;
};

// ---------------------------------------------------------------------------
// Benchmark
// ---------------------------------------------------------------------------

class Benchmark {
 public:
  Benchmark(const Options& options, Session& session, Report& report)
      : options_(options), session_(session), report_(report) {}

  [[nodiscard]] const Checker& checks() const noexcept { return checks_; }

  [[nodiscard]] fsl::Status run() {
    fsl::Status status = setup();
    if (status.is_error()) {
      return status;
    }
    const std::string& scenario = options_.scenario;
    if (scenario == "append-single") {
      return run_append_single();
    }
    if (scenario == "append-batch") {
      return run_append_batch();
    }
    if (scenario == "append-volatile") {
      return run_append_volatile();
    }
    if (scenario == "replay") {
      return run_replay();
    }
    if (scenario == "verify") {
      return run_verify();
    }
    if (scenario == "query") {
      return run_query();
    }
    if (scenario == "reopen") {
      return run_reopen();
    }
    // Every scenario runs in the documented order even when an earlier one
    // failed, so one broken measurement cannot hide the others. The first
    // failure is still the process exit status.
    fsl::Status first_failure = fsl::Status::ok();
    const auto record = [&first_failure](const fsl::Status& outcome) {
      if (outcome.is_error() && first_failure.is_ok()) {
        first_failure = outcome;
      }
    };
    record(run_append_single());
    record(run_append_batch());
    record(run_append_volatile());
    record(run_replay());
    record(run_verify());
    record(run_query());
    record(run_reopen());
    return first_failure;
  }

 private:
  // -- Setup ----------------------------------------------------------------

  [[nodiscard]] fsl::Status setup() {
    fsl::Status status = session_.create(options_.scenario == "append-volatile");
    if (status.is_error()) {
      return status;
    }
    status = prepare_facility();
    if (status.is_error()) {
      return status;
    }
    auto source = fsl::SourceComponentId::parse(kProducerSource);
    if (!source.has_value()) {
      return source.status();
    }
    factory_.emplace(std::move(source).value(), facility_.epoch, facility_.generation,
                     options_.payload_bytes);
    return fsl::Status::ok();
  }

  /// Opens the epoch and registers the bounded subject set. Every control event
  /// is a durable append under the handle's own policy and is never measured.
  [[nodiscard]] fsl::Status prepare_facility() {
    fsl::Ledger& ledger = session_.ledger();
    auto state = ledger.state();
    if (!state.has_value()) {
      return state.status();
    }
    const auto ledger_subject = fsl::SubjectRef::create(fsl::SubjectKind::kLedger, "facility-state-ledger");
    if (!ledger_subject.has_value()) {
      return ledger_subject.status();
    }

    if (!state.value().open_epoch.has_value()) {
      fsl::Status status = append_control(
          fsl::EventKind::kEpochOpened, ledger_subject.value(),
          fsl::payload::encode(
              fsl::payload::EpochOpened{fsl::FacilityEpoch(1), fsl::FacilityGeneration(1)}),
          "open the facility epoch");
      if (status.is_error()) {
        return status;
      }
    }

    const std::uint64_t location_count =
        std::max<std::uint64_t>(1, static_cast<std::uint64_t>(options_.subjects) / 8ULL);
    std::vector<fsl::SubjectRef> locations;
    locations.reserve(static_cast<std::size_t>(location_count));
    for (std::uint64_t index = 0; index < location_count; ++index) {
      auto location = fsl::SubjectRef::create(fsl::SubjectKind::kLocation,
                                              "site-a.hall-1.zone-" + std::to_string(index));
      if (!location.has_value()) {
        return location.status();
      }
      fsl::SubjectRef location_ref = std::move(location).value();
      fsl::Status status = append_control(
          fsl::EventKind::kSubjectRegistered, location_ref,
          fsl::payload::encode(fsl::payload::SubjectRegistered{location_ref, std::nullopt}),
          "register a location subject");
      if (status.is_error()) {
        return status;
      }
      locations.push_back(std::move(location_ref));
    }

    facility_.racks.reserve(options_.subjects);
    for (std::size_t position = 0; position < options_.subjects; ++position) {
      const fsl::SubjectRef& location = locations[position % locations.size()];
      auto rack = fsl::SubjectRef::create(
          fsl::SubjectKind::kRack,
          location.key().str() + ".rack-" + std::to_string(static_cast<std::uint64_t>(position)));
      if (!rack.has_value()) {
        return rack.status();
      }
      fsl::SubjectRef rack_ref = std::move(rack).value();
      fsl::Status status = append_control(
          fsl::EventKind::kSubjectRegistered, rack_ref,
          fsl::payload::encode(fsl::payload::SubjectRegistered{rack_ref, location}),
          "register a rack subject");
      if (status.is_error()) {
        return status;
      }
      facility_.racks.push_back(std::move(rack_ref));
    }
    return fsl::Status::ok();
  }

  /// Appends one control event: a lifecycle record or a subject registration.
  /// Every control event is an ordinary durable append under the handle's own
  /// policy, and its identity and provenance sequence come from one counter.
  [[nodiscard]] fsl::Status append_control(fsl::EventKind kind,
                                           const fsl::SubjectRef& subject,
                                           std::vector<std::uint8_t> payload,
                                           std::string_view what) {
    fsl::SubmittedObservationFields fields;
    fields.kind = kind;
    fields.event_id = fsl::EventId::from_words(kControlIdentityHigh, ++control_index_);
    fields.facility_generation = fsl::FacilityGeneration(1);
    fields.epoch = fsl::FacilityEpoch(1);
    fields.subject = subject;
    fields.payload_schema = fsl::payload::required_schema(kind);
    fields.payload_schema_version = fsl::payload::required_schema_version(kind);
    fields.payload = std::move(payload);
    auto provenance = make_provenance(kControlSource, control_index_);
    if (!provenance.has_value()) {
      return provenance.status();
    }
    fields.provenance = std::move(provenance).value();

    auto observation = fsl::SubmittedObservation::create(std::move(fields));
    if (!observation.has_value()) {
      return observation.status();
    }
    auto outcome = session_.ledger().append(observation.value());
    if (!outcome.has_value()) {
      return outcome.status();
    }
    if (outcome.value().duplicate) {
      return fsl::conflict(fsl::ErrorCode::kDuplicateEventId,
                           "preparing the workload found an already-committed control event while "
                           "trying to " +
                               std::string(what) + "; the ledger directory was not fresh");
    }
    return fsl::Status::ok();
  }

  /// One observation append against `position`. `measure` is null for setup
  /// work, which is never reported.
  [[nodiscard]] fsl::Status append_observation(std::size_t position,
                                               std::uint64_t identity_high,
                                               AppendMeasure* measure) {
    session_.clock().advance(kMonotonicStepNanos);
    auto observation =
        factory_->build(identity_high, ++observation_index_, facility_.racks[position]);
    if (!observation.has_value()) {
      return observation.status();
    }
    const auto started = Clock::now();
    auto outcome = session_.ledger().append(observation.value());
    const auto finished = Clock::now();
    if (!outcome.has_value()) {
      return outcome.status();
    }
    if (outcome.value().duplicate) {
      return fsl::conflict(fsl::ErrorCode::kDuplicateEventId,
                           "an observation identity was already committed; the ledger directory was "
                           "not fresh");
    }
    if (measure != nullptr) {
      ++measure->operations;
      ++measure->events;
      measure->operation_micros.push_back(micros_between(started, finished));
    }
    return fsl::Status::ok();
  }

  /// Makes sure the handle the next scenario needs is the one that is open.
  [[nodiscard]] fsl::Status ensure_durable_handle() {
    if (session_.ledger().is_open() && session_.ledger().is_durable()) {
      return fsl::Status::ok();
    }
    fsl::Status status = session_.close();
    if (status.is_error()) {
      return status;
    }
    return session_.open(true);
  }

  /// Commits `--events` events when the ledger holds fewer, so that a read-only
  /// scenario is runnable on its own. The setup appends are never measured.
  [[nodiscard]] fsl::Status ensure_workload() {
    auto watermark = session_.ledger().watermark();
    if (!watermark.has_value()) {
      return watermark.status();
    }
    if (watermark.value().event_count >= options_.events) {
      return fsl::Status::ok();
    }
    const std::uint64_t before = watermark.value().event_count;
    DeterministicRandom random(kWorkloadSeed);
    while (true) {
      auto current = session_.ledger().watermark();
      if (!current.has_value()) {
        return current.status();
      }
      if (current.value().event_count >= options_.events) {
        break;
      }
      const std::size_t position = random.below(facility_.racks.size());
      fsl::Status status = append_observation(position, kSetupIdentityHigh, nullptr);
      if (status.is_error()) {
        return status;
      }
    }
    setup_appended_ = session_.ledger().watermark().value().event_count - before;
    return fsl::Status::ok();
  }

  [[nodiscard]] Context context_for(const std::string& scenario, bool durable_commits) const {
    Context context;
    context.scenario = scenario;
    context.events = options_.events;
    context.batch = options_.batch;
    context.repeat = options_.repeat;
    context.query_limit = options_.limit;
    context.subjects = options_.subjects;
    context.max_segment_bytes = options_.segment_bytes;
    context.durable_commits = durable_commits;
    context.run_durable = session_.ledger().is_durable();
    return context;
  }

  [[nodiscard]] std::string setup_note() const {
    if (setup_appended_ == 0) {
      return "setup: the ledger already held the committed prefix this scenario reads";
    }
    return "setup: committed " + std::to_string(setup_appended_) +
           " kObservationAccepted events with single durable appends before measuring; setup is not "
           "measured";
  }

  /// LedgerOptions::auto_checkpoint_interval defaults to 10'000 committed
  /// events; the append that crosses the boundary also writes a checkpoint.
  [[nodiscard]] static bool crossed_checkpoint_boundary(std::uint64_t before, std::uint64_t after) {
    constexpr std::uint64_t kInterval = 10'000;
    return before / kInterval != after / kInterval;
  }

  [[nodiscard]] static const char* auto_checkpoint_note() {
    return "this scenario crossed a multiple of LedgerOptions::auto_checkpoint_interval (10'000 "
           "committed events), so one append in the loop also wrote a checkpoint and its latency "
           "outlier is that work, not a change in steady-state commit cost";
  }

  // -- Append scenarios -----------------------------------------------------

  [[nodiscard]] fsl::Status run_append_single() {
    fsl::Status status = ensure_durable_handle();
    if (status.is_error()) {
      return status;
    }
    const CheckMark mark(checks_);
    fsl::Ledger& ledger = session_.ledger();
    auto watermark_before = ledger.watermark();
    if (!watermark_before.has_value()) {
      return watermark_before.status();
    }
    const fsl::LedgerStats stats_before = ledger.stats();

    AppendMeasure measure;
    measure.operation_micros.reserve(
        std::min<std::size_t>(kMaxLatencyReserve, static_cast<std::size_t>(options_.events)));
    double elapsed = 0.0;

    for (std::size_t round = 0; round < options_.repeat; ++round) {
      const auto round_started = Clock::now();
      DeterministicRandom random(kWorkloadSeed);
      for (std::uint64_t index = 0; index < options_.events; ++index) {
        const std::size_t position = random.below(facility_.racks.size());
        status = append_observation(position, kSingleIdentityHigh, &measure);
        if (status.is_error()) {
          return status;
        }
      }
      elapsed += seconds_between(round_started, Clock::now());
    }

    const fsl::LedgerStats stats_after = ledger.stats();
    auto watermark_after = ledger.watermark();
    if (!watermark_after.has_value()) {
      return watermark_after.status();
    }
    const std::uint64_t commit_operations = stats_after.durable_commits - stats_before.durable_commits;
    const std::uint64_t flush_calls = stats_after.flush_calls - stats_before.flush_calls;
    const std::uint64_t duplicates = stats_after.duplicate_appends - stats_before.duplicate_appends;
    const LatencyStats latency = summarize(measure.operation_micros);
    const std::uint64_t expected_events =
        options_.events * static_cast<std::uint64_t>(options_.repeat);

    Section section;
    section.context = context_for("append-single", true);
    section.notes.push_back("append latency brackets the fsl::Ledger::append call only; throughput is "
                            "wall clock over the whole measured loop, submission construction included");
    section.notes.push_back("repeat policy: aggregate over " + std::to_string(options_.repeat) +
                            " round(s); every round appended a further " +
                            std::to_string(options_.events) + " observations");
    if (crossed_checkpoint_boundary(watermark_before.value().event_count,
                                    watermark_after.value().event_count)) {
      section.notes.push_back(auto_checkpoint_note());
    }
    add_metric(section, "elapsed_seconds", fixed1(elapsed), "s");
    add_metric(section, "completed_appends", std::to_string(measure.operations), "count");
    add_metric(section, "events_per_second",
               fixed1(elapsed > 0.0 ? static_cast<double>(measure.operations) / elapsed : 0.0),
               "events/s");
    add_latency_metrics(section, "append_latency", latency);
    add_metric(section, "durable_commit_operations", std::to_string(commit_operations), "count");
    add_metric(section, "durability_flush_calls", std::to_string(flush_calls), "count");
    add_metric(section, "duplicate_appends", std::to_string(duplicates), "count");
    add_metric(section, "committed_events_before", std::to_string(watermark_before.value().event_count),
               "count");
    add_metric(section, "committed_events_after", std::to_string(watermark_after.value().event_count),
               "count");
    add_metric(section, "durability_in_measurement",
               "yes - every completed append returned only after the segment flush and the manifest "
               "publication completed",
               "");

    checks_.check(measure.operations == expected_events,
                  "append-single: every configured observation append completed");
    checks_.check(duplicates == 0, "append-single: no append was answered as a duplicate");
    checks_.check(commit_operations == measure.operations,
                  "append-single: the handle recorded exactly one durable commit per completed append");
    checks_.check(flush_calls >= commit_operations,
                  "append-single: the measured loop issued at least one durability flush per commit");
    checks_.check(watermark_after.value().event_count ==
                      watermark_before.value().event_count + measure.operations,
                  "append-single: the committed prefix grew by exactly the number of completed appends");
    checks_.check(ledger.stats().committed_events == watermark_after.value().event_count,
                  "append-single: the handle statistics agree with the commit watermark");
    add_metric(section, "integrity_checks", std::to_string(mark.checks()), "count");
    section.passed = mark.passed();
    durable_single_rate_ = elapsed > 0.0 ? static_cast<double>(measure.operations) / elapsed : 0.0;
    report_.add_section(std::move(section));
    return fsl::Status::ok();
  }

  [[nodiscard]] fsl::Status run_append_batch() {
    fsl::Status status = ensure_durable_handle();
    if (status.is_error()) {
      return status;
    }
    const CheckMark mark(checks_);
    fsl::Ledger& ledger = session_.ledger();
    auto watermark_before = ledger.watermark();
    if (!watermark_before.has_value()) {
      return watermark_before.status();
    }
    const fsl::LedgerStats stats_before = ledger.stats();

    AppendMeasure measure;
    std::vector<fsl::SubmittedObservation> submissions;
    submissions.reserve(options_.batch);
    std::uint64_t short_batches = 0;
    double elapsed = 0.0;

    for (std::size_t round = 0; round < options_.repeat; ++round) {
      const auto round_started = Clock::now();
      DeterministicRandom random(kWorkloadSeed);
      std::uint64_t remaining = options_.events;
      while (remaining > 0) {
        const std::size_t size = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(options_.batch)));

        submissions.clear();
        for (std::size_t slot = 0; slot < size; ++slot) {
          const std::size_t position = random.below(facility_.racks.size());
          session_.clock().advance(kMonotonicStepNanos);
          auto observation = factory_->build(kBatchIdentityHigh, ++observation_index_,
                                             facility_.racks[position]);
          if (!observation.has_value()) {
            return observation.status();
          }
          submissions.push_back(std::move(observation).value());
        }

        const std::span<const fsl::SubmittedObservation> view(submissions);
        const auto started = Clock::now();
        auto outcome = ledger.append_batch(view);
        const auto finished = Clock::now();
        if (!outcome.has_value()) {
          return outcome.status();
        }
        if (outcome.value().events.size() != size) {
          return fsl::internal_error(fsl::ErrorCode::kInternalError,
                                     "append_batch returned " +
                                         std::to_string(outcome.value().events.size()) +
                                         " events for a batch of " + std::to_string(size));
        }
        if (outcome.value().nothing_written) {
          return fsl::internal_error(fsl::ErrorCode::kInternalError,
                                     "append_batch reported that nothing was written for a fresh batch");
        }
        if (size != options_.batch) {
          ++short_batches;
        }
        ++measure.operations;
        measure.events += size;
        measure.operation_micros.push_back(micros_between(started, finished));
        remaining -= static_cast<std::uint64_t>(size);
      }
      elapsed += seconds_between(round_started, Clock::now());
    }

    const fsl::LedgerStats stats_after = ledger.stats();
    auto watermark_after = ledger.watermark();
    if (!watermark_after.has_value()) {
      return watermark_after.status();
    }
    const std::uint64_t commit_operations = stats_after.durable_commits - stats_before.durable_commits;
    const std::uint64_t flush_calls = stats_after.flush_calls - stats_before.flush_calls;
    const std::uint64_t duplicates = stats_after.duplicate_appends - stats_before.duplicate_appends;
    const LatencyStats latency = summarize(measure.operation_micros);
    const std::uint64_t expected_events =
        options_.events * static_cast<std::uint64_t>(options_.repeat);
    const std::uint64_t expected_short =
        options_.events % static_cast<std::uint64_t>(options_.batch) == 0ULL
            ? 0ULL
            : static_cast<std::uint64_t>(options_.repeat);

    Section section;
    section.context = context_for("append-batch", true);
    section.notes.push_back("batch latency brackets the fsl::Ledger::append_batch call only; throughput "
                            "is wall clock over the whole measured loop, submission construction included");
    section.notes.push_back("repeat policy: aggregate over " + std::to_string(options_.repeat) +
                            " round(s); every round appended a further " +
                            std::to_string(options_.events) + " observations in batches of " +
                            std::to_string(options_.batch));
    if (crossed_checkpoint_boundary(watermark_before.value().event_count,
                                    watermark_after.value().event_count)) {
      section.notes.push_back(auto_checkpoint_note());
    }
    add_metric(section, "elapsed_seconds", fixed1(elapsed), "s");
    add_metric(section, "completed_batches", std::to_string(measure.operations), "count");
    add_metric(section, "events_committed", std::to_string(measure.events), "count");
    add_metric(section, "events_per_batch",
               fixed1(measure.operations == 0
                          ? 0.0
                          : static_cast<double>(measure.events) /
                                static_cast<double>(measure.operations)),
               "events/batch");
    add_metric(section, "batches_per_second",
               fixed1(elapsed > 0.0 ? static_cast<double>(measure.operations) / elapsed : 0.0),
               "batches/s");
    add_metric(section, "events_per_second",
               fixed1(elapsed > 0.0 ? static_cast<double>(measure.events) / elapsed : 0.0), "events/s");
    add_latency_metrics(section, "batch_latency", latency);
    add_metric(section, "short_batches", std::to_string(short_batches), "count");
    add_metric(section, "durable_commit_operations", std::to_string(commit_operations), "count");
    add_metric(section, "durability_flush_calls", std::to_string(flush_calls), "count");
    add_metric(section, "duplicate_appends", std::to_string(duplicates), "count");
    add_metric(section, "committed_events_after", std::to_string(watermark_after.value().event_count),
               "count");
    add_metric(section, "durability_in_measurement",
               "yes - every completed batch returned only after the segment flush and the manifest "
               "publication completed, and the batch is atomic",
               "");

    checks_.check(measure.events == expected_events,
                  "append-batch: every configured event was committed");
    checks_.check(duplicates == 0, "append-batch: no submission was answered as a duplicate");
    checks_.check(commit_operations == measure.operations,
                  "append-batch: the handle recorded exactly one durable commit per completed batch");
    checks_.check(flush_calls >= commit_operations,
                  "append-batch: the measured loop issued at least one durability flush per commit");
    checks_.check(watermark_after.value().event_count ==
                      watermark_before.value().event_count + measure.events,
                  "append-batch: the committed prefix grew by exactly the number of committed events");
    checks_.check(ledger.stats().committed_events == watermark_after.value().event_count,
                  "append-batch: the handle statistics agree with the commit watermark");
    checks_.check(short_batches == expected_short,
                  "append-batch: exactly the final batch of each round was short");
    add_metric(section, "integrity_checks", std::to_string(mark.checks()), "count");
    section.passed = mark.passed();
    report_.add_section(std::move(section));
    return fsl::Status::ok();
  }

  [[nodiscard]] fsl::Status run_append_volatile() {
    if (session_.ledger().is_durable()) {
      fsl::Status status = session_.close();
      if (status.is_error()) {
        return status;
      }
      status = session_.open(false);
      if (status.is_error()) {
        return status;
      }
    }
    const CheckMark mark(checks_);
    fsl::Ledger& ledger = session_.ledger();
    auto watermark_before = ledger.watermark();
    if (!watermark_before.has_value()) {
      return watermark_before.status();
    }
    const fsl::LedgerStats stats_before = ledger.stats();

    AppendMeasure measure;
    measure.operation_micros.reserve(
        std::min<std::size_t>(kMaxLatencyReserve, static_cast<std::size_t>(options_.events)));
    double elapsed = 0.0;

    for (std::size_t round = 0; round < options_.repeat; ++round) {
      const auto round_started = Clock::now();
      DeterministicRandom random(kWorkloadSeed);
      for (std::uint64_t index = 0; index < options_.events; ++index) {
        const std::size_t position = random.below(facility_.racks.size());
        fsl::Status status = append_observation(position, kVolatileIdentityHigh, &measure);
        if (status.is_error()) {
          return status;
        }
      }
      elapsed += seconds_between(round_started, Clock::now());
    }

    const fsl::LedgerStats stats_after = ledger.stats();
    auto watermark_after = ledger.watermark();
    if (!watermark_after.has_value()) {
      return watermark_after.status();
    }
    const std::uint64_t commit_operations = stats_after.durable_commits - stats_before.durable_commits;
    const std::uint64_t flush_calls = stats_after.flush_calls - stats_before.flush_calls;
    const std::uint64_t duplicates = stats_after.duplicate_appends - stats_before.duplicate_appends;
    const LatencyStats latency = summarize(measure.operation_micros);
    const std::uint64_t expected_events =
        options_.events * static_cast<std::uint64_t>(options_.repeat);
    const double rate = elapsed > 0.0 ? static_cast<double>(measure.operations) / elapsed : 0.0;

    Section section;
    section.context = context_for("append-volatile", false);
    section.notes.push_back("the same ledger directory and the same single-event workload as "
                            "append-single, with LedgerOptions::durable_commits = false");
    section.notes.push_back("NOT CRASH-DURABLE: this result is not an equivalent guarantee to "
                            "append-single; the difference is the cost of the durability flushes");
    if (crossed_checkpoint_boundary(watermark_before.value().event_count,
                                    watermark_after.value().event_count)) {
      section.notes.push_back(auto_checkpoint_note());
    }
    add_metric(section, "elapsed_seconds", fixed1(elapsed), "s");
    add_metric(section, "completed_appends", std::to_string(measure.operations), "count");
    add_metric(section, "events_per_second", fixed1(rate), "events/s");
    add_latency_metrics(section, "append_latency", latency);
    add_metric(section, "durable_commit_operations", std::to_string(commit_operations), "count");
    add_metric(section, "durability_flush_calls", std::to_string(flush_calls), "count");
    add_metric(section, "duplicates_observed", std::to_string(duplicates), "count");
    add_metric(section, "committed_events_after", std::to_string(watermark_after.value().event_count),
               "count");
    add_metric(section, "durability_in_measurement",
               "no - durable_commits is false, so no flush was issued; a host crash may lose "
               "acknowledged appends even though a clean close and reopen is consistent",
               "");
    if (durable_single_rate_ > 0.0) {
      add_metric(section, "durable_baseline_events_per_second", fixed1(durable_single_rate_), "events/s");
      add_metric(section, "throughput_ratio_vs_durable", fixed1(rate / durable_single_rate_), "ratio");
    } else {
      add_metric(section, "durable_baseline_events_per_second",
                 "not measured in this run; run --scenario append-single or --scenario all", "");
    }

    checks_.check(measure.operations == expected_events,
                  "append-volatile: every configured observation append completed");
    checks_.check(duplicates == 0, "append-volatile: no append was answered as a duplicate");
    checks_.check(!ledger.is_durable(),
                  "append-volatile: the handle really was opened without durability flushes");
    checks_.check(commit_operations == measure.operations,
                  "append-volatile: the handle recorded exactly one durable commit per completed append");
    checks_.check(watermark_after.value().event_count ==
                      watermark_before.value().event_count + measure.operations,
                  "append-volatile: the committed prefix grew by exactly the number of completed appends");
    add_metric(section, "integrity_checks", std::to_string(mark.checks()), "count");
    section.passed = mark.passed();
    report_.add_section(std::move(section));
    return fsl::Status::ok();
  }

  // -- Replay ---------------------------------------------------------------

  [[nodiscard]] fsl::Status run_replay() {
    fsl::Status status = ensure_durable_handle();
    if (status.is_error()) {
      return status;
    }
    status = ensure_workload();
    if (status.is_error()) {
      return status;
    }
    const CheckMark mark(checks_);
    fsl::Ledger& ledger = session_.ledger();
    auto watermark = ledger.watermark();
    if (!watermark.has_value()) {
      return watermark.status();
    }
    const std::uint64_t committed = watermark.value().event_count;

    double best_seconds = std::numeric_limits<double>::infinity();
    std::uint64_t best_events = 0;
    std::uint64_t best_bytes = 0;
    std::vector<double> best_micros;
    std::uint64_t sequence_errors = 0;
    std::uint64_t delivered = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t failed_after = 0;
    fsl::Status failure = fsl::Status::ok();

    for (std::size_t round = 0; round < options_.repeat; ++round) {
      std::vector<double> micros;
      std::uint64_t round_events = 0;
      std::uint64_t round_bytes = 0;
      std::uint64_t round_expected = 1;
      const fsl::ReplayRequest request;
      const auto started = Clock::now();
      Clock::time_point last = started;
      const auto visitor = [&](const fsl::EventEnvelope& event) {
        const auto now = Clock::now();
        micros.push_back(micros_between(last, now));
        last = now;
        if (event.sequence().value() != round_expected) {
          ++sequence_errors;
        }
        round_expected = event.sequence().value() + 1;
        round_bytes += event.payload().size();
        ++round_events;
        return true;
      };
      auto replayed = ledger.replay_each(request, visitor);
      const auto finished = Clock::now();
      const double seconds = seconds_between(started, finished);
      if (!replayed.has_value()) {
        failure = replayed.status();
        failed_after = round_events;
        if (seconds < best_seconds) {
          best_seconds = seconds;
          best_events = round_events;
          best_bytes = round_bytes;
          best_micros = std::move(micros);
        }
        break;
      }
      if (replayed.value() != round_events) {
        return fsl::internal_error(fsl::ErrorCode::kInternalError,
                                   "replay_each reported " + std::to_string(replayed.value()) +
                                       " delivered events but the visitor saw " +
                                       std::to_string(round_events));
      }
      if (seconds < best_seconds) {
        best_seconds = seconds;
        best_events = round_events;
        best_bytes = round_bytes;
        best_micros = std::move(micros);
      }
      delivered = round_events;
      payload_bytes = round_bytes;
    }

    const bool complete = failure.is_ok();
    const LatencyStats latency = summarize(std::move(best_micros));
    const double mib = static_cast<double>(best_bytes) / (1024.0 * 1024.0);

    Section section;
    section.context = context_for("replay", true);
    section.notes.push_back(setup_note());
    section.notes.push_back("repeat policy: best of " + std::to_string(options_.repeat) +
                            " rounds; the latency statistics are those of the best round");
    section.notes.push_back("per-event timestamps are taken with std::chrono::steady_clock inside the "
                            "replay visitor and are part of the wall clock the rate comes from");
    if (!complete) {
      section.notes.push_back("PARTIAL: Ledger::replay_each stopped early; the rates below cover only "
                              "the prefix it delivered before reporting the failure");
    }
    add_metric(section, "committed_events", std::to_string(committed), "count");
    add_metric(section, "events_delivered",
               std::to_string(complete ? best_events : failed_after), "count");
    add_metric(section, "elapsed_seconds", fixed1(best_seconds), "s");
    add_metric(section, "events_per_second",
               fixed1(best_seconds > 0.0 ? static_cast<double>(best_events) / best_seconds : 0.0),
               "events/s");
    add_metric(section, "decoded_payload_mib", fixed1(mib), "MiB");
    add_metric(section, "payload_mib_per_second",
               fixed1(best_seconds > 0.0 ? mib / best_seconds : 0.0), "MiB/s");
    add_latency_metrics(section, "replay_step_latency", latency);
    add_metric(section, "sequence_gaps_observed", std::to_string(sequence_errors), "count");
    add_metric(section, "replay_result", complete ? "ok" : failure.to_string(), "");
    add_metric(section, "durability_in_measurement",
               "not applicable - replay reads committed state and performs no flush", "");

    checks_.check(complete, "replay: Ledger::replay_each completed the committed prefix");
    checks_.check(sequence_errors == 0,
                  "replay: every delivered event carried the next expected ledger sequence");
    checks_.check(delivered == committed, "replay: the delivered count equals the committed event count");
    checks_.check(payload_bytes > 0, "replay: decoded payload bytes were observed");
    add_metric(section, "integrity_checks", std::to_string(mark.checks()), "count");
    section.passed = mark.passed();
    report_.add_section(std::move(section));
    return failure;
  }

  // -- Verify ---------------------------------------------------------------

  [[nodiscard]] fsl::Status run_verify() {
    fsl::Status status = ensure_durable_handle();
    if (status.is_error()) {
      return status;
    }
    status = ensure_workload();
    if (status.is_error()) {
      return status;
    }
    const CheckMark mark(checks_);
    fsl::Ledger& ledger = session_.ledger();
    auto watermark = ledger.watermark();
    if (!watermark.has_value()) {
      return watermark.status();
    }
    const std::uint64_t committed = watermark.value().event_count;

    fsl::VerifyRequest request;
    request.scope = fsl::VerifyScope::kFull;

    std::vector<double> micros;
    std::uint64_t best_records = 0;
    std::uint64_t best_bytes = 0;
    std::uint64_t segments = 0;
    std::uint64_t findings = 0;
    double best_seconds = std::numeric_limits<double>::infinity();
    bool verified_ok = true;
    bool index_verified = false;
    bool index_consistent = false;
    std::string status_text = "ok";

    for (std::size_t round = 0; round < options_.repeat; ++round) {
      const auto started = Clock::now();
      const fsl::VerifyReport verified = ledger.verify(request);
      const auto finished = Clock::now();
      const double seconds = seconds_between(started, finished);
      micros.push_back(micros_between(started, finished));
      if (!verified.ok()) {
        verified_ok = false;
        status_text = verified.status.to_string();
      }
      index_verified = verified.index_verified;
      index_consistent = verified.index_consistent;
      findings = verified.findings.size();
      segments = verified.segments_verified;
      if (seconds < best_seconds) {
        best_seconds = seconds;
        best_records = verified.records_verified;
        best_bytes = verified.bytes_verified;
      }
    }

    const LatencyStats latency = summarize(std::move(micros));
    const double mib = static_cast<double>(best_bytes) / (1024.0 * 1024.0);

    Section section;
    section.context = context_for("verify", true);
    section.notes.push_back(setup_note());
    section.notes.push_back("VerifyScope::kFull over the whole committed prefix; the walk happens "
                            "inside one library call, so the latency statistics are per verify call");
    section.notes.push_back("repeat policy: best of " + std::to_string(options_.repeat) +
                            " rounds for the rates; latency over every round's call");
    add_metric(section, "committed_events", std::to_string(committed), "count");
    add_metric(section, "records_verified", std::to_string(best_records), "count");
    add_metric(section, "bytes_verified", std::to_string(best_bytes), "bytes");
    add_metric(section, "segments_verified", std::to_string(segments), "count");
    add_metric(section, "elapsed_seconds", fixed1(best_seconds), "s");
    add_metric(section, "records_per_second",
               fixed1(best_seconds > 0.0 ? static_cast<double>(best_records) / best_seconds : 0.0),
               "records/s");
    add_metric(section, "verified_mib_per_second",
               fixed1(best_seconds > 0.0 ? mib / best_seconds : 0.0), "MiB/s");
    add_metric(section, "mean_microseconds_per_record",
               fixed1(best_records == 0 ? 0.0
                                        : (best_seconds * 1'000'000.0) / static_cast<double>(best_records)),
               "us");
    add_latency_metrics(section, "verify_latency", latency);
    add_metric(section, "verify_result", status_text, "");
    add_metric(section, "findings", std::to_string(findings), "count");
    add_metric(section, "index_verified", index_verified ? "yes" : "no", "");
    add_metric(section, "index_consistent", index_consistent ? "yes" : "no", "");
    add_metric(section, "durability_in_measurement",
               "not applicable - verify reads committed state and performs no flush", "");

    checks_.check(verified_ok, "verify: VerifyScope::kFull reported no integrity failure");
    checks_.check(findings == 0, "verify: the report carried no findings");
    checks_.check(best_records == committed, "verify: every committed record was verified");
    checks_.check(index_verified, "verify: the derived index was verified");
    checks_.check(index_consistent, "verify: the derived index agreed with the authoritative log");
    add_metric(section, "integrity_checks", std::to_string(mark.checks()), "count");
    section.passed = mark.passed();
    report_.add_section(std::move(section));
    return fsl::Status::ok();
  }

  // -- Query ----------------------------------------------------------------

  [[nodiscard]] fsl::Status run_query() {
    fsl::Status status = ensure_durable_handle();
    if (status.is_error()) {
      return status;
    }
    status = ensure_workload();
    if (status.is_error()) {
      return status;
    }
    const CheckMark mark(checks_);
    fsl::Ledger& ledger = session_.ledger();
    auto watermark = ledger.watermark();
    if (!watermark.has_value()) {
      return watermark.status();
    }
    const std::uint64_t committed = watermark.value().event_count;
    if (committed == 0) {
      return fsl::not_found(fsl::ErrorCode::kNotFound, "the ledger holds no committed event to query");
    }
    const std::size_t operations =
        static_cast<std::size_t>(std::min<std::uint64_t>(committed, kMaxQueryOperations));

    // Sample the workload before measuring, so that resolving sequences,
    // identities and subjects is not part of any measured duration.
    DeterministicRandom random(kWorkloadSeed);
    std::vector<fsl::LedgerSequence> sequences;
    std::vector<fsl::EventId> identities;
    std::vector<fsl::SubjectRef> subjects;
    sequences.reserve(operations);
    identities.reserve(operations);
    subjects.reserve(operations);
    for (std::size_t index = 0; index < operations; ++index) {
      const std::uint64_t sequence = 1ULL + random.below(static_cast<std::size_t>(committed));
      auto event = ledger.read(fsl::LedgerSequence(sequence));
      if (!event.has_value()) {
        return event.status();
      }
      sequences.push_back(fsl::LedgerSequence(sequence));
      identities.push_back(event.value().event_id());
      subjects.push_back(facility_.racks[random.below(facility_.racks.size())]);
    }

    std::vector<double> read_micros;
    std::vector<double> find_micros;
    std::vector<double> query_micros;
    read_micros.reserve(operations);
    find_micros.reserve(operations);
    query_micros.reserve(operations);
    std::uint64_t read_mismatches = 0;
    std::uint64_t find_mismatches = 0;
    std::uint64_t limit_violations = 0;
    std::uint64_t subject_mismatches = 0;
    std::uint64_t non_empty_pages = 0;
    std::uint64_t indexed_pages = 0;
    std::uint64_t linear_scan_pages = 0;

    std::uint64_t reads = 0;
    for (std::size_t index = 0; index < operations; ++index) {
      const auto started = Clock::now();
      auto event = ledger.read(sequences[index]);
      const auto finished = Clock::now();
      if (!event.has_value()) {
        return event.status();
      }
      read_micros.push_back(micros_between(started, finished));
      ++reads;
      if (event.value().sequence().value() != sequences[index].value()) {
        ++read_mismatches;
      }
    }

    std::uint64_t finds = 0;
    for (std::size_t index = 0; index < operations; ++index) {
      const auto started = Clock::now();
      auto event = ledger.find_event(identities[index]);
      const auto finished = Clock::now();
      if (!event.has_value()) {
        return event.status();
      }
      find_micros.push_back(micros_between(started, finished));
      ++finds;
      if (!(event.value().event_id() == identities[index])) {
        ++find_mismatches;
      }
    }

    std::uint64_t queries = 0;
    for (std::size_t index = 0; index < operations; ++index) {
      fsl::Query request;
      request.subject = subjects[index];
      request.limit = options_.limit;
      const auto started = Clock::now();
      auto page = ledger.query(request);
      const auto finished = Clock::now();
      if (!page.has_value()) {
        return page.status();
      }
      query_micros.push_back(micros_between(started, finished));
      ++queries;
      if (page.value().events.size() > options_.limit) {
        ++limit_violations;
      }
      if (page.value().source == fsl::QuerySource::kIndexed) {
        ++indexed_pages;
      } else {
        ++linear_scan_pages;
      }
      if (!page.value().events.empty()) {
        ++non_empty_pages;
      }
      for (const fsl::EventEnvelope& event : page.value().events) {
        if (!(event.subject() == subjects[index])) {
          ++subject_mismatches;
        }
      }
    }

    const LatencyStats read_stats = summarize(std::move(read_micros));
    const LatencyStats find_stats = summarize(std::move(find_micros));
    const LatencyStats query_stats = summarize(std::move(query_micros));
    std::string observed_source = "not observed";
    if (indexed_pages > 0) {
      observed_source = std::string(fsl::to_string(fsl::QuerySource::kIndexed));
    } else if (linear_scan_pages > 0) {
      observed_source = std::string(fsl::to_string(fsl::QuerySource::kLinearScan));
    }

    Section section;
    section.context = context_for("query", true);
    section.notes.push_back(setup_note());
    section.notes.push_back("a single pass of " + std::to_string(operations) +
                            " operations per group; QuerySource is reported as observed, not assumed");
    add_metric(section, "committed_events", std::to_string(committed), "count");
    add_metric(section, "operations_per_group", std::to_string(operations), "count");
    add_metric(section, "read_by_sequence_completed", std::to_string(reads), "count");
    add_metric(section, "read_by_sequence_per_second",
               fixed1(read_stats.mean_us > 0.0 ? 1'000'000.0 / read_stats.mean_us : 0.0), "ops/s");
    add_latency_metrics(section, "read_by_sequence_latency", read_stats);
    add_metric(section, "find_by_event_id_completed", std::to_string(finds), "count");
    add_metric(section, "find_by_event_id_per_second",
               fixed1(find_stats.mean_us > 0.0 ? 1'000'000.0 / find_stats.mean_us : 0.0), "ops/s");
    add_latency_metrics(section, "find_by_event_id_latency", find_stats);
    add_metric(section, "query_by_subject_completed", std::to_string(queries), "count");
    add_metric(section, "query_by_subject_per_second",
               fixed1(query_stats.mean_us > 0.0 ? 1'000'000.0 / query_stats.mean_us : 0.0), "ops/s");
    add_latency_metrics(section, "query_by_subject_latency", query_stats);
    add_metric(section, "query_source_observed", observed_source, "");
    add_metric(section, "pages_served_from_index", std::to_string(indexed_pages), "count");
    add_metric(section, "pages_served_from_linear_scan", std::to_string(linear_scan_pages), "count");
    add_metric(section, "non_empty_pages", std::to_string(non_empty_pages), "count");
    add_metric(section, "query_limit_violations", std::to_string(limit_violations), "count");
    add_metric(section, "durability_in_measurement",
               "not applicable - queries read committed state and perform no flush", "");

    checks_.check(reads == operations, "query: every point read completed");
    checks_.check(finds == operations, "query: every event-id lookup completed");
    checks_.check(queries == operations, "query: every bounded subject query completed");
    checks_.check(read_mismatches == 0, "query: every point read returned its requested sequence");
    checks_.check(find_mismatches == 0, "query: every event-id lookup returned its requested identity");
    checks_.check(limit_violations == 0, "query: no page exceeded --limit");
    checks_.check(subject_mismatches == 0, "query: every returned event matched the queried subject");
    checks_.check(non_empty_pages > 0, "query: at least one bounded subject query returned events");
    add_metric(section, "integrity_checks", std::to_string(mark.checks()), "count");
    section.passed = mark.passed();
    report_.add_section(std::move(section));
    return fsl::Status::ok();
  }

  // -- Reopen ---------------------------------------------------------------

  [[nodiscard]] fsl::Status run_reopen() {
    fsl::Status status = ensure_durable_handle();
    if (status.is_error()) {
      return status;
    }
    status = ensure_workload();
    if (status.is_error()) {
      return status;
    }
    const CheckMark mark(checks_);
    auto watermark = session_.ledger().watermark();
    if (!watermark.has_value()) {
      return watermark.status();
    }
    const std::uint64_t committed = watermark.value().event_count;

    std::vector<double> open_micros;
    std::vector<double> close_micros;
    std::uint64_t replayed = 0;
    std::uint64_t events_after = 0;
    std::uint64_t truncated_bytes = 0;
    double best_open_seconds = std::numeric_limits<double>::infinity();
    double best_close_seconds = 0.0;
    bool clean_reopen = true;
    bool index_rebuilt = false;

    for (std::size_t round = 0; round < options_.repeat; ++round) {
      const auto close_started = Clock::now();
      fsl::Status closed = session_.close();
      const auto close_finished = Clock::now();
      if (closed.is_error()) {
        return closed;
      }

      const auto open_started = Clock::now();
      fsl::Status opened = session_.open(true);
      const auto open_finished = Clock::now();
      if (opened.is_error()) {
        return opened;
      }

      const double close_seconds = seconds_between(close_started, close_finished);
      const double open_seconds = seconds_between(open_started, open_finished);
      open_micros.push_back(micros_between(open_started, open_finished));
      close_micros.push_back(micros_between(close_started, close_finished));

      const fsl::OpenReport& opened_report = session_.ledger().open_report();
      index_rebuilt = index_rebuilt || opened_report.index_rebuilt;
      truncated_bytes += opened_report.truncated_tail_bytes;
      if (opened_report.recovered || opened_report.manifest_recovered_from_backup ||
          opened_report.truncated_tail_bytes != 0) {
        clean_reopen = false;
      }
      auto reopened_watermark = session_.ledger().watermark();
      if (!reopened_watermark.has_value()) {
        return reopened_watermark.status();
      }
      if (reopened_watermark.value().event_count != committed) {
        clean_reopen = false;
      }
      if (!session_.ledger().is_open() || !session_.ledger().is_writable()) {
        clean_reopen = false;
      }
      if (open_seconds < best_open_seconds) {
        best_open_seconds = open_seconds;
        best_close_seconds = close_seconds;
        replayed = opened_report.replayed_events;
        events_after = reopened_watermark.value().event_count;
      }
    }

    const LatencyStats open_stats = summarize(std::move(open_micros));
    const LatencyStats close_stats = summarize(std::move(close_micros));

    Section section;
    section.context = context_for("reopen", true);
    section.notes.push_back(setup_note());
    section.notes.push_back("close and Ledger::open are timed separately; the reported open duration "
                            "excludes the close");
    section.notes.push_back("repeat policy: best of " + std::to_string(options_.repeat) +
                            " rounds for the durations; latency over every round's call");
    section.notes.push_back("a clean close persists the derived index, so replayed_events reports only "
                            "the part of the prefix the persisted index did not already cover");
    add_metric(section, "committed_events", std::to_string(committed), "count");
    add_metric(section, "open_seconds", fixed1(best_open_seconds), "s");
    add_metric(section, "close_seconds", fixed1(best_close_seconds), "s");
    add_metric(section, "events_replayed_to_reconstruct_state", std::to_string(replayed), "count");
    add_metric(section, "events_after_reopen", std::to_string(events_after), "count");
    add_metric(section, "replayed_events_per_second",
               fixed1(best_open_seconds > 0.0 && replayed > 0
                          ? static_cast<double>(replayed) / best_open_seconds
                          : 0.0),
               "events/s");
    add_latency_metrics(section, "open_latency", open_stats);
    add_latency_metrics(section, "close_latency", close_stats);
    add_metric(section, "index_rebuilt_at_open", index_rebuilt ? "yes" : "no", "");
    add_metric(section, "truncated_tail_bytes", std::to_string(truncated_bytes), "bytes");
    add_metric(section, "durability_in_measurement",
               "yes for the close that precedes the open: a durable close flushes pending state before "
               "the writer lock is released",
               "");

    checks_.check(clean_reopen,
                  "reopen: the ledger reopened with the same watermark, no recovery and no truncation");
    checks_.check(events_after == committed, "reopen: every committed event survived the reopen");
    checks_.check(truncated_bytes == 0, "reopen: no unacknowledged tail was discarded");
    add_metric(section, "integrity_checks", std::to_string(mark.checks()), "count");
    section.passed = mark.passed();
    report_.add_section(std::move(section));
    return fsl::Status::ok();
  }

  // -- State ----------------------------------------------------------------

  const Options& options_;
  Session& session_;
  Report& report_;
  Checker checks_;
  Facility facility_;
  std::optional<ObservationFactory> factory_;
  std::uint64_t control_index_ = 0;
  std::uint64_t observation_index_ = 0;
  std::uint64_t setup_appended_ = 0;
  double durable_single_rate_ = 0.0;
};

// ---------------------------------------------------------------------------
// Report scaffolding
// ---------------------------------------------------------------------------

void build_environment(Report& report) {
  std::vector<Metric> metrics;
  metrics.push_back(Metric{"build_type", build_description(), ""});
  metrics.push_back(Metric{"platform", platform_description(), ""});
  metrics.push_back(Metric{"compiler", compiler_description(), ""});
  metrics.push_back(Metric{"library_version", std::string(fsl::library_version_string()), ""});
  metrics.push_back(Metric{"timing_clock", "std::chrono::steady_clock", ""});
  metrics.push_back(Metric{"percentile_method", "nearest-rank over sorted samples", ""});
  metrics.push_back(Metric{"result_label", "SYNTHETIC (this build, this machine)", ""});
  report.set_build_summary(ndebug_flag() + "  " + platform_flag() + "  " + compiler_description());
  report.set_environment(std::move(metrics));
}

[[nodiscard]] std::string durability_mode(const std::string& scenario) {
  if (scenario == "append-volatile") {
    return "volatile: LedgerOptions::durable_commits = false";
  }
  if (scenario == "all") {
    return "durable, except append-volatile which reopens the same directory with durable_commits = false";
  }
  return "durable: LedgerOptions::durable_commits = true";
}

void build_run_configuration(Report& report, const Options& options) {
  std::vector<Metric> metrics;
  metrics.push_back(Metric{"scenario", options.scenario, ""});
  metrics.push_back(Metric{"events", std::to_string(options.events), "count"});
  metrics.push_back(Metric{"batch_size", std::to_string(options.batch), "events/batch"});
  metrics.push_back(Metric{"repeat", std::to_string(options.repeat), "rounds"});
  metrics.push_back(Metric{"query_limit", std::to_string(options.limit), "events/page"});
  metrics.push_back(Metric{"subjects", std::to_string(options.subjects), "rack subjects"});
  metrics.push_back(Metric{"payload_bytes", std::to_string(options.payload_bytes), "bytes"});
  metrics.push_back(Metric{"max_segment_bytes", std::to_string(options.segment_bytes), "bytes"});
  metrics.push_back(Metric{"durability_mode", durability_mode(options.scenario), ""});
  metrics.push_back(Metric{"ledger_directory", narrow(options.directory), ""});
  metrics.push_back(Metric{"segment_timestamp_unix_nanos", std::to_string(kPinnedUnixNanos), ""});
  metrics.push_back(Metric{"recorded_monotonic_step",
                           std::to_string(kMonotonicStepNanos) + " ns/event", ""});
  metrics.push_back(Metric{"random_seed", std::to_string(kWorkloadSeed), ""});
  metrics.push_back(Metric{"workload",
                           "one open epoch, a bounded pool of location/rack subjects registered up "
                           "front, then kObservationAccepted events against that pool, each with "
                           "payload::encode(ObservationAccepted) and a ProvenanceRecord carrying a "
                           "source generation and a per-source sequence",
                           ""});
  report.set_run_configuration(std::move(metrics));
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  const CommandLine command = parse_command_line(argc, argv);
  if (command.kind == CommandLine::Kind::kHelp) {
    print_usage(stdout);
    return 0;
  }
  if (command.kind == CommandLine::Kind::kError) {
    std::fprintf(stderr, "fsl_bench: %s\n\n", command.message.c_str());
    print_usage(stderr);
    return 2;
  }

  const Options& options = command.options;
  DirectoryGuard guard(options.directory);
  Report report;
  report.set_ledger_directory(narrow(options.directory));
  build_environment(report);
  build_run_configuration(report, options);

  Context run_context;
  run_context.scenario = options.scenario;
  run_context.events = options.events;
  run_context.batch = options.batch;
  run_context.repeat = options.repeat;
  run_context.query_limit = options.limit;
  run_context.subjects = options.subjects;
  run_context.max_segment_bytes = options.segment_bytes;
  run_context.durable_commits = options.scenario != "append-volatile";
  run_context.run_durable = options.scenario != "append-volatile";
  report.set_run_context(run_context);

  int exit_code = 0;
  std::string failure_report;
  std::uint64_t checks_passed = 0;
  std::uint64_t checks_failed = 0;

  {
    // A stale directory would turn this deterministic workload into duplicate
    // submissions, so the benchmark always starts from an empty tree.
    guard.remove_now();

    Session session(options.directory, options);
    Benchmark benchmark(options, session, report);
    const fsl::Status status = benchmark.run();
    if (status.is_error()) {
      std::fprintf(stderr, "fsl_bench: scenario %s failed: %s\n", options.scenario.c_str(),
                   status.to_string().c_str());
      failure_report = status.to_string();
      exit_code = 1;
    }
    for (const std::string& failure : benchmark.checks().failures()) {
      std::fprintf(stderr, "fsl_bench: integrity check failed: %s\n", failure.c_str());
      exit_code = 1;
    }
    checks_passed = benchmark.checks().checks() - benchmark.checks().failure_count();
    checks_failed = benchmark.checks().failure_count();

    Section summary;
    summary.context = run_context;
    summary.context.scenario = "summary";
    summary.passed = exit_code == 0;
    add_metric(summary, "scenarios_completed", std::to_string(report.scenario_count()), "count");
    add_metric(summary, "scenarios_failed", std::to_string(report.failed_scenario_count()), "count");
    add_metric(summary, "checks_passed", std::to_string(checks_passed), "count");
    add_metric(summary, "checks_failed", std::to_string(checks_failed), "count");
    add_metric(summary, "scenario_failure", failure_report.empty() ? "none" : failure_report, "");
    add_metric(summary, "exit_code", std::to_string(exit_code), "");
    report.add_section(std::move(summary));

    const fsl::Status closed = session.close();
    if (closed.is_error()) {
      std::fprintf(stderr, "fsl_bench: closing the ledger failed: %s\n", closed.to_string().c_str());
      exit_code = 1;
    }
  }

  guard.remove_now();
  const bool residue = guard.exists();
  if (residue) {
    std::fprintf(stderr, "fsl_bench: the ledger directory %s was not removed\n",
                 narrow(options.directory).c_str());
    exit_code = 1;
  }

  report.print();

  std::printf("[cleanup]\n");
  std::printf("  ledger_directory_removed : %s\n",
              residue ? "FAILED - residue left behind" : "yes - no residue left behind");
  if (options.csv.has_value()) {
    std::string error;
    if (!report.write_csv(*options.csv, error)) {
      std::fprintf(stderr, "fsl_bench: CSV output failed: %s\n", error.c_str());
      return 1;
    }
    std::printf("  csv_output               : %s\n", narrow(*options.csv).c_str());
  } else {
    std::printf("  csv_output               : not requested\n");
  }
  std::printf("  exit_code                : %d\n", exit_code);

  return exit_code;
}

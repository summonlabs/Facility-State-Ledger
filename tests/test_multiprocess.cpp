// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "fsl/ledger.hpp"
#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_multiprocess.cpp
/// Real operating-system processes: writer fencing across processes, and the
/// committed-prefix semantics observed after a writer is killed at each
/// durability boundary.
///
/// The crash writer is an independent executable. Killing it proves that the
/// commit watermark, not the mere presence of bytes, decides what is committed.

namespace {

#ifndef FSL_CRASH_WRITER_PATH
#error "FSL_CRASH_WRITER_PATH must be defined by the build system"
#endif

[[nodiscard]] int run_command(const std::string& command) { return std::system(command.c_str()); }

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
  std::string text;
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return text;
  }
  char buffer[512];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, got);
  }
  std::fclose(file);
  return text;
}

[[nodiscard]] std::string quoted(const std::filesystem::path& path) {
  return std::string("\"") + path.string() + "\"";
}

struct CrashRun {
  int exit_code = 0;
  std::string output;
  std::string error;
};

[[nodiscard]] CrashRun run_crash_writer(const std::filesystem::path& work,
                                        const std::filesystem::path& directory,
                                        const std::string& boundary,
                                        int events,
                                        int batch,
                                        int identity_base = 100) {
  const std::filesystem::path out = work / ("crash-" + boundary + ".txt");
  const std::filesystem::path err = work / ("crash-" + boundary + ".err");
  std::string inner = quoted(FSL_CRASH_WRITER_PATH);
  inner += " --dir " + quoted(directory);
  inner += " --events " + std::to_string(events);
  inner += " --batch " + std::to_string(batch);
  inner += " --boundary " + boundary;
  inner += " --identity-base " + std::to_string(identity_base);
  inner += " > " + quoted(out) + " 2> " + quoted(err);
  // cmd.exe removes the first and last quote of a command that begins with one,
  // so the whole line is wrapped in an extra pair. These paths contain spaces.
  const std::string command = "\"" + inner + "\"";

  CrashRun run;
  run.exit_code = run_command(command);
  run.output = read_text(out);
  run.error = read_text(err);
  return run;
}

void report_run(const CrashRun& run) {
  fsl_test::Context::instance().note("crash writer exit=" + std::to_string(run.exit_code) +
                                     " stdout='" + run.output + "' stderr='" + run.error + "'");
}

/// Every committed event must be readable, contiguous and chain-verified.
/// `committed` receives the commit watermark after the check.
void check_ledger_health(const std::filesystem::path& directory, std::uint64_t& committed) {
  auto opened =
      fsl::Ledger::open(directory, fsl::OpenMode::kReadWrite, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(opened);
  fsl::Ledger ledger = std::move(opened).value();
  const auto watermark = ledger.watermark();
  FSL_REQUIRE_OK(watermark);
  committed = watermark->sequence.has_value() ? watermark->sequence->value() : 0;
  for (std::uint64_t sequence = 1; sequence <= committed; ++sequence) {
    auto event = ledger.read(fsl::LedgerSequence(sequence));
    FSL_REQUIRE_OK(event);
    FSL_CHECK_EQ(event->sequence().value(), sequence);
  }
  const fsl::VerifyReport report = ledger.verify();
  FSL_CHECK(report.ok());
  FSL_CHECK_EQ(report.records_verified, committed);
  FSL_REQUIRE_OK(ledger.close());
}

/// Prepares a ledger with the crash writer itself, so that the writer's own
/// setup appends are already committed and cannot be mistaken for the work
/// under test. Returns the commit watermark afterwards.
[[nodiscard]] std::uint64_t prepare_ledger(const std::filesystem::path& directory) {
  const CrashRun prepared = run_crash_writer(directory, directory, "none", 1, 1, 1);
  if (prepared.exit_code != 0) {
    report_run(prepared);
  }
  FSL_CHECK_EQ(prepared.exit_code, 0);
  FSL_CHECK(prepared.output.find("committed 1") != std::string::npos);
  std::uint64_t committed = 0;
  check_ledger_health(directory, committed);
  return committed;
}

FSL_TEST(a_second_process_cannot_take_the_writer_role) {
  fsl_test::TempDirectory temp("mp_lock");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger holder = std::move(created).value();
  auto seed = fsl_test::seed_facility(holder);
  FSL_REQUIRE_OK(seed);

  // With this process holding the writer lock, a second process must be refused
  // by the operating system rather than allowed to write.
  const CrashRun blocked = run_crash_writer(temp.path(), temp.path(), "none", 1, 1);
  if (blocked.exit_code == 0) {
    report_run(blocked);
  }
  FSL_CHECK(blocked.exit_code != 0);
  FSL_CHECK(blocked.output.find("committed") == std::string::npos);
  FSL_CHECK(blocked.error.find("locked/ledger-locked") != std::string::npos);

  FSL_REQUIRE_OK(holder.close());

  // Once the writer releases the role, another process may take it.
  const CrashRun allowed = run_crash_writer(temp.path(), temp.path(), "none", 1, 1);
  if (allowed.exit_code != 0) {
    report_run(allowed);
  }
  FSL_CHECK_EQ(allowed.exit_code, 0);
  FSL_CHECK(allowed.output.find("committed 1") != std::string::npos);
}

FSL_TEST(a_killed_writer_never_publishes_an_unacknowledged_commit) {
  const char* boundaries[] = {"after-encode", "after-segment-write", "after-segment-flush",
                              "before-manifest-publish"};
  constexpr int kEvents = 4;

  for (const char* boundary : boundaries) {
    fsl_test::TempDirectory temp(std::string("mp_crash_") + boundary);
    const std::uint64_t before = prepare_ledger(temp.path());

    const CrashRun crashed = run_crash_writer(temp.path(), temp.path(), boundary, kEvents, 1);
    if (crashed.exit_code != 97) {
      report_run(crashed);
    }
    // The process must have been terminated at the boundary rather than have
    // returned normally: a boundary crash is not a successful run.
    FSL_CHECK_EQ(crashed.exit_code, 97);
    FSL_CHECK(crashed.output.find("committed") == std::string::npos);

    std::uint64_t after = 0;
    check_ledger_health(temp.path(), after);
    // The writer arms its injector only for the final append, so every earlier
    // event is committed and the interrupted one is not. Nothing in between is
    // ever observable, and the reclaimed tail is reusable.
    FSL_CHECK_EQ(after, before + (kEvents - 1));
  }
}

FSL_TEST(a_writer_killed_after_the_commit_acknowledgement_leaves_the_commit) {
  fsl_test::TempDirectory temp("mp_crash_after_ack");
  const std::uint64_t before = prepare_ledger(temp.path());

  const CrashRun crashed = run_crash_writer(temp.path(), temp.path(), "after-commit-ack", 3, 1);
  if (crashed.exit_code != 97) {
    report_run(crashed);
  }
  FSL_CHECK_EQ(crashed.exit_code, 97);
  FSL_CHECK(crashed.output.find("committed") == std::string::npos);

  std::uint64_t after = 0;
  check_ledger_health(temp.path(), after);
  // The final commit crossed the acknowledgement boundary before the process
  // died, so it is durable even though no caller ever saw the result.
  FSL_CHECK_EQ(after, before + 3);

  // Retrying the same work after the restart is idempotent: the identities the
  // killed writer already committed are recognised, so nothing is duplicated.
  const CrashRun retry = run_crash_writer(temp.path(), temp.path(), "none", 3, 1);
  if (retry.exit_code != 0) {
    report_run(retry);
  }
  FSL_CHECK_EQ(retry.exit_code, 0);
  std::uint64_t final_count = 0;
  check_ledger_health(temp.path(), final_count);
  FSL_CHECK_EQ(final_count, after);
}

FSL_TEST(a_writer_killed_before_the_manifest_publication_leaves_no_phantom_commit) {
  fsl_test::TempDirectory temp("mp_crash_phantom");
  const std::uint64_t before = prepare_ledger(temp.path());

  // One atomic batch that dies before publication must not be observable at all.
  const CrashRun crashed = run_crash_writer(temp.path(), temp.path(), "before-manifest-publish", 8, 8);
  if (crashed.exit_code != 97) {
    report_run(crashed);
  }
  FSL_CHECK_EQ(crashed.exit_code, 97);
  FSL_CHECK(crashed.output.find("committed") == std::string::npos);

  std::uint64_t after = 0;
  check_ledger_health(temp.path(), after);
  FSL_CHECK_EQ(after, before);

  // The reclaimed tail is reusable and the sequence continues from the
  // committed watermark.
  const CrashRun resumed = run_crash_writer(temp.path(), temp.path(), "none", 1, 1, 900);
  if (resumed.exit_code != 0) {
    report_run(resumed);
  }
  FSL_CHECK_EQ(resumed.exit_code, 0);
  FSL_CHECK(resumed.output.find("committed 1") != std::string::npos);

  std::uint64_t final_count = 0;
  check_ledger_health(temp.path(), final_count);
  FSL_CHECK_EQ(final_count, before + 1);
}

FSL_TEST(a_reader_process_sees_only_the_committed_prefix) {
  fsl_test::TempDirectory temp("mp_reader");
  auto created = fsl::Ledger::create(temp.path(), fsl_test::deterministic_options());
  FSL_REQUIRE_OK(created);
  fsl::Ledger writer = std::move(created).value();
  auto seed = fsl_test::seed_facility(writer);
  FSL_REQUIRE_OK(seed);
  for (std::uint64_t i = 0; i < 10; ++i) {
    FSL_REQUIRE_OK(fsl_test::record_observation(writer, seed->asset, "obs", 100 + i, 8));
  }

  // A child process may not take the writer role while this handle holds it.
  const CrashRun blocked = run_crash_writer(temp.path(), temp.path(), "none", 1, 1);
  FSL_CHECK(blocked.exit_code != 0);

  // Reading from this process while the writer is attached yields exactly the
  // committed prefix.
  auto reader =
      fsl::Ledger::open(temp.path(), fsl::OpenMode::kReadOnly, fsl_test::deterministic_options());
  FSL_REQUIRE_OK(reader);
  fsl::Ledger read_handle = std::move(reader).value();
  FSL_CHECK_EQ(read_handle.watermark().value().sequence->value(),
               writer.watermark().value().sequence->value());
  FSL_CHECK(read_handle.verify().ok());
  FSL_REQUIRE_OK(read_handle.close());
  FSL_REQUIRE_OK(writer.close());
}

}  // namespace

FSL_TEST_MAIN("test_multiprocess")

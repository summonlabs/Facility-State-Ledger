// Facility State Ledger - DCCP Tranche 1
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "support/journal.hpp"
#include "support/test_support.hpp"

/// \file test_cli.cpp
/// The inspection CLI is a consumer of the public API, not a bypass around it.
/// These checks drive the real executable and assert on its observable
/// behaviour, including the exit codes that scripts depend on.

namespace {

#ifndef FSL_TOOL_PATH
#error "FSL_TOOL_PATH must be defined by the build system"
#endif
#ifndef FSL_CRASH_WRITER_PATH
#error "FSL_CRASH_WRITER_PATH must be defined by the build system"
#endif

struct Invocation {
  int exit_code = 0;
  std::string out;
  std::string err;
};

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
  std::string text;
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return text;
  }
  char buffer[1024];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, got);
  }
  std::fclose(file);
  return text;
}

[[nodiscard]] Invocation invoke(const std::filesystem::path& work,
                                const std::string& name,
                                const std::string& arguments) {
  const std::filesystem::path out = work / (name + ".out");
  const std::filesystem::path err = work / (name + ".err");
  const std::string inner = std::string("\"") + FSL_TOOL_PATH + "\" " + arguments + " > \"" +
                            out.string() + "\" 2> \"" + err.string() + "\"";
  // cmd.exe removes the first and last quote of a command that begins with one,
  // so the whole line is wrapped in an extra pair. These paths contain spaces.
  const std::string command = "\"" + inner + "\"";
  Invocation invocation;
  invocation.exit_code = std::system(command.c_str());
  invocation.out = read_text(out);
  invocation.err = read_text(err);
  return invocation;
}

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

FSL_TEST(help_and_usage_errors_are_reported_with_stable_exit_codes) {
  fsl_test::TempDirectory temp("cli_usage");
  const Invocation help = invoke(temp.path(), "help", "--help");
  FSL_CHECK_EQ(help.exit_code, 0);
  FSL_CHECK(contains(help.out, "usage: fsl <command>"));
  FSL_CHECK(contains(help.out, "read-only commands"));

  const Invocation unknown = invoke(temp.path(), "unknown", "no-such-command --dir nowhere");
  FSL_CHECK_EQ(unknown.exit_code, 1);
  FSL_CHECK(contains(unknown.err, "unknown command"));

  const Invocation no_directory = invoke(temp.path(), "nodir", "inspect");
  FSL_CHECK_EQ(no_directory.exit_code, 1);
  FSL_CHECK(contains(no_directory.err, "--dir"));

  const Invocation unknown_option = invoke(temp.path(), "badopt", "inspect --dir x --nonsense");
  FSL_CHECK_EQ(unknown_option.exit_code, 1);
  FSL_CHECK(contains(unknown_option.err, "unknown option"));
}

FSL_TEST(init_then_inspect_reports_the_created_ledger) {
  fsl_test::TempDirectory temp("cli_init");
  const std::filesystem::path ledger = temp.child("journal");
  const Invocation init = invoke(temp.path(), "init", "init --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(init.exit_code, 0);
  FSL_CHECK(contains(init.out, "created=yes"));
  FSL_CHECK(contains(init.out, "committed=1"));
  FSL_CHECK(contains(init.out, "ledger_id="));

  const Invocation inspect = invoke(temp.path(), "inspect", "inspect --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(inspect.exit_code, 0);
  FSL_CHECK(contains(inspect.out, "committed_sequence=1"));
  FSL_CHECK(contains(inspect.out, "facility_generation=1"));
  FSL_CHECK(contains(inspect.out, "open_epoch=none"));
  FSL_CHECK(contains(inspect.out, "writable=no"));
  FSL_CHECK(contains(inspect.out, "segment_count=1"));
}

FSL_TEST(append_commits_an_event_and_a_repeat_is_a_duplicate) {
  fsl_test::TempDirectory temp("cli_append");
  const std::filesystem::path ledger = temp.child("journal");
  FSL_CHECK_EQ(invoke(temp.path(), "init", "init --dir \"" + ledger.string() + "\"").exit_code, 0);

  const std::string epoch = "append --dir \"" + ledger.string() +
                            "\" --kind epoch-opened --subject ledger/facility-state-ledger"
                            " --source dccp.test --generation 1 --epoch 1";
  const Invocation first = invoke(temp.path(), "epoch", epoch);
  FSL_CHECK_EQ(first.exit_code, 0);
  FSL_CHECK(contains(first.out, "sequence=2"));
  FSL_CHECK(contains(first.out, "duplicate=no"));

  // The same command line derives the same event identity, so repeating it is
  // an idempotent retry rather than a second event.
  const Invocation repeat = invoke(temp.path(), "epoch_repeat", epoch);
  FSL_CHECK_EQ(repeat.exit_code, 0);
  FSL_CHECK(contains(repeat.out, "duplicate=yes"));
  FSL_CHECK(contains(repeat.out, "sequence=2"));

  const Invocation inspect = invoke(temp.path(), "inspect", "inspect --dir \"" + ledger.string() + "\"");
  FSL_CHECK(contains(inspect.out, "committed_sequence=2"));
  FSL_CHECK(contains(inspect.out, "open_epoch=1"));
}

FSL_TEST(a_rejected_append_reports_a_machine_readable_status) {
  fsl_test::TempDirectory temp("cli_reject");
  const std::filesystem::path ledger = temp.child("journal");
  FSL_CHECK_EQ(invoke(temp.path(), "init", "init --dir \"" + ledger.string() + "\"").exit_code, 0);
  FSL_CHECK_EQ(invoke(temp.path(), "epoch",
                      "append --dir \"" + ledger.string() +
                          "\" --kind epoch-opened --subject ledger/facility-state-ledger"
                          " --source dccp.test --generation 1 --epoch 1")
                  .exit_code,
               0);

  // A second epoch while one is open is a lifecycle violation.
  const Invocation duplicate_epoch =
      invoke(temp.path(), "epoch2", "append --dir \"" + ledger.string() +
                                        "\" --kind epoch-opened --subject ledger/facility-state-ledger"
                                        " --source dccp.test --generation 1 --epoch 2");
  FSL_CHECK_EQ(duplicate_epoch.exit_code, 2);
  FSL_CHECK(contains(duplicate_epoch.err, "conflict/illegal-lifecycle-transition"));

  // An event for an unknown subject is refused with a stable code.
  const Invocation unknown_subject =
      invoke(temp.path(), "unknown_subject",
             "append --dir \"" + ledger.string() +
                 "\" --kind observation-accepted --subject rack/no-such-rack"
                 " --source dccp.test --generation 1 --epoch 1 --ref obs-1");
  FSL_CHECK_EQ(unknown_subject.exit_code, 2);
  FSL_CHECK(contains(unknown_subject.err, "not-found/subject-not-registered"));
}

FSL_TEST(read_query_export_and_replay_agree_on_the_same_records) {
  fsl_test::TempDirectory temp("cli_read");
  const std::filesystem::path ledger = temp.child("journal");
  {
    auto created = fsl::Ledger::create(ledger, fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger handle = std::move(created).value();
    auto seed = fsl_test::seed_facility(handle);
    FSL_REQUIRE_OK(seed);
    for (std::uint64_t i = 0; i < 4; ++i) {
      FSL_REQUIRE_OK(fsl_test::record_observation(handle, seed->asset, "obs", 100 + i, 8));
    }
    FSL_REQUIRE_OK(handle.close());
  }

  const Invocation read = invoke(temp.path(), "read", "read --dir \"" + ledger.string() + "\" --sequence 2");
  FSL_CHECK_EQ(read.exit_code, 0);
  FSL_CHECK(contains(read.out, "\"sequence\":2"));
  FSL_CHECK(contains(read.out, "\"record\":\"header\""));

  const Invocation out_of_range =
      invoke(temp.path(), "read_far", "read --dir \"" + ledger.string() + "\" --sequence 100000");
  FSL_CHECK_EQ(out_of_range.exit_code, 2);
  FSL_CHECK(contains(out_of_range.err, "invalid-argument/range-invalid"));

  const Invocation query =
      invoke(temp.path(), "query", "query --dir \"" + ledger.string() +
                                       "\" --subject asset/site-a.hall-1.row-1.rack-1.asset-1 --limit 10");
  FSL_CHECK_EQ(query.exit_code, 0);
  FSL_CHECK(contains(query.err, "source=indexed"));
  FSL_CHECK(contains(query.err, "returned=5"));

  const Invocation export_all = invoke(temp.path(), "export", "export --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(export_all.exit_code, 0);
  FSL_CHECK(contains(export_all.out, "\"record\":\"header\""));
  FSL_CHECK(contains(export_all.out, "\"record\":\"trailer\""));
  FSL_CHECK(contains(export_all.out, "\"complete\":true"));

  const Invocation replay = invoke(temp.path(), "replay", "replay --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(replay.exit_code, 0);
  FSL_CHECK(contains(replay.err, "replayed=9"));

  const Invocation verify = invoke(temp.path(), "verify", "verify --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(verify.exit_code, 0);
  FSL_CHECK(contains(verify.out, "status=none/ok"));
  FSL_CHECK(contains(verify.out, "records_verified=9"));
  FSL_CHECK(contains(verify.out, "index_consistent=yes"));
}

FSL_TEST(structure_commands_enumerate_segments_subjects_sources_and_checkpoints) {
  fsl_test::TempDirectory temp("cli_structure");
  const std::filesystem::path ledger = temp.child("journal");
  {
    fsl::LedgerOptions options = fsl_test::deterministic_options();
    options.max_segment_bytes = 2048;
    auto created = fsl::Ledger::create(ledger, options);
    FSL_REQUIRE_OK(created);
    fsl::Ledger handle = std::move(created).value();
    auto seed = fsl_test::seed_facility(handle);
    FSL_REQUIRE_OK(seed);
    for (std::uint64_t i = 0; i < 12; ++i) {
      FSL_REQUIRE_OK(fsl_test::record_observation(handle, seed->asset, "obs", 100 + i, 16));
    }
    FSL_REQUIRE_OK(handle.create_checkpoint());
    FSL_REQUIRE_OK(handle.close());
  }

  const Invocation segments = invoke(temp.path(), "segments", "segments --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(segments.exit_code, 0);
  FSL_CHECK(contains(segments.out, "segment=1"));
  FSL_CHECK(contains(segments.out, "file=segment-0000000000000001.fsl"));

  const Invocation subjects = invoke(temp.path(), "subjects", "subjects --dir \"" + ledger.string() + "\" --limit 10");
  FSL_CHECK_EQ(subjects.exit_code, 0);
  FSL_CHECK(contains(subjects.out, "subject=asset/site-a.hall-1.row-1.rack-1.asset-1"));
  FSL_CHECK(contains(subjects.out, "registered=yes"));

  const Invocation sources = invoke(temp.path(), "sources", "sources --dir \"" + ledger.string() + "\" --limit 10");
  FSL_CHECK_EQ(sources.exit_code, 0);
  FSL_CHECK(contains(sources.out, "source=test.harness"));

  const Invocation checkpoints =
      invoke(temp.path(), "checkpoints", "checkpoints --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(checkpoints.exit_code, 0);
  FSL_CHECK(contains(checkpoints.out, "sequence="));
}

FSL_TEST(mutating_commands_operate_through_the_public_api) {
  fsl_test::TempDirectory temp("cli_mutate");
  const std::filesystem::path ledger = temp.child("journal");
  FSL_CHECK_EQ(invoke(temp.path(), "init", "init --dir \"" + ledger.string() + "\"").exit_code, 0);

  const Invocation rotate = invoke(temp.path(), "rotate", "rotate --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(rotate.exit_code, 0);
  FSL_CHECK(contains(rotate.out, "segment_count=2"));

  const Invocation checkpoint = invoke(temp.path(), "checkpoint", "checkpoint --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(checkpoint.exit_code, 0);
  FSL_CHECK(contains(checkpoint.out, "sequence=1"));

  const Invocation dry_run = invoke(temp.path(), "recover_dry", "recover --dir \"" + ledger.string() + "\" --dry-run");
  FSL_CHECK_EQ(dry_run.exit_code, 0);
  FSL_CHECK(contains(dry_run.out, "dry_run=yes"));
  FSL_CHECK(contains(dry_run.out, "truncated=no"));

  const Invocation recover = invoke(temp.path(), "recover", "recover --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(recover.exit_code, 0);
  FSL_CHECK(contains(recover.out, "status=none/ok"));
}

FSL_TEST(a_read_only_command_fails_cleanly_on_a_missing_directory) {
  fsl_test::TempDirectory temp("cli_missing");
  const std::filesystem::path missing = temp.child("not-a-ledger");
  const Invocation inspect = invoke(temp.path(), "missing", "inspect --dir \"" + missing.string() + "\"");
  FSL_CHECK_EQ(inspect.exit_code, 2);
  FSL_CHECK(contains(inspect.err, "not-found/ledger-not-found"));
  FSL_CHECK(inspect.out.empty());

  // A directory that exists but holds no ledger is reported the same way.
  std::filesystem::create_directories(missing);
  const Invocation empty = invoke(temp.path(), "empty", "inspect --dir \"" + missing.string() + "\"");
  FSL_CHECK_EQ(empty.exit_code, 2);
  FSL_CHECK(contains(empty.err, "not-found/ledger-not-found"));
}

FSL_TEST(the_cli_reports_damage_precisely_and_verifies_healthy_ledgers) {
  fsl_test::TempDirectory temp("cli_damage");
  const std::filesystem::path ledger = temp.child("journal");
  {
    auto created = fsl::Ledger::create(ledger, fsl_test::deterministic_options());
    FSL_REQUIRE_OK(created);
    fsl::Ledger handle = std::move(created).value();
    auto seed = fsl_test::seed_facility(handle);
    FSL_REQUIRE_OK(seed);
    FSL_REQUIRE_OK(handle.close());
  }

  const Invocation verify = invoke(temp.path(), "verify_ok", "verify --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(verify.exit_code, 0);
  FSL_CHECK(contains(verify.out, "status=none/ok"));

  std::FILE* file = std::fopen((ledger / "segments" / "segment-0000000000000001.fsl").string().c_str(), "r+b");
  FSL_REQUIRE(file != nullptr);
  FSL_CHECK(std::fseek(file, 140, SEEK_SET) == 0);
  FSL_CHECK(std::fputc(0x7F, file) != EOF);
  std::fclose(file);

  const Invocation damaged = invoke(temp.path(), "verify_damaged", "verify --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(damaged.exit_code, 2);
  FSL_CHECK(contains(damaged.err, "integrity-failure"));
}

FSL_TEST(the_crash_writer_commits_and_verifies_through_the_cli) {
  fsl_test::TempDirectory temp("cli_crash");
  const std::filesystem::path ledger = temp.child("journal");
  const std::filesystem::path out = temp.child("crash.out");
  const std::string inner = std::string("\"") + FSL_CRASH_WRITER_PATH + "\" --dir \"" +
                            ledger.string() + "\" --events 3 --batch 1 --boundary none > \"" +
                            out.string() + "\" 2>&1";
  const std::string command = "\"" + inner + "\"";
  FSL_CHECK_EQ(std::system(command.c_str()), 0);
  FSL_CHECK(contains(read_text(out), "committed 3"));

  const Invocation verify = invoke(temp.path(), "verify", "verify --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(verify.exit_code, 0);
  FSL_CHECK(contains(verify.out, "status=none/ok"));

  // The ledger holds its own creation record, the epoch and subject setup the
  // writer performs, and the three events it was asked for.
  const Invocation inspect = invoke(temp.path(), "inspect", "inspect --dir \"" + ledger.string() + "\"");
  FSL_CHECK_EQ(inspect.exit_code, 0);
  FSL_CHECK(contains(inspect.out, "committed_sequence=7"));
  FSL_CHECK(contains(inspect.out, "open_epoch=1"));
}

}  // namespace

FSL_TEST_MAIN("test_cli")

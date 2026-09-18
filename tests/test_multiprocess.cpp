// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Real multiprocess validation: the coordinator runs as its own OS process
// (flowplace serve), workers run as their own OS processes (flowplace worker),
// and every request and result crosses a real TCP socket as a framed message.
//
// Readiness is probed by waiting for the file the server writes once its
// listener is bound. The probe is bounded in attempts so that a server that
// never starts fails loudly with its log attached instead of hanging.

#include "fixtures.hpp"
#include "harness.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "flowplace/process.hpp"
#include "flowplace/scenario.hpp"

using namespace flowplace;
using namespace fptest;

#ifndef FLOWPLACE_CLI_PATH
#error "FLOWPLACE_CLI_PATH must be defined by the build"
#endif

namespace {

constexpr std::uint64_t kReadyProbeAttempts = 600;
constexpr std::uint64_t kReadyProbeMillis = 10;

std::filesystem::path MakeTempDirectory(const std::string& name) {
  static int counter = 0;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("flowplace-multiproc-" + name + "-" + std::to_string(CurrentProcessId()) + "-" +
       std::to_string(++counter));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  return directory;
}

struct TempDir {
  std::filesystem::path directory;
  explicit TempDir(const std::string& name) : directory(MakeTempDirectory(name)) {}
  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  [[nodiscard]] std::string File(const std::string& name) const {
    return (directory / name).string();
  }
};

std::string ReadTextFile(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return std::string();
  std::string text;
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0) text.append(buffer, read);
  std::fclose(file);
  return text;
}

void WriteTextFile(const std::string& path, const std::string& text) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return;
  static_cast<void>(std::fwrite(text.data(), 1, text.size(), file));
  std::fclose(file);
}

struct CliResult {
  int exit_code = -1;
  std::string report;
};

CliResult RunCli(const std::vector<std::string>& arguments, const std::string& report_path) {
  std::vector<std::string> argv;
  argv.push_back(FLOWPLACE_CLI_PATH);
  for (const std::string& argument : arguments) argv.push_back(argument);
  if (!report_path.empty()) {
    argv.push_back("--report");
    argv.push_back(report_path);
  }
  std::error_code error;
  std::filesystem::remove(report_path, error);
  ChildProcess child;
  const Status spawned = SpawnProcess(argv, "", &child);
  CliResult result;
  if (!spawned.ok()) {
    result.exit_code = -1;
    return result;
  }
  Result<int> exit_code = WaitForExit(child);
  DetachProcess(&child);
  result.exit_code = exit_code.ok() ? exit_code.value() : -1;
  if (!report_path.empty()) result.report = ReadTextFile(report_path);
  return result;
}

// Waits for the ready file written by the server once its listener is bound.
bool WaitForReadyFile(const std::string& path, std::string* port_out, std::string* log_out) {
  for (std::uint64_t attempt = 0; attempt < kReadyProbeAttempts; ++attempt) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
      const std::string text = ReadTextFile(path);
      if (!text.empty()) {
        *port_out = text;
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kReadyProbeMillis));
  }
  if (log_out != nullptr) *log_out = ReadTextFile(*log_out);
  return false;
}

std::string BaselineScenario() { return WriteScenario(Baseline(3)); }

// Waits for a child process to exit, killing it if it has not finished within a
// bounded number of checks. This is child lifecycle management inside the test
// harness: a coordinator that never reaches its request budget would otherwise
// be waited on forever. |killed| reports that the child had to be terminated,
// which makes the calling test fail loudly on its exit-code assertion.
Result<int> WaitForExitBounded(ChildProcess& child, std::uint64_t attempts, bool* killed) {
  *killed = false;
  for (std::uint64_t attempt = 0; attempt < attempts; ++attempt) {
    Result<std::optional<int>> code = TryWaitForExit(child);
    if (!code.ok()) return code.status();
    if (code.value().has_value()) return *code.value();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  *killed = true;
  static_cast<void>(KillProcess(child));
  Result<std::optional<int>> code = TryWaitForExit(child);
  if (code.ok() && code.value().has_value()) return *code.value();
  return -1;  // terminated without a recoverable exit code
}

}  // namespace

FP_TEST(multiprocess, worker_and_server_commit_over_a_real_socket) {
  TempDir dir("commit");
  const std::string store = dir.File("placements.log");
  const std::string ready = dir.File("ready.txt");
  const std::string log = dir.File("server.log");
  const std::string scenario = dir.File("scenario.txt");
  WriteTextFile(scenario, BaselineScenario());

  constexpr std::uint64_t kRequests = 8;
  std::vector<std::string> server_args = {
      "serve", "--store", store, "--port", "0", "--ready-file", ready, "--log-file", log,
      "--workers", "2", "--max-requests", std::to_string(kRequests)};
  std::vector<std::string> argv;
  argv.push_back(FLOWPLACE_CLI_PATH);
  for (const std::string& argument : server_args) argv.push_back(argument);
  ChildProcess server;
  FP_REQUIRE(SpawnProcess(argv, "", &server).ok());

  std::string port;
  std::string log_content;
  const bool ready_ok = WaitForReadyFile(ready, &port, &log_content);
  if (!ready_ok) {
    std::fprintf(stderr, "server log:\n%s\n", ReadTextFile(log).c_str());
  }
  FP_REQUIRE(ready_ok);
  FP_CHECK(!port.empty());

  const CliResult worker = RunCli({"worker", "--scenario", scenario, "--port", port, "--repeat",
                                   std::to_string(kRequests), "--expect-committed",
                                   std::to_string(kRequests), "--worker", "1"},
                                  dir.File("worker1.txt"));
  FP_CHECK_EQ(worker.exit_code, 0);
  FP_CHECK(worker.report.find("committed=8") != std::string::npos);
  FP_CHECK(worker.report.find("fenced=0") != std::string::npos);

  bool server_killed = false;
  Result<int> server_exit = WaitForExitBounded(server, 1500, &server_killed);
  FP_REQUIRE(server_exit.ok());
  DetachProcess(&server);
  FP_CHECK(!server_killed);
  FP_CHECK_EQ(server_exit.value(), 0);

  const CliResult inspect = RunCli({"store-inspect", "--store", store}, dir.File("inspect.txt"));
  FP_CHECK_EQ(inspect.exit_code, 0);
  FP_CHECK(inspect.report.find("liveness_restored=false") != std::string::npos);
  FP_CHECK(inspect.report.find("requiring_revalidation=1") != std::string::npos);
  FP_CHECK(inspect.report.find("unfinished=0") != std::string::npos);
  FP_CHECK(inspect.report.find("placement flow=100") != std::string::npos);
}

FP_TEST(multiprocess, several_workers_commit_against_one_coordinator) {
  TempDir dir("two-workers");
  const std::string store = dir.File("placements.log");
  const std::string ready = dir.File("ready.txt");
  const std::string log = dir.File("server.log");
  const std::string scenario = dir.File("scenario.txt");
  WriteTextFile(scenario, BaselineScenario());

  constexpr std::uint64_t kPerWorker = 4;
  std::vector<std::string> server_args = {
      "serve", "--store", store, "--port", "0", "--ready-file", ready, "--log-file", log,
      "--workers", "2", "--max-requests", std::to_string(kPerWorker * 2)};
  std::vector<std::string> argv;
  argv.push_back(FLOWPLACE_CLI_PATH);
  for (const std::string& argument : server_args) argv.push_back(argument);
  ChildProcess server;
  FP_REQUIRE(SpawnProcess(argv, "", &server).ok());
  std::string port;
  std::string log_content;
  FP_REQUIRE(WaitForReadyFile(ready, &port, &log_content));

  std::vector<std::string> other_args = {"worker", "--scenario", scenario, "--port", port,
                                         "--repeat", std::to_string(kPerWorker),
                                         "--expect-committed", std::to_string(kPerWorker),
                                         "--worker", "2"};
  std::vector<std::string> second_argv;
  second_argv.push_back(FLOWPLACE_CLI_PATH);
  for (const std::string& argument : other_args) second_argv.push_back(argument);
  second_argv.push_back("--report");
  second_argv.push_back(dir.File("worker2.txt"));
  ChildProcess second_worker;
  FP_REQUIRE(SpawnProcess(second_argv, "", &second_worker).ok());

  const CliResult first = RunCli({"worker", "--scenario", scenario, "--port", port, "--repeat",
                                  std::to_string(kPerWorker), "--expect-committed",
                                  std::to_string(kPerWorker), "--worker", "1"},
                                 dir.File("worker1.txt"));
  FP_CHECK_EQ(first.exit_code, 0);
  bool second_killed = false;
  Result<int> second_exit = WaitForExitBounded(second_worker, 1500, &second_killed);
  FP_REQUIRE(second_exit.ok());
  DetachProcess(&second_worker);
  FP_CHECK(!second_killed);
  FP_CHECK_EQ(second_exit.value(), 0);
  bool server_killed = false;
  Result<int> server_exit = WaitForExitBounded(server, 1500, &server_killed);
  FP_REQUIRE(server_exit.ok());
  DetachProcess(&server);
  FP_CHECK(!server_killed);
  FP_CHECK_EQ(server_exit.value(), 0);

  const CliResult inspect = RunCli({"store-inspect", "--store", store}, dir.File("inspect.txt"));
  FP_CHECK_EQ(inspect.exit_code, 0);
  FP_CHECK(inspect.report.find("placements=8") != std::string::npos);
  FP_CHECK(inspect.report.find("committed=8") != std::string::npos);
  FP_CHECK_EQ(first.report.find("committed=4") != std::string::npos, true);
  FP_CHECK_EQ(ReadTextFile(dir.File("worker2.txt")).find("committed=4") != std::string::npos, true);
}

FP_TEST(multiprocess, stale_epoch_worker_is_rejected_at_the_handshake) {
  TempDir dir("stale-epoch");
  const std::string store = dir.File("placements.log");
  const std::string ready = dir.File("ready.txt");
  const std::string scenario = dir.File("scenario.txt");
  WriteTextFile(scenario, BaselineScenario());
  std::vector<std::string> server_args = {"serve", "--store", store, "--port", "0",
                                          "--ready-file", ready, "--epoch", "3",
                                          "--max-requests", "0"};
  std::vector<std::string> argv;
  argv.push_back(FLOWPLACE_CLI_PATH);
  for (const std::string& argument : server_args) argv.push_back(argument);
  ChildProcess server;
  FP_REQUIRE(SpawnProcess(argv, "", &server).ok());
  std::string port;
  std::string log_content;
  FP_REQUIRE(WaitForReadyFile(ready, &port, &log_content));

  const CliResult stale = RunCli({"worker", "--scenario", scenario, "--port", port, "--epoch", "2",
                                  "--repeat", "1"},
                                 dir.File("stale.txt"));
  FP_CHECK_EQ(stale.exit_code, 3);
  FP_CHECK(stale.report.find("handshake rejected") != std::string::npos);

  const CliResult current = RunCli({"worker", "--scenario", scenario, "--port", port, "--epoch", "3",
                                    "--repeat", "2", "--expect-committed", "2"},
                                   dir.File("current.txt"));
  FP_CHECK_EQ(current.exit_code, 0);

  FP_CHECK(KillProcess(server).ok());
  DetachProcess(&server);
}

FP_TEST(multiprocess, killed_coordinator_leaves_a_recoverable_store) {
  TempDir dir("kill");
  const std::string store = dir.File("placements.log");
  const std::string ready = dir.File("ready.txt");
  const std::string log = dir.File("server.log");
  const std::string scenario = dir.File("scenario.txt");
  WriteTextFile(scenario, BaselineScenario());
  std::vector<std::string> server_args = {"serve", "--store", store, "--port", "0",
                                          "--ready-file", ready, "--log-file", log,
                                          "--max-requests", "0", "--workers", "2"};
  std::vector<std::string> argv;
  argv.push_back(FLOWPLACE_CLI_PATH);
  for (const std::string& argument : server_args) argv.push_back(argument);
  ChildProcess server;
  FP_REQUIRE(SpawnProcess(argv, "", &server).ok());
  std::string port;
  std::string log_content;
  FP_REQUIRE(WaitForReadyFile(ready, &port, &log_content));

  const CliResult worker = RunCli({"worker", "--scenario", scenario, "--port", port, "--repeat", "3",
                                   "--expect-committed", "3"},
                                  dir.File("worker.txt"));
  FP_CHECK_EQ(worker.exit_code, 0);

  // Simulated crash: the coordinator is killed without shutting down.
  FP_CHECK(KillProcess(server).ok());
  DetachProcess(&server);

  const CliResult inspect = RunCli({"store-inspect", "--store", store}, dir.File("inspect.txt"));
  FP_CHECK_EQ(inspect.exit_code, 0);
  FP_CHECK(inspect.report.find("placements=3") != std::string::npos);
  FP_CHECK(inspect.report.find("liveness_restored=false") != std::string::npos);

  // A fresh coordinator opens the same store with a new incarnation and every
  // recovered placement requires revalidation.
  const CliResult second = RunCli({"place", "--scenario", scenario, "--store", store},
                                  dir.File("place.txt"));
  FP_CHECK_EQ(second.exit_code, 0);
  FP_CHECK(second.report.find("outcome=PLACED") != std::string::npos);
  const CliResult after = RunCli({"store-inspect", "--store", store}, dir.File("after.txt"));
  FP_CHECK_EQ(after.exit_code, 0);
  FP_CHECK(after.report.find("placements=4") != std::string::npos);
  FP_CHECK(after.report.find("committed=4") != std::string::npos);
}

FP_TEST(multiprocess, cli_end_to_end_place_validate_and_roundtrip) {
  TempDir dir("cli");
  const std::string scenario = dir.File("scenario.txt");
  const std::string store = dir.File("placements.log");
  WriteTextFile(scenario, BaselineScenario());

  const CliResult validate = RunCli({"validate", "--scenario", scenario}, dir.File("validate.txt"));
  FP_CHECK_EQ(validate.exit_code, 0);
  FP_CHECK(validate.report.find("valid:") != std::string::npos);
  FP_CHECK(validate.report.find("request_digest=") != std::string::npos);

  const CliResult roundtrip =
      RunCli({"roundtrip", "--scenario", scenario}, dir.File("roundtrip.txt"));
  FP_CHECK_EQ(roundtrip.exit_code, 0);
  FP_CHECK(roundtrip.report.find("roundtrip ok") != std::string::npos);

  const CliResult place = RunCli({"place", "--scenario", scenario}, dir.File("place.txt"));
  FP_CHECK_EQ(place.exit_code, 0);
  FP_CHECK(place.report.find("outcome=PLACED") != std::string::npos);
  FP_CHECK(place.report.find("ranking:") != std::string::npos);

  const CliResult committed =
      RunCli({"place", "--scenario", scenario, "--store", store}, dir.File("commit.txt"));
  FP_CHECK_EQ(committed.exit_code, 0);
  FP_CHECK(committed.report.find("state=COMMITTED") != std::string::npos);

  const CliResult compact =
      RunCli({"store-compact", "--store", store}, dir.File("compact.txt"));
  FP_CHECK_EQ(compact.exit_code, 0);
  const CliResult inspected = RunCli({"store-inspect", "--store", store}, dir.File("inspect.txt"));
  FP_CHECK_EQ(inspected.exit_code, 0);
  FP_CHECK(inspected.report.find("snapshot_loaded=true") != std::string::npos);
  WriteTextFile(dir.File("unknown.txt"), BaselineScenario() + "mystery key 1\n");

  // A scenario that no longer matches its authority expectations is a
  // well-formed request that must not be placed: exit code 2, outcome
  // STALE_INPUT, and no durable mutation.
  std::string stale = BaselineScenario();
  const std::string from = "expected pathauth 7 ";
  const std::size_t position = stale.find(from);
  FP_CHECK(position != std::string::npos);
  stale.replace(position, from.size(), "expected pathauth 8 ");
  WriteTextFile(dir.File("stale.txt"), stale);
  const CliResult rejected =
      RunCli({"place", "--scenario", dir.File("stale.txt")}, dir.File("stale-report.txt"));
  FP_CHECK_EQ(rejected.exit_code, 2);
  FP_CHECK(rejected.report.find("outcome=STALE_INPUT") != std::string::npos);

  // Missing inputs are usage errors, and the grammar rejects unknown keys.
  const CliResult missing = RunCli({"place"}, dir.File("missing.txt"));
  FP_CHECK_EQ(missing.exit_code, 1);
  const CliResult unknown_key =
      RunCli({"validate", "--scenario", dir.File("unknown.txt")}, dir.File("unknown-report.txt"));
  FP_CHECK_EQ(unknown_key.exit_code, 1);
}

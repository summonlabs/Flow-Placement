// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Minimal process control used by the multiprocess validation harness. It
// exists so that the test suite can start and kill real OS processes; it is not
// part of the placement boundary.

#ifndef FLOWPLACE_PROCESS_HPP
#define FLOWPLACE_PROCESS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "flowplace/status.hpp"

namespace flowplace {

struct ChildProcess {
  std::int64_t id = 0;      // OS process id
  void* handle = nullptr;   // platform handle, owned by this object
  bool running = false;
};

// Start a process. |argv| is the full command line including the executable.
Status SpawnProcess(const std::vector<std::string>& argv, const std::string& working_directory,
                    ChildProcess* out);

// True when the process is still running.
[[nodiscard]] bool ProcessRunning(ChildProcess& child);

// Block until the process exits and return its exit code.
Result<int> WaitForExit(ChildProcess& child);

// Non-blocking completion check: returns the exit code when the process has
// exited, an empty optional while it is still running, and an error only when
// the operating system call itself fails. On POSIX this reaps the child, so it
// must not be combined with a later WaitForExit for the same process.
[[nodiscard]] Result<std::optional<int>> TryWaitForExit(ChildProcess& child);

// Terminate the process without allowing it to clean up (simulated crash).
Status KillProcess(ChildProcess& child);

// Release the handle. The process keeps running if it has not exited.
void DetachProcess(ChildProcess* child);

[[nodiscard]] std::int64_t CurrentProcessId();
[[nodiscard]] std::int64_t CurrentUnixNanos();

}  // namespace flowplace

#endif  // FLOWPLACE_PROCESS_HPP

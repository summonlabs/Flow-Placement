// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/process.hpp"

#include <chrono>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
extern char** environ;
#endif

namespace flowplace {
namespace {

#ifdef _WIN32
// Quotes one argument for the Windows command line parser.
std::string QuoteArgument(const std::string& argument) {
  const bool needs_quotes =
      argument.empty() || argument.find_first_of(" \t\n\v\"") != std::string::npos;
  if (!needs_quotes) return argument;
  std::string out = "\"";
  for (std::size_t i = 0; i < argument.size(); ++i) {
    std::size_t backslashes = 0;
    while (i < argument.size() && argument[i] == '\\') {
      ++backslashes;
      ++i;
    }
    if (i == argument.size()) {
      out.append(backslashes * 2, '\\');
      break;
    }
    if (argument[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(argument[i]);
    }
  }
  out.push_back('"');
  return out;
}
#endif

}  // namespace

Status SpawnProcess(const std::vector<std::string>& argv, const std::string& working_directory,
                    ChildProcess* out) {
  if (argv.empty()) return Status(StatusCode::kInvalidArgument, "argv must not be empty");
  if (out == nullptr) return Status(StatusCode::kInvalidArgument, "output handle is null");
  *out = ChildProcess{};

#ifdef _WIN32
  std::string command_line;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) command_line.push_back(' ');
    command_line += QuoteArgument(argv[i]);
  }
  STARTUPINFOA startup;
  PROCESS_INFORMATION info;
  std::memset(&startup, 0, sizeof(startup));
  std::memset(&info, 0, sizeof(info));
  startup.cb = sizeof(startup);
  const char* cwd = working_directory.empty() ? nullptr : working_directory.c_str();
  if (CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd,
                     &startup, &info) == 0) {
    return Status(StatusCode::kIoError, "CreateProcess failed for " + argv[0]);
  }
  CloseHandle(info.hThread);
  out->id = static_cast<std::int64_t>(info.dwProcessId);
  out->handle = info.hProcess;
  out->running = true;
  return Status::Ok();
#else
  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (const std::string& argument : argv) {
    raw.push_back(const_cast<char*>(argument.c_str()));
  }
  raw.push_back(nullptr);
  pid_t pid = 0;
  const char* cwd = working_directory.empty() ? nullptr : working_directory.c_str();
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  const int rc = posix_spawn(&pid, raw[0], &actions, nullptr, raw.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  static_cast<void>(cwd);
  if (rc != 0) return Status(StatusCode::kIoError, "posix_spawn failed for " + argv[0]);
  out->id = static_cast<std::int64_t>(pid);
  out->handle = nullptr;
  out->running = true;
  return Status::Ok();
#endif
}

bool ProcessRunning(ChildProcess& child) {
  if (!child.running) return false;
#ifdef _WIN32
  if (child.handle == nullptr) return false;
  const DWORD result = WaitForSingleObject(static_cast<HANDLE>(child.handle), 0);
  if (result == WAIT_TIMEOUT) return true;
  child.running = false;
  return false;
#else
  if (child.id <= 0) return false;
  int status = 0;
  const pid_t result = waitpid(static_cast<pid_t>(child.id), &status, WNOHANG);
  if (result == 0) return true;
  child.running = false;
  return false;
#endif
}

Result<int> WaitForExit(ChildProcess& child) {
  if (!child.running) return Status(StatusCode::kInvalidArgument, "process is not running");
#ifdef _WIN32
  if (child.handle == nullptr) return Status(StatusCode::kInvalidArgument, "process handle is null");
  const DWORD result = WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
  if (result != WAIT_OBJECT_0) {
    return Status(StatusCode::kIoError, "WaitForSingleObject failed");
  }
  DWORD exit_code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(child.handle), &exit_code) == 0) {
    return Status(StatusCode::kIoError, "GetExitCodeProcess failed");
  }
  child.running = false;
  return static_cast<int>(exit_code);
#else
  int status = 0;
  if (waitpid(static_cast<pid_t>(child.id), &status, 0) < 0) {
    return Status(StatusCode::kIoError, "waitpid failed");
  }
  child.running = false;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return Status(StatusCode::kIoError, "unexpected wait status");
#endif
}

Result<std::optional<int>> TryWaitForExit(ChildProcess& child) {
  if (!child.running) return std::optional<int>{};
#ifdef _WIN32
  if (child.handle == nullptr) return std::optional<int>{};
  const DWORD result = WaitForSingleObject(static_cast<HANDLE>(child.handle), 0);
  if (result == WAIT_TIMEOUT) return std::optional<int>{};
  if (result != WAIT_OBJECT_0) {
    return Status(StatusCode::kIoError, "WaitForSingleObject failed");
  }
  DWORD exit_code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(child.handle), &exit_code) == 0) {
    return Status(StatusCode::kIoError, "GetExitCodeProcess failed");
  }
  child.running = false;
  return std::optional<int>{static_cast<int>(exit_code)};
#else
  int status = 0;
  const pid_t result = waitpid(static_cast<pid_t>(child.id), &status, WNOHANG);
  if (result == 0) return std::optional<int>{};
  if (result < 0) return Status(StatusCode::kIoError, "waitpid failed");
  child.running = false;
  if (WIFEXITED(status)) return std::optional<int>{WEXITSTATUS(status)};
  if (WIFSIGNALED(status)) return std::optional<int>{128 + WTERMSIG(status)};
  return Status(StatusCode::kIoError, "unexpected wait status");
#endif
}

Status KillProcess(ChildProcess& child) {
  if (!child.running) return Status(StatusCode::kInvalidArgument, "process is not running");
#ifdef _WIN32
  if (child.handle == nullptr) return Status(StatusCode::kInvalidArgument, "process handle is null");
  if (TerminateProcess(static_cast<HANDLE>(child.handle), 0xDEADu) == 0) {
    return Status(StatusCode::kIoError, "TerminateProcess failed");
  }
  static_cast<void>(WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE));
  child.running = false;
  return Status::Ok();
#else
  if (kill(static_cast<pid_t>(child.id), SIGKILL) != 0) {
    return Status(StatusCode::kIoError, "kill failed");
  }
  int status = 0;
  static_cast<void>(waitpid(static_cast<pid_t>(child.id), &status, 0));
  child.running = false;
  return Status::Ok();
#endif
}

void DetachProcess(ChildProcess* child) {
  if (child == nullptr) return;
#ifdef _WIN32
  if (child->handle != nullptr) {
    CloseHandle(static_cast<HANDLE>(child->handle));
    child->handle = nullptr;
  }
#else
  static_cast<void>(child);
#endif
  child->running = false;
}

std::int64_t CurrentProcessId() {
#ifdef _WIN32
  return static_cast<std::int64_t>(GetCurrentProcessId());
#else
  return static_cast<std::int64_t>(getpid());
#endif
}

std::int64_t CurrentUnixNanos() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

}  // namespace flowplace

// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Coordinator server process: accepts framed connections, validates the
// handshake (protocol version, epoch), and serves placement requests through a
// PlacementCoordinator. Every connection is served by one bounded worker
// thread; connections beyond the bound are refused rather than queued.

#ifndef FLOWPLACE_SERVER_HPP
#define FLOWPLACE_SERVER_HPP

#include <cstdint>
#include <string>

#include "flowplace/runtime.hpp"
#include "flowplace/status.hpp"
#include "flowplace/transport.hpp"

namespace flowplace {

struct ServerOptions {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;                 // 0 = ephemeral
  std::string store_path;                 // empty = in-memory
  std::uint32_t worker_threads = 4;
  std::uint64_t queue_capacity = 1024;
  std::uint32_t max_connections = 16;
  std::uint32_t max_frame_bytes = kDefaultMaxFrameBytes;
  FabricEpoch epoch{1};
  // Serve at most this many placement requests, then shut down. 0 = unbounded
  // (the server then runs until a shutdown frame or a signal arrives).
  std::uint64_t max_requests = 0;
  // Announce readiness by creating this file (used by the multiprocess
  // validation harness). Empty = no announcement file.
  std::string ready_file;
  std::string log_file;
};

struct ServerResult {
  std::uint16_t port = 0;
  std::uint64_t requests_served = 0;
  std::uint64_t connections = 0;
  std::uint64_t rejected_handshakes = 0;
  std::uint64_t protocol_errors = 0;
  std::uint64_t fenced_requests = 0;
  std::uint64_t committed = 0;
  bool clean_shutdown = false;
};

Result<ServerResult> RunServer(const ServerOptions& options);

// Client helper used by worker processes and by the multiprocess tests.
struct WorkerOptions {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;
  FabricEpoch epoch{1};
  WorkerId worker;
  std::uint32_t max_frame_bytes = kDefaultMaxFrameBytes;
  std::uint64_t connect_attempts = 400;      // bounded readiness probe
  std::uint64_t connect_retry_millis = 10;   // probe interval
};

struct WorkerResult {
  bool handshake_accepted = false;
  StatusCode handshake_code = StatusCode::kOk;
  std::string handshake_message;
  IncarnationId coordinator;
  FabricEpoch coordinator_epoch;
  std::uint64_t placements_committed = 0;
  std::uint64_t placements_rejected = 0;
  std::uint64_t responses_fenced = 0;
  std::uint64_t responses_other = 0;
};

// Connect, handshake, and return an open connection together with the
// coordinator's handshake response.
Result<TcpConnection> ConnectWorker(const WorkerOptions& options, WorkerResult* result);

}  // namespace flowplace

#endif  // FLOWPLACE_SERVER_HPP

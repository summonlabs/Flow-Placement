// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "flowplace/codec.hpp"
#include "flowplace/process.hpp"
#include "flowplace/transport.hpp"
#include "flowplace/version.hpp"
#include "flowplace/wire.hpp"

namespace flowplace {
namespace {

// One connection is owned by one entry: the entry keeps the socket alive so
// that shutdown can interrupt a blocked receive, and the thread handle so that
// finished connections can be reaped while the server keeps running.
struct ConnectionEntry {
  std::shared_ptr<TcpConnection> connection;
  std::atomic<bool> done{false};
  std::thread thread;
};

struct ServerContext {
  ServerOptions options;
  PlacementCoordinator* coordinator = nullptr;
  std::atomic<std::uint64_t> requests_served{0};
  std::atomic<std::uint64_t> connections{0};
  std::atomic<std::uint64_t> rejected_handshakes{0};
  std::atomic<std::uint64_t> protocol_errors{0};
  std::atomic<std::uint64_t> fenced_requests{0};
  std::atomic<std::uint64_t> committed{0};
  std::atomic<std::uint64_t> live_connections{0};
  std::atomic<bool> stop{false};
  std::mutex log_mu;
  std::FILE* log_file = nullptr;
  std::mutex open_connections_mu;
  std::vector<std::shared_ptr<ConnectionEntry>> open_connections;
};

void Log(ServerContext& context, const std::string& message) {
  std::lock_guard<std::mutex> lock(context.log_mu);
  if (context.log_file == nullptr) return;
  static_cast<void>(std::fputs(message.c_str(), context.log_file));
  static_cast<void>(std::fputc('\n', context.log_file));
  static_cast<void>(std::fflush(context.log_file));
}

Status SendError(TcpConnection& connection, StatusCode code, AttemptId attempt,
                 const std::string& message, std::uint64_t sequence) {
  Frame frame;
  frame.kind = FrameKind::kError;
  frame.sequence = sequence;
  ErrorPayload payload;
  payload.code = code;
  payload.attempt = attempt;
  payload.message = message;
  ByteWriter writer;
  EncodeErrorPayload(payload, writer);
  frame.body = writer.data();
  return connection.SendFrame(frame);
}

Status SendPlaceResult(TcpConnection& connection, const AttemptOutcome& outcome,
                       std::uint64_t sequence) {
  Frame frame;
  frame.kind = FrameKind::kPlaceResult;
  frame.sequence = sequence;
  PlaceResultPayload payload;
  payload.attempt = outcome.attempt;
  payload.state = outcome.state;
  payload.code = outcome.code;
  payload.placement_id = outcome.placement_id;
  payload.placement_generation = outcome.placement_generation;
  payload.decision = outcome.decision;
  ByteWriter writer;
  EncodePlaceResult(payload, writer);
  frame.body = writer.data();
  return connection.SendFrame(frame);
}

// One connection is served by one thread. No coordinator lock is held while a
// frame is read or written.
void ServeConnection(ServerContext& context, TcpConnection& connection) {
  struct ConnectionGuard {
    ServerContext* context;
    ~ConnectionGuard() { --context->live_connections; }
  } guard{&context};

  const Limits& wire_limits = context.coordinator->limits();

  std::uint64_t sequence = 0;
  Result<Frame> hello_frame = connection.ReceiveFrame();
  if (!hello_frame.ok()) {
    ++context.rejected_handshakes;
    Log(context, "handshake read failed: " + hello_frame.status().ToString());
    return;
  }
  if (hello_frame.value().kind != FrameKind::kHello) {
    ++context.protocol_errors;
    static_cast<void>(SendError(connection, StatusCode::kWireMalformed, AttemptId{},
                                "first frame must be HELLO", ++sequence));
    Log(context, "handshake rejected: first frame was not HELLO");
    return;
  }
  HelloPayload hello;
  {
    ByteReader reader(hello_frame.value().body);
    const Status status = DecodeHello(reader, &hello);
    if (!status.ok()) {
      ++context.protocol_errors;
      static_cast<void>(SendError(connection, status.code(), AttemptId{}, status.message().data(),
                                  ++sequence));
      Log(context, "handshake rejected: malformed HELLO");
      return;
    }
  }
  const std::uint32_t negotiated =
      std::min<std::uint32_t>(hello.max_frame_bytes, context.options.max_frame_bytes);
  connection.SetMaxFrameBytes(negotiated);

  HelloAckPayload ack;
  ack.protocol_version = kWireProtocolVersion;
  ack.coordinator = context.coordinator->incarnation();
  ack.epoch = context.coordinator->epoch();
  ack.max_frame_bytes = negotiated;
  ack.worker_limit = context.options.max_connections;
  ack.build = std::string(kProjectName) + " " + std::string(kVersionString);
  ack.accepted = true;
  if (hello.protocol_version != kWireProtocolVersion) {
    ack.accepted = false;
    ack.code = StatusCode::kProtocolMismatch;
    ack.message = "wire protocol version mismatch";
    ++context.rejected_handshakes;
  } else if (hello.epoch != ack.epoch) {
    ack.accepted = false;
    ack.code = StatusCode::kStaleFabricEpoch;
    ack.message = "worker epoch is not the coordinator epoch";
    ++context.rejected_handshakes;
    ++context.fenced_requests;
  }
  {
    Frame frame;
    frame.kind = FrameKind::kHelloAck;
    frame.sequence = ++sequence;
    ByteWriter writer;
    EncodeHelloAck(ack, writer);
    frame.body = writer.data();
    const Status status = connection.SendFrame(frame);
    if (!status.ok()) return;
  }
  Log(context, std::string("handshake ") + (ack.accepted ? "accepted" : "rejected") +
                   " worker=" + hello.worker.ToString() +
                   " epoch=" + std::to_string(hello.epoch.value()));
  if (!ack.accepted) return;
  ++context.connections;

  while (!context.stop.load()) {
    Result<Frame> received = connection.ReceiveFrame();
    if (!received.ok()) {
      if (received.status().code() != StatusCode::kConnectionClosed) {
        Log(context, "connection ended: " + received.status().ToString());
      }
      return;
    }
    Frame frame = std::move(received.value());
    switch (frame.kind) {
      case FrameKind::kPlaceRequest: {
        PlaceRequestPayload payload;
        ByteReader reader(frame.body);
        const Status decoded = DecodePlaceRequest(reader, wire_limits, &payload);
        if (!decoded.ok()) {
          ++context.protocol_errors;
          static_cast<void>(SendError(connection, decoded.code(), AttemptId{},
                                      std::string(decoded.message()), ++sequence));
          Log(context, "place request rejected: " + decoded.ToString());
          return;
        }
        if (payload.epoch != context.coordinator->epoch()) {
          ++context.fenced_requests;
          static_cast<void>(SendError(connection, StatusCode::kFencedStaleEpoch, payload.attempt,
                                      "request is bound to a superseded fabric epoch", ++sequence));
          Log(context, "place request fenced: epoch " + std::to_string(payload.epoch.value()));
          continue;
        }
        if (!payload.request.attempt.valid()) payload.request.attempt = payload.attempt;
        Result<AttemptOutcome> outcome =
            context.coordinator->PlaceNow(std::move(payload.request), payload.attempt);
        if (!outcome.ok()) {
          static_cast<void>(
              SendError(connection, outcome.status().code(), payload.attempt,
                        std::string(outcome.status().message()), ++sequence));
          Log(context, "place request failed: " + outcome.status().ToString());
          continue;
        }
        if (outcome.value().state == AttemptState::kCommitted) ++context.committed;
        const Status sent = SendPlaceResult(connection, outcome.value(), ++sequence);
        ++context.requests_served;
        if (!sent.ok()) return;
        if (context.options.max_requests != 0 &&
            context.requests_served.load() >= context.options.max_requests) {
          context.stop.store(true);
          return;
        }
        break;
      }
      case FrameKind::kSetEpoch: {
        EpochPayload payload;
        ByteReader reader(frame.body);
        const Status decoded = DecodeEpochPayload(reader, &payload);
        if (!decoded.ok()) {
          ++context.protocol_errors;
          static_cast<void>(SendError(connection, decoded.code(), AttemptId{},
                                      std::string(decoded.message()), ++sequence));
          return;
        }
        AuthorityExpectation authority = context.coordinator->authority();
        const FabricEpoch previous = context.coordinator->epoch();
        authority.fabric_epoch = payload.epoch;
        const Status applied = context.coordinator->SetAuthority(authority);
        EpochPayload reply;
        reply.previous_epoch = previous;
        reply.epoch = context.coordinator->epoch();
        reply.code = applied.ok() ? StatusCode::kOk : applied.code();
        reply.message = applied.ok() ? "epoch updated" : std::string(applied.message());
        Frame reply_frame;
        reply_frame.kind = FrameKind::kEpochAck;
        reply_frame.sequence = ++sequence;
        ByteWriter writer;
        EncodeEpochPayload(reply, writer);
        reply_frame.body = writer.data();
        const Status sent = connection.SendFrame(reply_frame);
        Log(context, "epoch request: " + std::to_string(payload.epoch.value()) + " -> " +
                         (applied.ok() ? "applied" : applied.ToString()));
        if (!sent.ok()) return;
        break;
      }
      case FrameKind::kRecoveryQuery: {
        Frame reply;
        reply.kind = FrameKind::kRecoveryReport;
        reply.sequence = ++sequence;
        ByteWriter writer;
        EncodeRecoveryReport(context.coordinator->recovery(), writer);
        reply.body = writer.data();
        const Status sent = connection.SendFrame(reply);
        if (!sent.ok()) return;
        break;
      }
      case FrameKind::kStatsQuery: {
        const CoordinatorStats stats = context.coordinator->stats();
        ByteWriter writer;
        const std::uint64_t values[] = {
            stats.submitted,  stats.accepted,  stats.duplicate_replays,
            stats.duplicate_conflicts, stats.rejected_queue_full,
            stats.rejected_shutting_down, stats.placed, stats.rejected,
            stats.cancelled,  stats.fenced,    stats.failed,
            stats.queued,     stats.active,    stats.completed};
        for (const std::uint64_t value : values) writer.U64(value);
        Frame reply;
        reply.kind = FrameKind::kStatsReport;
        reply.sequence = ++sequence;
        reply.body = writer.data();
        const Status sent = connection.SendFrame(reply);
        if (!sent.ok()) return;
        break;
      }
      case FrameKind::kShutdown: {
        Frame reply;
        reply.kind = FrameKind::kShutdownAck;
        reply.sequence = ++sequence;
        EpochPayload payload;
        payload.epoch = context.coordinator->epoch();
        payload.previous_epoch = payload.epoch;
        payload.code = StatusCode::kOk;
        payload.message = "shutting down";
        ByteWriter writer;
        EncodeEpochPayload(payload, writer);
        reply.body = writer.data();
        static_cast<void>(connection.SendFrame(reply));
        context.stop.store(true);
        Log(context, "shutdown requested by peer");
        return;
      }
      case FrameKind::kHello:
      case FrameKind::kHelloAck:
      case FrameKind::kPlaceResult:
      case FrameKind::kEpochAck:
      case FrameKind::kShutdownAck:
      case FrameKind::kError:
      case FrameKind::kRecoveryReport:
      case FrameKind::kStatsReport: {
        ++context.protocol_errors;
        static_cast<void>(SendError(connection, StatusCode::kWireMalformed, AttemptId{},
                                    "unexpected frame kind from worker", ++sequence));
        return;
      }
    }
  }
}

}  // namespace

Result<ServerResult> RunServer(const ServerOptions& options) {
  const Status sockets = InitializeSockets();
  if (!sockets.ok()) return sockets;

  Result<TcpListener> listener = TcpListener::Bind(options.address, options.port);
  if (!listener.ok()) return listener.status();

  ServerContext context;
  context.options = options;
  if (!options.log_file.empty()) {
    context.log_file = std::fopen(options.log_file.c_str(), "ab");
  }

  CoordinatorOptions coordinator_options;
  coordinator_options.store_path = options.store_path;
  coordinator_options.worker_threads = options.worker_threads;
  coordinator_options.queue_capacity = options.queue_capacity;
  coordinator_options.initial_epoch = options.epoch;
  Result<std::unique_ptr<PlacementCoordinator>> coordinator =
      PlacementCoordinator::Start(coordinator_options);
  if (!coordinator.ok()) return coordinator.status();
  context.coordinator = coordinator.value().get();

  ServerResult result;
  result.port = listener.value().port();

  if (!options.ready_file.empty()) {
    std::FILE* ready = std::fopen(options.ready_file.c_str(), "wb");
    if (ready == nullptr) {
      if (context.log_file != nullptr) static_cast<void>(std::fclose(context.log_file));
      return Status(StatusCode::kIoError, "cannot create ready file " + options.ready_file);
    }
    const std::string text = std::to_string(result.port);
    static_cast<void>(std::fwrite(text.data(), 1, text.size(), ready));
    static_cast<void>(std::fflush(ready));
    static_cast<void>(std::fclose(ready));
  }
  Log(context, "listening on " + options.address + ":" + std::to_string(result.port) +
                   " epoch=" + std::to_string(options.epoch.value()));

  while (!context.stop.load()) {
    // Reap connections that have finished so that a connect/disconnect loop
    // cannot grow the tracked set without bound.
    {
      std::lock_guard<std::mutex> lock(context.open_connections_mu);
      for (auto it = context.open_connections.begin(); it != context.open_connections.end();) {
        if ((*it)->done.load()) {
          if ((*it)->thread.joinable()) (*it)->thread.join();
          it = context.open_connections.erase(it);
        } else {
          ++it;
        }
      }
    }
    bool readable = false;
    const Status wait = listener.value().WaitReadable(50, &readable);
    if (!wait.ok()) {
      Log(context, "listener wait failed: " + wait.ToString());
      break;
    }
    if (!readable) continue;
    Result<TcpConnection> connection = listener.value().Accept();
    if (!connection.ok()) {
      if (context.stop.load()) break;
      Log(context, "accept failed: " + connection.status().ToString());
      break;
    }
    if (context.live_connections.load() >= options.max_connections) {
      static_cast<void>(SendError(connection.value(), StatusCode::kWorkerLimitExceeded, AttemptId{},
                                  "connection limit reached", 1));
      ++context.rejected_handshakes;
      Log(context, "connection refused: limit reached");
      continue;
    }
    ++context.live_connections;
    auto entry = std::make_shared<ConnectionEntry>();
    entry->connection = std::make_shared<TcpConnection>(std::move(connection.value()));
    {
      std::lock_guard<std::mutex> lock(context.open_connections_mu);
      context.open_connections.push_back(entry);
    }
    entry->thread = std::thread([&context, entry]() {
      ServeConnection(context, *entry->connection);
      entry->done.store(true);
    });
  }

  // Shutdown: interrupt every open connection first, so a peer that is idle
  // cannot keep a connection thread blocked in a receive forever...
  context.stop.store(true);
  {
    std::lock_guard<std::mutex> lock(context.open_connections_mu);
    for (const std::shared_ptr<ConnectionEntry>& entry : context.open_connections) {
      static_cast<void>(entry->connection->Close());
    }
  }
  {
    std::lock_guard<std::mutex> lock(context.open_connections_mu);
    for (const std::shared_ptr<ConnectionEntry>& entry : context.open_connections) {
      if (entry->thread.joinable()) entry->thread.join();
    }
    context.open_connections.clear();
  }
  static_cast<void>(listener.value().Close());

  result.requests_served = context.requests_served.load();
  result.connections = context.connections.load();
  result.rejected_handshakes = context.rejected_handshakes.load();
  result.protocol_errors = context.protocol_errors.load();
  result.fenced_requests = context.fenced_requests.load();
  result.committed = context.committed.load();
  const Status shutdown = coordinator.value()->Shutdown();
  result.clean_shutdown = shutdown.ok();
  if (context.log_file != nullptr) static_cast<void>(std::fclose(context.log_file));
  if (!shutdown.ok()) return shutdown;
  return result;
}

Result<TcpConnection> ConnectWorker(const WorkerOptions& options, WorkerResult* result) {
  WorkerResult local;
  WorkerResult* out = result != nullptr ? result : &local;
  *out = WorkerResult{};

  Status last_status = Status(StatusCode::kIoError, "connect was not attempted");
  TcpConnection connection;
  for (std::uint64_t attempt = 0; attempt < std::max<std::uint64_t>(1, options.connect_attempts);
       ++attempt) {
    Result<TcpConnection> candidate =
        TcpConnection::Connect(options.address, options.port, options.max_frame_bytes);
    if (candidate.ok()) {
      connection = std::move(candidate.value());
      break;
    }
    last_status = candidate.status();
    std::this_thread::sleep_for(std::chrono::milliseconds(options.connect_retry_millis));
  }
  if (!connection.valid()) return last_status;

  HelloPayload hello;
  hello.protocol_version = kWireProtocolVersion;
  hello.incarnation = IncarnationId{static_cast<std::uint64_t>(CurrentProcessId())};
  hello.epoch = options.epoch;
  hello.worker = options.worker;
  hello.max_frame_bytes = options.max_frame_bytes;
  hello.build = std::string(kProjectName) + " " + std::string(kVersionString);
  Frame frame;
  frame.kind = FrameKind::kHello;
  frame.sequence = 1;
  ByteWriter writer;
  EncodeHello(hello, writer);
  frame.body = writer.data();
  const Status sent = connection.SendFrame(frame);
  if (!sent.ok()) return sent;

  Result<Frame> reply = connection.ReceiveFrame();
  if (!reply.ok()) return reply.status();
  if (reply.value().kind != FrameKind::kHelloAck) {
    return Status(StatusCode::kWireMalformed, "expected HELLO_ACK");
  }
  HelloAckPayload ack;
  ByteReader reader(reply.value().body);
  const Status decoded = DecodeHelloAck(reader, &ack);
  if (!decoded.ok()) return decoded;
  out->handshake_accepted = ack.accepted;
  out->handshake_code = ack.code;
  out->handshake_message = ack.message;
  out->coordinator = ack.coordinator;
  out->coordinator_epoch = ack.epoch;
  connection.SetMaxFrameBytes(std::min<std::uint32_t>(options.max_frame_bytes, ack.max_frame_bytes));
  if (!ack.accepted) {
    return Status(ack.code == StatusCode::kOk ? StatusCode::kProtocolMismatch : ack.code,
                  ack.message.empty() ? "handshake rejected" : ack.message);
  }
  return connection;
}

}  // namespace flowplace

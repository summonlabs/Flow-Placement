// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// flowplace command line interface.
//
// Exit codes:
//   0  success (for place: the flow was placed, possibly degraded)
//   1  usage, I/O, or protocol error
//   2  the engine returned a non-placed outcome (rejected, deferred, stale)
//   3  the worker handshake was rejected (protocol or epoch mismatch)

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "flowplace/bench.hpp"
#include "flowplace/engine.hpp"
#include "flowplace/process.hpp"
#include "flowplace/runtime.hpp"
#include "flowplace/scenario.hpp"
#include "flowplace/server.hpp"
#include "flowplace/store.hpp"
#include "flowplace/transport.hpp"
#include "flowplace/version.hpp"
#include "flowplace/wire.hpp"

namespace {

using namespace flowplace;

void PrintUsage() {
  std::fputs(
      "flowplace 1.0.0\n"
      "Deterministic, generation-bound placement of admitted flows onto authorized candidate paths.\n"
      "\n"
      "Usage:\n"
      "  flowplace version\n"
      "  flowplace validate --scenario FILE\n"
      "  flowplace place    --scenario FILE [--store FILE] [--no-commit]\n"
      "  flowplace explain  --scenario FILE [--store FILE] [--no-commit]\n"
      "  flowplace roundtrip --scenario FILE\n"
      "  flowplace bench    [--fanout N] [--iterations N] [--resources N] [--domains N]\n"
      "                     [--churn N] [--objectives N] [--seed N]\n"
      "  flowplace serve    --store FILE [--port N] [--address A] [--workers N] [--epoch N]\n"
      "                     [--max-requests N] [--ready-file FILE] [--log-file FILE]\n"
      "  flowplace worker   --scenario FILE [--port N] [--address A] [--repeat N] [--epoch N]\n"
      "  flowplace store-inspect --store FILE\n"
      "  flowplace store-compact --store FILE\n"
      "\n"
      "Every command accepts --report FILE, which redirects the report to FILE.\n",
      stdout);
}

struct Arguments {
  std::vector<std::string> positional;
  std::vector<std::pair<std::string, std::string>> options;

  [[nodiscard]] bool Has(const std::string& name) const {
    for (const auto& option : options) {
      if (option.first == name) return true;
    }
    return false;
  }

  [[nodiscard]] std::string Get(const std::string& name, const std::string& fallback) const {
    for (const auto& option : options) {
      if (option.first == name) return option.second;
    }
    return fallback;
  }

  [[nodiscard]] std::uint64_t GetU64(const std::string& name, std::uint64_t fallback,
                                     bool* ok) const {
    for (const auto& option : options) {
      if (option.first != name) continue;
      std::uint64_t value = 0;
      const char* first = option.second.data();
      const char* last = first + option.second.size();
      const std::from_chars_result result = std::from_chars(first, last, value);
      *ok = result.ec == std::errc() && result.ptr == last;
      return value;
    }
    *ok = true;
    return fallback;
  }
};

void ParseArguments(int argc, char** argv, Arguments* out) {
  for (int i = 0; i < argc; ++i) {
    const std::string token = argv[i];
    if (token.size() >= 2 && token[0] == '-' && token[1] == '-') {
      const std::string name = token.substr(2);
      const bool has_value = i + 1 < argc && (argv[i + 1][0] != '-' || argv[i + 1][1] == 0);
      if (has_value) {
        out->options.emplace_back(name, argv[i + 1]);
        ++i;
      } else {
        out->options.emplace_back(name, "true");
      }
    } else {
      out->positional.push_back(token);
    }
  }
}

Result<std::string> ReadFile(const std::string& path, std::uint64_t max_bytes) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return Status(StatusCode::kIoError, "cannot open " + path);
  }
  std::string text;
  // The read buffer lives on the heap: a large buffer on the stack is both a
  // static-analysis finding and a poor fit for a deep call chain.
  std::vector<char> buffer(64 * 1024);
  for (;;) {
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
    if (read != 0) text.append(buffer.data(), read);
    if (text.size() > max_bytes) {
      std::fclose(file);
      return Status(StatusCode::kOversizeRequest, path + " exceeds the maximum scenario size");
    }
    if (read < buffer.size()) break;
  }
  std::fclose(file);
  return text;
}

Result<PlacementRequest> LoadScenario(const Arguments& args, std::string* path_out) {
  const std::string path = args.Get("scenario", "");
  if (path.empty()) return Status(StatusCode::kInvalidArgument, "--scenario FILE is required");
  *path_out = path;
  Result<std::string> text = ReadFile(path, kMaxScenarioBytes);
  if (!text.ok()) return text.status();
  return ParseScenario(text.value(), Limits{});
}

int RunPlace(const Arguments& args, bool explain) {
  std::string path;
  Result<PlacementRequest> request = LoadScenario(args, &path);
  if (!request.ok()) {
    std::fprintf(stderr, "scenario error: %s\n", request.status().ToString().c_str());
    return 1;
  }
  PlacementRequest value = std::move(request.value());
  if (!args.Has("no-commit") && args.Has("store")) {
    CoordinatorOptions options;
    options.store_path = args.Get("store", "");
    // A one-shot placement adopts the fabric epoch declared by the scenario:
    // this process has no prior epoch history to protect, and the request
    // states which epoch its evidence is bound to.
    if (value.expected.fabric_epoch.valid()) {
      options.initial_epoch = value.expected.fabric_epoch;
    }
    Result<std::unique_ptr<PlacementCoordinator>> coordinator =
        PlacementCoordinator::Start(options);
    if (!coordinator.ok()) {
      std::fprintf(stderr, "coordinator error: %s\n", coordinator.status().ToString().c_str());
      return 1;
    }
    const AttemptId attempt = value.attempt.valid() ? value.attempt : AttemptId{1};
    Result<AttemptOutcome> outcome = coordinator.value()->PlaceNow(value, attempt);
    if (!outcome.ok()) {
      std::fprintf(stderr, "placement error: %s\n", outcome.status().ToString().c_str());
      return 1;
    }
    std::fputs(RenderDecision(outcome.value().decision).c_str(), stdout);
    std::printf("attempt=%llu state=%s code=%s placement=%llu generation=%llu\n",
                static_cast<unsigned long long>(attempt.value()),
                std::string(AttemptStateName(outcome.value().state)).c_str(),
                std::string(StatusCodeName(outcome.value().code)).c_str(),
                static_cast<unsigned long long>(outcome.value().placement_id.value()),
                static_cast<unsigned long long>(outcome.value().placement_generation.value()));
    const bool placed = outcome.value().state == AttemptState::kCommitted;
    const Status shutdown = coordinator.value()->Shutdown();
    if (!shutdown.ok()) {
      std::fprintf(stderr, "shutdown error: %s\n", shutdown.ToString().c_str());
      return 1;
    }
    return placed ? 0 : 2;
  }
  const PlacementEngine engine;
  const PlacementDecision decision = engine.Place(value);
  std::fputs(RenderDecision(decision).c_str(), stdout);
  if (explain) {
    std::printf("request_digest=%s\n", RequestDigest(value, Limits{}).ToHex().c_str());
  }
  return decision.placed() ? 0 : 2;
}

int RunValidate(const Arguments& args) {
  std::string path;
  Result<PlacementRequest> request = LoadScenario(args, &path);
  if (!request.ok()) {
    std::printf("invalid: %s\n", request.status().ToString().c_str());
    return 1;
  }
  const Status status = ValidateRequest(request.value(), Limits{});
  if (!status.ok()) {
    std::printf("invalid: %s\n", status.ToString().c_str());
    return 1;
  }
  std::printf("valid: %s\n", path.c_str());
  std::printf("request_digest=%s\n", RequestDigest(request.value(), Limits{}).ToHex().c_str());
  return 0;
}

int RunRoundtrip(const Arguments& args) {
  std::string path;
  Result<PlacementRequest> request = LoadScenario(args, &path);
  if (!request.ok()) {
    std::fprintf(stderr, "scenario error: %s\n", request.status().ToString().c_str());
    return 1;
  }
  const std::string first = WriteScenario(request.value());
  Result<PlacementRequest> reparsed = ParseScenario(first, Limits{});
  if (!reparsed.ok()) {
    std::fprintf(stderr, "roundtrip parse error: %s\n", reparsed.status().ToString().c_str());
    return 1;
  }
  const Digest before = RequestDigest(request.value(), Limits{});
  const Digest after = RequestDigest(reparsed.value(), Limits{});
  if (before != after) {
    std::fprintf(stderr, "roundtrip digest mismatch: %s != %s\n", before.ToHex().c_str(),
                 after.ToHex().c_str());
    return 1;
  }
  if (WriteScenario(reparsed.value()) != first) {
    std::fprintf(stderr, "roundtrip rendering is not canonical\n");
    return 1;
  }
  std::printf("roundtrip ok: digest=%s\n", before.ToHex().c_str());
  return 0;
}

int RunBench(const Arguments& args) {
  BenchConfig config;
  bool ok = true;
  config.fanout = args.GetU64("fanout", config.fanout, &ok);
  config.iterations = args.GetU64("iterations", config.iterations, &ok);
  config.resources = args.GetU64("resources", config.resources, &ok);
  config.domains = args.GetU64("domains", config.domains, &ok);
  config.churn_percent = args.GetU64("churn", config.churn_percent, &ok);
  config.objectives = args.GetU64("objectives", config.objectives, &ok);
  config.seed = args.GetU64("seed", config.seed, &ok);
  if (!ok) {
    std::fprintf(stderr, "bench: numeric option could not be parsed\n");
    return 1;
  }
  const BenchResult result = RunSyntheticPlacementBenchmark(config);
  std::fputs(RenderBenchResult(result).c_str(), stdout);
  return 0;
}

int RunServe(const Arguments& args) {
  ServerOptions options;
  bool ok = true;
  const std::uint64_t port = args.GetU64("port", 0, &ok);
  options.port = static_cast<std::uint16_t>(port);
  const std::uint64_t workers = args.GetU64("workers", 4, &ok);
  options.worker_threads = static_cast<std::uint32_t>(workers);
  options.queue_capacity = args.GetU64("queue", 1024, &ok);
  const std::uint64_t epoch = args.GetU64("epoch", 1, &ok);
  options.epoch = FabricEpoch{epoch};
  options.max_requests = args.GetU64("max-requests", 0, &ok);
  const std::uint64_t connections = args.GetU64("max-connections", 16, &ok);
  options.max_connections = static_cast<std::uint32_t>(connections);
  options.store_path = args.Get("store", "");
  options.ready_file = args.Get("ready-file", "");
  options.log_file = args.Get("log-file", "");
  options.address = args.Get("address", "127.0.0.1");
  if (!ok || port > 65535) {
    std::fprintf(stderr, "serve: option could not be parsed\n");
    return 1;
  }
  Result<ServerResult> result = RunServer(options);
  if (!result.ok()) {
    std::fprintf(stderr, "serve error: %s\n", result.status().ToString().c_str());
    return 1;
  }
  std::printf("serve: port=%u requests=%llu connections=%llu committed=%llu fenced=%llu "
              "rejected_handshakes=%llu protocol_errors=%llu clean_shutdown=%s\n",
              static_cast<unsigned>(result.value().port),
              static_cast<unsigned long long>(result.value().requests_served),
              static_cast<unsigned long long>(result.value().connections),
              static_cast<unsigned long long>(result.value().committed),
              static_cast<unsigned long long>(result.value().fenced_requests),
              static_cast<unsigned long long>(result.value().rejected_handshakes),
              static_cast<unsigned long long>(result.value().protocol_errors),
              result.value().clean_shutdown ? "true" : "false");
  return result.value().clean_shutdown ? 0 : 1;
}

int RunWorker(const Arguments& args) {
  std::string path;
  Result<PlacementRequest> request = LoadScenario(args, &path);
  if (!request.ok()) {
    std::fprintf(stderr, "scenario error: %s\n", request.status().ToString().c_str());
    return 1;
  }
  bool ok = true;
  WorkerOptions options;
  const std::uint64_t port = args.GetU64("port", 0, &ok);
  options.port = static_cast<std::uint16_t>(port);
  const std::uint64_t epoch = args.GetU64("epoch", 1, &ok);
  options.epoch = FabricEpoch{epoch};
  const std::uint64_t repeat = args.GetU64("repeat", 1, &ok);
  options.worker = WorkerId{args.GetU64("worker", 1, &ok)};
  options.address = args.Get("address", "127.0.0.1");
  const std::uint64_t expect_committed = args.GetU64("expect-committed", UINT64_MAX, &ok);
  if (!ok || port > 65535) {
    std::fprintf(stderr, "worker: option could not be parsed\n");
    return 1;
  }
  WorkerResult handshake;
  Result<TcpConnection> connection = ConnectWorker(options, &handshake);
  if (!connection.ok()) {
    std::printf("worker: handshake rejected code=%s message=%s\n",
                std::string(StatusCodeName(handshake.handshake_code)).c_str(),
                handshake.handshake_message.c_str());
    return 3;
  }
  // Bind every request to the epoch the coordinator declared during the
  // handshake: that is the epoch this worker's work belongs to.
  const FabricEpoch bound_epoch =
      handshake.coordinator_epoch.valid() ? handshake.coordinator_epoch : options.epoch;
  std::uint64_t sequence = 2;
  std::uint64_t committed = 0;
  std::uint64_t rejected = 0;
  std::uint64_t fenced = 0;
  std::uint64_t other = 0;
  for (std::uint64_t i = 0; i < std::max<std::uint64_t>(1, repeat); ++i) {
    PlaceRequestPayload payload;
    payload.epoch = bound_epoch;
    payload.request = request.value();
    payload.request.expected.fabric_epoch = bound_epoch;
    payload.attempt = AttemptId{1000000 + options.worker.value() * 100000 + i};
    payload.request.attempt = payload.attempt;
    Frame frame;
    frame.kind = FrameKind::kPlaceRequest;
    frame.sequence = ++sequence;
    ByteWriter writer;
    EncodePlaceRequest(payload, writer);
    frame.body = writer.data();
    const Status sent = connection.value().SendFrame(frame);
    if (!sent.ok()) {
      std::fprintf(stderr, "worker: send failed: %s\n", sent.ToString().c_str());
      return 1;
    }
    Result<Frame> reply = connection.value().ReceiveFrame();
    if (!reply.ok()) {
      std::fprintf(stderr, "worker: receive failed: %s\n", reply.status().ToString().c_str());
      return 1;
    }
    if (reply.value().kind == FrameKind::kError) {
      ErrorPayload error;
      ByteReader reader(reply.value().body);
      const Status decoded = DecodeErrorPayload(reader, &error);
      if (!decoded.ok()) {
        std::fprintf(stderr, "worker: malformed error frame\n");
        return 1;
      }
      if (error.code == StatusCode::kFencedStaleEpoch) {
        ++fenced;
      } else {
        ++other;
      }
      continue;
    }
    if (reply.value().kind != FrameKind::kPlaceResult) {
      std::fprintf(stderr, "worker: unexpected frame kind %s\n",
                   std::string(FrameKindName(reply.value().kind)).c_str());
      return 1;
    }
    PlaceResultPayload result;
    ByteReader reader(reply.value().body);
    const Status decoded = DecodePlaceResult(reader, Limits{}, &result);
    if (!decoded.ok()) {
      std::fprintf(stderr, "worker: malformed result frame: %s\n", decoded.ToString().c_str());
      return 1;
    }
    if (result.state == AttemptState::kCommitted) {
      ++committed;
    } else if (result.state == AttemptState::kFenced) {
      ++fenced;
    } else {
      ++rejected;
    }
  }
  std::printf("worker: committed=%llu rejected=%llu fenced=%llu other=%llu coordinator=%llu "
              "epoch=%llu\n",
              static_cast<unsigned long long>(committed),
              static_cast<unsigned long long>(rejected),
              static_cast<unsigned long long>(fenced),
              static_cast<unsigned long long>(other),
              static_cast<unsigned long long>(handshake.coordinator.value()),
              static_cast<unsigned long long>(handshake.coordinator_epoch.value()));
  const Status closed = connection.value().Close();
  if (!closed.ok()) {
    std::fprintf(stderr, "worker: close failed: %s\n", closed.ToString().c_str());
    return 1;
  }
  if (expect_committed != UINT64_MAX && committed != expect_committed) {
    std::fprintf(stderr, "worker: expected %llu committed placements but committed %llu\n",
                 static_cast<unsigned long long>(expect_committed),
                 static_cast<unsigned long long>(committed));
    return 2;
  }
  return 0;
}

int RunStoreInspect(const Arguments& args) {
  const std::string path = args.Get("store", "");
  if (path.empty()) {
    std::fprintf(stderr, "store-inspect: --store FILE is required\n");
    return 1;
  }
  Result<PlacementStore> store = PlacementStore::Open(path, StoreOptions{});
  if (!store.ok()) {
    std::fprintf(stderr, "store-inspect: %s\n", store.status().ToString().c_str());
    return 1;
  }
  const RecoveryReport& report = store.value().recovery();
  std::printf("store=%s\n", path.c_str());
  std::printf("liveness_restored=%s truncated_tail=%s snapshot_loaded=%s bytes_discarded=%llu\n",
              report.liveness_restored ? "true" : "false",
              report.truncated_tail ? "true" : "false",
              report.snapshot_loaded ? "true" : "false",
              static_cast<unsigned long long>(report.bytes_discarded));
  std::printf("records=%llu placements=%llu attempts=%llu committed=%llu rejected=%llu "
              "cancelled=%llu fenced=%llu unfinished=%llu superseded=%llu "
              "requiring_revalidation=%llu retired=%llu max_sequence=%llu\n",
              static_cast<unsigned long long>(report.records_valid),
              static_cast<unsigned long long>(report.placements_total),
              static_cast<unsigned long long>(report.attempts_total),
              static_cast<unsigned long long>(report.committed_attempts),
              static_cast<unsigned long long>(report.rejected_attempts),
              static_cast<unsigned long long>(report.cancelled_attempts),
              static_cast<unsigned long long>(report.fenced_attempts),
              static_cast<unsigned long long>(report.unfinished_attempts),
              static_cast<unsigned long long>(report.superseded_placements),
              static_cast<unsigned long long>(report.placements_requiring_revalidation),
              static_cast<unsigned long long>(report.retired_attempts),
              static_cast<unsigned long long>(report.max_sequence));
  std::printf("last_epoch=%llu last_incarnation=%llu\n",
              static_cast<unsigned long long>(report.last_epoch.value()),
              static_cast<unsigned long long>(report.last_incarnation.value()));
  for (const PlacementRecord& record : report.latest_by_flow) {
    std::printf("placement flow=%llu generation=%llu path=%llu pathauth=%llu placement=%llu "
                "epoch=%llu attempt=%llu requires_revalidation=%s\n",
                static_cast<unsigned long long>(record.intent.flow.value()),
                static_cast<unsigned long long>(record.generation.value()),
                static_cast<unsigned long long>(record.intent.path.value()),
                static_cast<unsigned long long>(record.intent.path_authority.value()),
                static_cast<unsigned long long>(record.id.value()),
                static_cast<unsigned long long>(record.commit_epoch.value()),
                static_cast<unsigned long long>(record.intent.attempt.value()),
                record.requires_revalidation ? "true" : "false");
  }
  return store.value().Close().ok() ? 0 : 1;
}

int RunStoreCompact(const Arguments& args) {
  const std::string path = args.Get("store", "");
  if (path.empty()) {
    std::fprintf(stderr, "store-compact: --store FILE is required\n");
    return 1;
  }
  Result<PlacementStore> store = PlacementStore::Open(path, StoreOptions{});
  if (!store.ok()) {
    std::fprintf(stderr, "store-compact: %s\n", store.status().ToString().c_str());
    return 1;
  }
  const Status status = store.value().Compact();
  if (!status.ok()) {
    std::fprintf(stderr, "store-compact: %s\n", status.ToString().c_str());
    return 1;
  }
  const StoreStats stats = store.value().stats();
  std::printf("store-compact: journal_bytes=%llu snapshot_bytes=%llu compactions=%llu\n",
              static_cast<unsigned long long>(stats.journal_bytes),
              static_cast<unsigned long long>(stats.snapshot_bytes),
              static_cast<unsigned long long>(stats.compactions));
  return store.value().Close().ok() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 1;
  }
  const Status sockets = InitializeSockets();
  if (!sockets.ok()) {
    std::fprintf(stderr, "%s\n", sockets.ToString().c_str());
    return 1;
  }
  const std::string command = argv[1];
  Arguments args;
  ParseArguments(argc - 2, argv + 2, &args);
  // --report FILE redirects this process's standard output to FILE so that the
  // full decision, explanation, benchmark, or recovery report can be consumed
  // by an automated caller without a pipe.
  const std::string report_path = args.Get("report", "");
  if (!report_path.empty() && std::freopen(report_path.c_str(), "wb", stdout) == nullptr) {
    std::fprintf(stderr, "cannot open report file %s\n", report_path.c_str());
    return 1;
  }
  int exit_code = 1;
  if (command == "version" || command == "--version") {
    std::printf("%s %s\n", std::string(kProjectName).c_str(), std::string(kVersionString).c_str());
    exit_code = 0;
  } else if (command == "help" || command == "--help") {
    PrintUsage();
    exit_code = 0;
  } else if (command == "validate") {
    exit_code = RunValidate(args);
  } else if (command == "place") {
    exit_code = RunPlace(args, false);
  } else if (command == "explain") {
    exit_code = RunPlace(args, true);
  } else if (command == "roundtrip") {
    exit_code = RunRoundtrip(args);
  } else if (command == "bench") {
    exit_code = RunBench(args);
  } else if (command == "serve") {
    exit_code = RunServe(args);
  } else if (command == "worker") {
    exit_code = RunWorker(args);
  } else if (command == "store-inspect") {
    exit_code = RunStoreInspect(args);
  } else if (command == "store-compact") {
    exit_code = RunStoreCompact(args);
  } else {
    std::fprintf(stderr, "unknown command '%s'\n", command.c_str());
    PrintUsage();
    exit_code = 1;
  }
  static_cast<void>(ShutdownSockets());
  return exit_code;
}

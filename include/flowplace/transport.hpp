// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Real framed transport over TCP sockets. This is the transport used by the
// multiprocess validation: a coordinator process listens, worker processes
// connect, and every request and result crosses a real socket boundary as a
// bounded, integrity-checked frame.

#ifndef FLOWPLACE_TRANSPORT_HPP
#define FLOWPLACE_TRANSPORT_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "flowplace/status.hpp"
#include "flowplace/wire.hpp"

namespace flowplace {

class TcpConnection {
 public:
  TcpConnection() = default;
  ~TcpConnection();
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;
  TcpConnection(TcpConnection&& other) noexcept;
  TcpConnection& operator=(TcpConnection&& other) noexcept;

  static Result<TcpConnection> Connect(const std::string& host, std::uint16_t port,
                                       std::uint32_t max_frame_bytes = kDefaultMaxFrameBytes);

  [[nodiscard]] bool valid() const noexcept { return socket_ >= 0; }
  Status SendFrame(const Frame& frame);
  Result<Frame> ReceiveFrame();
  Status SetNoDelay(bool enable);
  Status Close();
  [[nodiscard]] std::uint32_t max_frame_bytes() const noexcept { return max_frame_bytes_; }
  // Applies the negotiated bound to both directions: outbound frames are
  // refused and inbound frames are rejected before they are buffered.
  void SetMaxFrameBytes(std::uint32_t v) noexcept {
    max_frame_bytes_ = v;
    stream_.SetMaxFrameBytes(v);
  }

 private:
  friend class TcpListener;
  std::intptr_t socket_ = -1;
  FrameStream stream_;
  std::uint32_t max_frame_bytes_ = kDefaultMaxFrameBytes;
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;

  // Bind and listen on |address| (IPv4 dotted quad) and |port| (0 = ephemeral).
  static Result<TcpListener> Bind(const std::string& address, std::uint16_t port);
  // Waits up to |timeout_millis| for an inbound connection. |ready| reports
  // whether a subsequent Accept() will not block.
  Status WaitReadable(std::int64_t timeout_millis, bool* ready);
  Result<TcpConnection> Accept();
  Status Close();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool valid() const noexcept { return socket_ >= 0; }

 private:
  std::intptr_t socket_ = -1;
  std::uint16_t port_ = 0;
};

// Process-wide socket subsystem lifetime (Winsock on Windows, no-op on POSIX).
Status InitializeSockets();
Status ShutdownSockets();

// Returns the last socket error as a Status.
Status LastSocketError(std::string_view what);

}  // namespace flowplace

#endif  // FLOWPLACE_TRANSPORT_HPP

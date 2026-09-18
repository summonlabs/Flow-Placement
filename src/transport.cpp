// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "flowplace/transport.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
// The Windows SDK header winsock2/ws2tcpip is annotated for static analysis in a
// way that reports an uninitialized out-parameter inside the system header
// itself (C6101 at ws2tcpip.h). The suppression is scoped to those includes.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 6101)
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace flowplace {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

std::once_flag g_socket_once;
int g_socket_init_result = 0;

void InitializeSocketsOnce() {
  std::call_once(g_socket_once, []() {
#ifdef _WIN32
    WSADATA data;
    g_socket_init_result = WSAStartup(MAKEWORD(2, 2), &data);
#else
    g_socket_init_result = 0;
#endif
  });
}

int CloseSocket(NativeSocket socket) {
#ifdef _WIN32
  return closesocket(socket);
#else
  return ::close(socket);
#endif
}

}  // namespace

Status InitializeSockets() {
  InitializeSocketsOnce();
  if (g_socket_init_result != 0) {
    return Status(StatusCode::kSocketError, "socket subsystem initialization failed");
  }
  return Status::Ok();
}

Status ShutdownSockets() {
#ifdef _WIN32
  static std::once_flag shutdown_once;
  std::call_once(shutdown_once, []() { WSACleanup(); });
#endif
  return Status::Ok();
}

Status LastSocketError(std::string_view what) {
#ifdef _WIN32
  const int code = WSAGetLastError();
  return Status(StatusCode::kSocketError, std::string(what) + " failed with Winsock error " +
                                              std::to_string(code));
#else
  const int code = errno;
  return Status(StatusCode::kSocketError,
                std::string(what) + " failed: " + std::string(std::strerror(code)));
#endif
}

TcpConnection::~TcpConnection() { static_cast<void>(Close()); }

TcpConnection::TcpConnection(TcpConnection&& other) noexcept
    : socket_(other.socket_), stream_(other.max_frame_bytes_), max_frame_bytes_(other.max_frame_bytes_) {
  other.socket_ = -1;
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
  if (this == &other) return *this;
  static_cast<void>(Close());
  socket_ = other.socket_;
  max_frame_bytes_ = other.max_frame_bytes_;
  stream_.SetMaxFrameBytes(max_frame_bytes_);
  other.socket_ = -1;
  return *this;
}

Status TcpConnection::Close() {
  if (socket_ < 0) return Status::Ok();
  const NativeSocket socket = static_cast<NativeSocket>(socket_);
  socket_ = -1;
#ifdef _WIN32
  static_cast<void>(shutdown(socket, SD_BOTH));
#else
  static_cast<void>(shutdown(socket, SHUT_RDWR));
#endif
  CloseSocket(socket);
  return Status::Ok();
}

Result<TcpConnection> TcpConnection::Connect(const std::string& host, std::uint16_t port,
                                             std::uint32_t max_frame_bytes) {
  InitializeSocketsOnce();
  if (g_socket_init_result != 0) {
    return Status(StatusCode::kSocketError, "socket subsystem is not available");
  }
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) return LastSocketError("socket");
  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    CloseSocket(socket);
    return Status(StatusCode::kInvalidArgument, "connect host must be an IPv4 address");
  }
  if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const Status status = LastSocketError("connect");
    CloseSocket(socket);
    return status;
  }
  int one = 1;
  static_cast<void>(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                               reinterpret_cast<const char*>(&one), sizeof(one)));
  TcpConnection connection;
  connection.socket_ = static_cast<std::intptr_t>(socket);
  connection.max_frame_bytes_ = max_frame_bytes;
  connection.stream_.SetMaxFrameBytes(max_frame_bytes);
  return connection;
}

Status TcpConnection::SendFrame(const Frame& frame) {
  if (socket_ < 0) return Status(StatusCode::kConnectionClosed, "connection is closed");
  std::vector<std::uint8_t> encoded;
  const Status status = EncodeFrame(frame, &encoded, max_frame_bytes_);
  if (!status.ok()) return status;
  const NativeSocket socket = static_cast<NativeSocket>(socket_);
  std::size_t sent = 0;
  while (sent < encoded.size()) {
#ifdef _WIN32
    const int chunk = ::send(socket, reinterpret_cast<const char*>(encoded.data() + sent),
                             static_cast<int>(encoded.size() - sent), 0);
#else
    const ssize_t chunk = ::send(socket, encoded.data() + sent, encoded.size() - sent,
                                 MSG_NOSIGNAL);
#endif
    if (chunk <= 0) return LastSocketError("send");
    sent += static_cast<std::size_t>(chunk);
  }
  return Status::Ok();
}

Result<Frame> TcpConnection::ReceiveFrame() {
  if (socket_ < 0) return Status(StatusCode::kConnectionClosed, "connection is closed");
  const NativeSocket socket = static_cast<NativeSocket>(socket_);
  std::vector<std::uint8_t> buffer(64 * 1024);
  for (;;) {
    std::optional<Frame> frame;
    const Status status = stream_.Next(&frame);
    if (!status.ok()) return status;
    if (frame.has_value()) return *frame;
#ifdef _WIN32
    const int received = ::recv(socket, reinterpret_cast<char*>(buffer.data()),
                                static_cast<int>(buffer.size()), 0);
#else
    const ssize_t received = ::recv(socket, buffer.data(), buffer.size(), 0);
#endif
    if (received == 0) {
      return Status(StatusCode::kConnectionClosed, "peer closed the connection");
    }
    if (received < 0) return LastSocketError("recv");
    const Status feed = stream_.Feed(buffer.data(), static_cast<std::size_t>(received));
    if (!feed.ok()) return feed;
  }
}

Status TcpConnection::SetNoDelay(bool enable) {
  if (socket_ < 0) return Status(StatusCode::kConnectionClosed, "connection is closed");
  const int value = enable ? 1 : 0;
  if (setsockopt(static_cast<NativeSocket>(socket_), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return LastSocketError("setsockopt(TCP_NODELAY)");
  }
  return Status::Ok();
}

TcpListener::~TcpListener() { static_cast<void>(Close()); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : socket_(other.socket_), port_(other.port_) {
  other.socket_ = -1;
  other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this == &other) return *this;
  static_cast<void>(Close());
  socket_ = other.socket_;
  port_ = other.port_;
  other.socket_ = -1;
  other.port_ = 0;
  return *this;
}

Status TcpListener::Close() {
  if (socket_ < 0) return Status::Ok();
  const NativeSocket socket = static_cast<NativeSocket>(socket_);
  socket_ = -1;
  CloseSocket(socket);
  return Status::Ok();
}

Result<TcpListener> TcpListener::Bind(const std::string& address, std::uint16_t port) {
  InitializeSocketsOnce();
  if (g_socket_init_result != 0) {
    return Status(StatusCode::kSocketError, "socket subsystem is not available");
  }
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) return LastSocketError("socket");
  int one = 1;
  static_cast<void>(setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                               reinterpret_cast<const char*>(&one), sizeof(one)));
  sockaddr_in local;
  std::memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_port = htons(port);
  if (inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1) {
    CloseSocket(socket);
    return Status(StatusCode::kInvalidArgument, "listen address must be an IPv4 address");
  }
  if (::bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    const Status status = LastSocketError("bind");
    CloseSocket(socket);
    return status;
  }
  if (::listen(socket, 16) != 0) {
    const Status status = LastSocketError("listen");
    CloseSocket(socket);
    return status;
  }
  sockaddr_in bound;
  std::memset(&bound, 0, sizeof(bound));
#ifdef _WIN32
  int bound_length = sizeof(bound);
#else
  socklen_t bound_length = sizeof(bound);
#endif
  if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    const Status status = LastSocketError("getsockname");
    CloseSocket(socket);
    return status;
  }
  TcpListener listener;
  listener.socket_ = static_cast<std::intptr_t>(socket);
  listener.port_ = ntohs(bound.sin_port);
  return listener;
}

Status TcpListener::WaitReadable(std::int64_t timeout_millis, bool* ready) {
  if (ready == nullptr) return Status(StatusCode::kInvalidArgument, "output flag is null");
  *ready = false;
  if (socket_ < 0) return Status(StatusCode::kNotRunning, "listener is closed");
  fd_set read_set;
  FD_ZERO(&read_set);
  const NativeSocket socket = static_cast<NativeSocket>(socket_);
  FD_SET(socket, &read_set);
  timeval timeout;
  timeout.tv_sec = static_cast<long>(timeout_millis / 1000);
  timeout.tv_usec = static_cast<long>((timeout_millis % 1000) * 1000);
  const int result = ::select(static_cast<int>(socket) + 1, &read_set, nullptr, nullptr, &timeout);
  if (result < 0) return LastSocketError("select");
  *ready = result > 0;
  return Status::Ok();
}

Result<TcpConnection> TcpListener::Accept() {
  if (socket_ < 0) return Status(StatusCode::kNotRunning, "listener is closed");
  sockaddr_in peer;
  std::memset(&peer, 0, sizeof(peer));
#ifdef _WIN32
  int peer_length = sizeof(peer);
#else
  socklen_t peer_length = sizeof(peer);
#endif
  const NativeSocket accepted =
      ::accept(static_cast<NativeSocket>(socket_), reinterpret_cast<sockaddr*>(&peer), &peer_length);
  if (accepted == kInvalidSocket) return LastSocketError("accept");
  int one = 1;
  static_cast<void>(setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                               reinterpret_cast<const char*>(&one), sizeof(one)));
  TcpConnection connection;
  connection.socket_ = static_cast<std::intptr_t>(accepted);
  connection.max_frame_bytes_ = kDefaultMaxFrameBytes;
  connection.stream_.SetMaxFrameBytes(kDefaultMaxFrameBytes);
  return connection;
}

}  // namespace flowplace

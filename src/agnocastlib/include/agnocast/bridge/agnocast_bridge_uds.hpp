#pragma once

// Helpers for the abstract-namespace UDS transport that replaces the POSIX
// message-queue path Agnocast publishers/subscribers/services and the
// discovery agent used to talk to the bridge_manager over.
//
// Wire model: SOCK_DGRAM, one fixed-size datagram per sendto() call. The
// sender opens a datagram socket and sendto()s the payload to the
// bridge_manager's abstract-namespace address; the bridge_manager binds the
// same address and recv()s one datagram at a time. No connect() / accept()
// handshake, no per-message framing — every datagram is delivered atomically
// by the kernel or not at all.
//
// Reliability: UNIX-domain datagram sockets are reliable and in-order on
// Linux, so dropping POSIX MQ for DGRAM UDS does not weaken delivery
// guarantees. The system-wide `fs.mqueue.*` quotas disappear; the only knobs
// that matter now are `net.core.{wmem,rmem}_{default,max}`, which already
// have headroom (~200 KB by default) for our 524-byte messages.
//
// Resource cleanup: abstract-namespace addresses are released by the kernel
// as soon as the last fd referencing them is closed, so the original
// motivation for the rework (stale `/dev/mqueue/agnocast_bridge_manager@-1`
// after abnormal exits) is resolved by construction.
//
// Startup tradeoff: sendto() returns ECONNREFUSED until the bridge_manager
// binds the listener. The sender retries with the same budget (100 attempts
// at 100 ms spacing = 10 s) the old POSIX MQ EAGAIN loop used, so
// bridge_manager startup latency is the only behaviour difference visible to
// the application.

#include "agnocast/agnocast_mq.hpp"

#include <rclcpp/logger.hpp>
#include <rclcpp/logging.hpp>
#include <rclcpp/utilities.hpp>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

namespace agnocast
{

// Forward-declared so this free-standing helper can poll agnocast::ok()
// without pulling in node/agnocast_context.hpp (which would recreate the
// include cycle bridge_uds.hpp <- publisher.hpp <- context).
bool ok();

// Mirrors the historical POSIX MQ retry budget (100 attempts * 100ms = 10s).
// Triggers on (a) ECONNREFUSED while the bridge_manager has not yet bind()ed
// the listener, and (b) EAGAIN/ENOBUFS when the receiver's socket buffer is
// momentarily full.
inline constexpr int BRIDGE_UDS_SEND_MAX_RETRIES = 100;
inline constexpr useconds_t BRIDGE_UDS_SEND_RETRY_INTERVAL_US = 100000;
// Sliced abort poll granularity. Splitting the 100 ms back-off into ten 10 ms
// chunks bounds the worst-case shutdown latency to ~10 ms per outstanding
// sender without otherwise changing the retry cadence.
inline constexpr useconds_t BRIDGE_UDS_SEND_ABORT_POLL_INTERVAL_US = 10000;
static_assert(
  BRIDGE_UDS_SEND_RETRY_INTERVAL_US % BRIDGE_UDS_SEND_ABORT_POLL_INTERVAL_US == 0,
  "abort poll interval must divide the retry interval evenly");

namespace detail
{

// Fill `out` from an abstract-namespace address string (must start with '\0').
// Returns the socklen_t that bind()/sendto() expect (offsetof(sun_path) +
// address length, *without* a trailing NUL — abstract names are length-scoped).
inline socklen_t fill_abstract_sockaddr(const std::string & addr, sockaddr_un & out)
{
  out = {};
  out.sun_family = AF_UNIX;
  if (addr.empty() || addr[0] != '\0') {
    throw std::invalid_argument("abstract UDS address must start with NUL");
  }
  if (addr.size() > sizeof(out.sun_path)) {
    throw std::length_error("abstract UDS address too long for sun_path");
  }
  // Copy the leading NUL + the name bytes; abstract names are not C-strings.
  std::memcpy(out.sun_path, addr.data(), addr.size());
  return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + addr.size());
}

}  // namespace detail

// Create a non-blocking, CLOEXEC SOCK_DGRAM socket bound to `addr` (must be
// abstract, i.e. start with NUL). The returned fd is ready for epoll: it
// becomes readable whenever a queued datagram is available. Throws on failure.
inline int create_bridge_uds_listener(const std::string & addr)
{
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    throw std::system_error(errno, std::generic_category(), "bridge UDS socket() failed");
  }

  try {
    sockaddr_un sa{};
    const socklen_t alen = detail::fill_abstract_sockaddr(addr, sa);

    // SOCK_DGRAM has no listen()/accept() phase; bind() is enough to start
    // receiving datagrams once we add the fd to an epoll set.
    if (bind(fd, reinterpret_cast<sockaddr *>(&sa), alen) == -1) {
      throw std::system_error(errno, std::generic_category(), "bridge UDS bind() failed");
    }
  } catch (...) {
    close(fd);
    throw;
  }
  return fd;
}

// Send `msg` as a single datagram to `addr`. Returns true on success. The
// retry loop covers the window where the bridge_manager has not yet bound its
// listener (sendto == ECONNREFUSED) and the case where its receive buffer is
// momentarily full (EAGAIN / ENOBUFS), matching the budget the old POSIX MQ
// EAGAIN loop used.
//
// Shutdown handling: at entry, the function samples rclcpp::ok() and
// agnocast::ok() once each. Any lifecycle that is true at entry becomes a
// "watched" lifecycle; the retry loop then bails as soon as any watched
// lifecycle transitions to false. If both are already false at entry, the
// function returns false without attempting any sendto() — there is no
// caller whose lifecycle is still meant to observe the result.
//
// Why sample at entry instead of checking both unconditionally?
//   * A pure-rclcpp process never calls agnocast::init(), so agnocast::ok()
//     is permanently false. Treating that as "abort" would short-circuit
//     every send. Conversely a pure-agnocast process leaves rclcpp::ok()
//     undefined/false. Sampling once at entry means we only react to a
//     true->false transition of a lifecycle that was actually in use when
//     the call started, which is the SIGINT/SIGTERM behaviour we want.
//   * Mixed processes (both contexts initialised) get the strictest
//     behaviour: a shutdown on either context aborts the send.
//
// Abort polling cadence: the predicate is checked before each sendto() and
// every ~10 ms during the back-off sleep, bounding the worst-case
// SIGINT-to-return latency to ~10 ms even when the bridge_manager never
// comes up.
template <typename MsgT>
bool send_bridge_uds_message(
  const std::string & addr, const MsgT & msg, const rclcpp::Logger & logger)
{
  // Snapshot both lifecycles. Whatever was true at entry becomes a watched
  // lifecycle; a later transition to false aborts the retry loop.
  const bool watch_rclcpp = rclcpp::ok();
  const bool watch_agnocast = agnocast::ok();
  if (!watch_rclcpp && !watch_agnocast) {
    // No lifecycle is alive to observe the result; don't even open a socket.
    RCLCPP_WARN_ONCE(
      logger, "bridge UDS sendto() skipped: neither rclcpp nor agnocast context is initialised");
    return false;
  }
  // Returns true while every watched lifecycle is still up.
  const auto still_alive = [watch_rclcpp, watch_agnocast]() {
    if (watch_rclcpp && !rclcpp::ok()) {
      return false;
    }
    if (watch_agnocast && !agnocast::ok()) {
      return false;
    }
    return true;
  };

  // Non-blocking so a slow receiver cannot stall the publisher's calling
  // thread (e.g. a constructor on the application's hot path); the retry loop
  // below converts EAGAIN/EWOULDBLOCK into a bounded back-off.
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    RCLCPP_ERROR(logger, "bridge UDS socket() failed: %s (errno: %d)", strerror(errno), errno);
    return false;
  }

  sockaddr_un sa{};
  socklen_t alen = 0;
  try {
    alen = detail::fill_abstract_sockaddr(addr, sa);
  } catch (const std::exception & e) {
    RCLCPP_ERROR(logger, "bridge UDS address invalid: %s", e.what());
    close(fd);
    return false;
  }

  // Strip the leading NUL when logging so the abstract name is readable.
  const std::string display_name = (!addr.empty() && addr.front() == '\0') ? addr.substr(1) : addr;

  // Helper: usleep(BRIDGE_UDS_SEND_RETRY_INTERVAL_US) sliced into
  // BRIDGE_UDS_SEND_ABORT_POLL_INTERVAL_US chunks so a watched-lifecycle
  // shutdown can fire within ~10 ms of e.g. SIGINT. Returns true if the full
  // back-off elapsed without abort, false if a watched lifecycle went down.
  const auto interruptible_backoff = [&still_alive]() {
    constexpr int slices =
      static_cast<int>(BRIDGE_UDS_SEND_RETRY_INTERVAL_US / BRIDGE_UDS_SEND_ABORT_POLL_INTERVAL_US);
    for (int i = 0; i < slices; ++i) {
      if (!still_alive()) {
        return false;
      }
      usleep(BRIDGE_UDS_SEND_ABORT_POLL_INTERVAL_US);
    }
    return true;
  };

  ssize_t send_result = -1;
  int last_errno = 0;
  bool aborted = false;
  for (int retry = 0; retry <= BRIDGE_UDS_SEND_MAX_RETRIES; ++retry) {
    // Honour the shutdown check on entry so a request issued after shutdown
    // has already begun never even attempts the first sendto().
    if (!still_alive()) {
      aborted = true;
      break;
    }
    // MSG_NOSIGNAL is largely a no-op for SOCK_DGRAM (no broken-pipe path),
    // but keep it for parity with the stream-based debug socket and to remain
    // robust against any future protocol changes.
    send_result =
      sendto(fd, &msg, sizeof(msg), MSG_NOSIGNAL, reinterpret_cast<sockaddr *>(&sa), alen);
    if (send_result >= 0) break;
    last_errno = errno;
    if (last_errno == EINTR) {
      // EINTR is harmless; don't count it against the retry budget.
      --retry;
      continue;
    }
    // ECONNREFUSED: listener not yet bound.
    // ENOENT: address freshly recycled.
    // EAGAIN/EWOULDBLOCK/ENOBUFS: receiver buffer momentarily full.
    // Anything else is a hard failure (EPERM, EINVAL, EMSGSIZE, ...).
    if (
      last_errno != ECONNREFUSED && last_errno != ENOENT && last_errno != EAGAIN &&
      last_errno != EWOULDBLOCK && last_errno != ENOBUFS) {
      break;
    }
    if (retry < BRIDGE_UDS_SEND_MAX_RETRIES) {
      if (!interruptible_backoff()) {
        aborted = true;
        break;
      }
    }
  }

  if (aborted) {
    RCLCPP_WARN_ONCE(
      logger, "bridge UDS sendto() aborted by shutdown while waiting for '%s'",
      display_name.c_str());
    close(fd);
    return false;
  }
  if (send_result < 0) {
    RCLCPP_ERROR(
      logger, "bridge UDS sendto() failed for '%s': %s (errno: %d)", display_name.c_str(),
      strerror(last_errno), last_errno);
    close(fd);
    return false;
  }
  if (static_cast<size_t>(send_result) != sizeof(msg)) {
    // SOCK_DGRAM is atomic, so a short send should be impossible; treat any
    // discrepancy as a hard error rather than papering over it.
    RCLCPP_ERROR(
      logger, "bridge UDS sendto() short send to '%s': sent %zd of %zu bytes", display_name.c_str(),
      send_result, sizeof(msg));
    close(fd);
    return false;
  }

  close(fd);
  return true;
}

}  // namespace agnocast

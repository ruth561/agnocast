#pragma once

// Abstract-namespace UDS (SOCK_DGRAM) transport for bridge_manager
// registration. Replaces the POSIX MQ path so the address self-cleans on
// process exit; the wire payload (`BridgeMsg`) is unchanged.

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

// Forward-declared to avoid the include cycle
// bridge_uds.hpp <- publisher.hpp <- node/agnocast_context.hpp.
bool ok();

// Mirrors the historical POSIX MQ retry budget (100 * 100ms = 10s).
inline constexpr int BRIDGE_UDS_SEND_MAX_RETRIES = 100;
inline constexpr useconds_t BRIDGE_UDS_SEND_RETRY_INTERVAL_US = 100000;
inline constexpr useconds_t BRIDGE_UDS_SEND_ABORT_POLL_INTERVAL_US = 10000;
static_assert(
  BRIDGE_UDS_SEND_RETRY_INTERVAL_US % BRIDGE_UDS_SEND_ABORT_POLL_INTERVAL_US == 0,
  "abort poll interval must divide the retry interval evenly");

namespace detail
{

// Abstract-namespace addresses are length-scoped: leading NUL + name bytes,
// no trailing NUL. Returns the socklen_t bind()/sendto() expect.
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
  std::memcpy(out.sun_path, addr.data(), addr.size());
  return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + addr.size());
}

}  // namespace detail

inline int create_bridge_uds_listener(const std::string & addr)
{
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    throw std::system_error(errno, std::generic_category(), "bridge UDS socket() failed");
  }

  try {
    sockaddr_un sa{};
    const socklen_t alen = detail::fill_abstract_sockaddr(addr, sa);
    if (bind(fd, reinterpret_cast<sockaddr *>(&sa), alen) == -1) {
      throw std::system_error(errno, std::generic_category(), "bridge UDS bind() failed");
    }
  } catch (...) {
    close(fd);
    throw;
  }
  return fd;
}

// Send `data`/`size` as a single datagram to `addr`. Retries on
// ECONNREFUSED/ENOENT/EAGAIN/EWOULDBLOCK/ENOBUFS.
//
// Shutdown handling: at entry, samples rclcpp::ok() / agnocast::ok(); each
// lifecycle that is true at entry becomes "watched" and a later
// true->false transition aborts the retry loop. This lets pure-rclcpp and
// pure-agnocast processes coexist -- an unused context stays "off" without
// being mistaken for a shutdown.
inline bool send_bridge_uds_message(
  const std::string & addr, const void * data, size_t size, const rclcpp::Logger & logger)
{
  const bool watch_rclcpp = rclcpp::ok();
  const bool watch_agnocast = agnocast::ok();
  if (!watch_rclcpp && !watch_agnocast) {
    RCLCPP_WARN_ONCE(
      logger, "bridge UDS sendto() skipped: neither rclcpp nor agnocast context is initialised");
    return false;
  }
  const auto still_alive = [watch_rclcpp, watch_agnocast]() {
    if (watch_rclcpp && !rclcpp::ok()) return false;
    if (watch_agnocast && !agnocast::ok()) return false;
    return true;
  };

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

  const std::string display_name = (!addr.empty() && addr.front() == '\0') ? addr.substr(1) : addr;

  // Sliced back-off so a SIGINT can abort within ~10 ms even mid-sleep.
  const auto interruptible_backoff = [&still_alive]() {
    constexpr int slices =
      static_cast<int>(BRIDGE_UDS_SEND_RETRY_INTERVAL_US / BRIDGE_UDS_SEND_ABORT_POLL_INTERVAL_US);
    for (int i = 0; i < slices; ++i) {
      if (!still_alive()) return false;
      usleep(BRIDGE_UDS_SEND_ABORT_POLL_INTERVAL_US);
    }
    return true;
  };

  ssize_t send_result = -1;
  int last_errno = 0;
  bool aborted = false;
  for (int retry = 0; retry <= BRIDGE_UDS_SEND_MAX_RETRIES; ++retry) {
    if (!still_alive()) {
      aborted = true;
      break;
    }
    send_result = sendto(fd, data, size, MSG_NOSIGNAL, reinterpret_cast<sockaddr *>(&sa), alen);
    if (send_result >= 0) break;
    last_errno = errno;
    if (last_errno == EINTR) {
      --retry;
      continue;
    }
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
  if (static_cast<size_t>(send_result) != size) {
    // SOCK_DGRAM is atomic; a short send would indicate a kernel/API bug.
    RCLCPP_ERROR(
      logger, "bridge UDS sendto() short send to '%s': sent %zd of %zu bytes", display_name.c_str(),
      send_result, size);
    close(fd);
    return false;
  }

  close(fd);
  return true;
}

}  // namespace agnocast

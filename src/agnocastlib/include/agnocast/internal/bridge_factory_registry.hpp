// Copyright 2025
// SPDX-License-Identifier: Apache-2.0
//
// Process-local table mapping a ROS 2 message type name to the bridge
// factory function pair (`start_a2r_pubsub_node<T>` /
// `start_r2a_pubsub_node<T>`).
//
// `Publisher<T>` / `Subscription<T>` constructors register the entry for
// their `T` exactly once. The Standard bridge_manager (which runs inside
// the same user process) looks the entry up when it receives a
// daemon-originated `MqMsgDaemonBridge` and instantiates the bridge node
// without needing the factory information to travel over the MQ.
//
// Implementation notes:
//
// * This header avoids including `agnocast/bridge/agnocast_bridge_node.hpp`
//   because that header pulls in `agnocast_publisher.hpp` /
//   `agnocast_subscription.hpp` and the publisher / subscription headers
//   include this header. The factory templates are forward-declared here
//   and resolved at instantiation time when the user's translation unit
//   has already pulled in the bridge factory definitions through the
//   public `agnocast.hpp` umbrella.
//
// * `register_bridge_factory<T>()` is gated on
//   `rosidl_generator_traits::is_message<T>::value` so that it is a no-op
//   when instantiated for service types (which `BasicService<ServiceT>`
//   does internally). Without this gate the bridge factory lambdas would
//   force-instantiate `start_a2r_pubsub_node<ServiceT>` and trip a
//   static_assert in `rclcpp::Publisher`.

#pragma once

#include "agnocast/agnocast_mq.hpp"
#include "agnocast/agnocast_utils.hpp"

#include <rclcpp/logger.hpp>
#include <rclcpp/logging.hpp>
#include <rclcpp/node.hpp>
#include <rclcpp/qos.hpp>
#include <rosidl_runtime_cpp/traits.hpp>

#include <dlfcn.h>
#include <fcntl.h>
#include <mqueue.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace agnocast
{

// Forward declarations from agnocast/bridge/agnocast_bridge_node.hpp.
class PubsubBridgeBase;

template <typename MessageT>
std::shared_ptr<PubsubBridgeBase> start_a2r_pubsub_node(
  rclcpp::Node::SharedPtr node, const std::string & topic_name, const rclcpp::QoS & qos);

template <typename MessageT>
std::shared_ptr<PubsubBridgeBase> start_r2a_pubsub_node(
  rclcpp::Node::SharedPtr node, const std::string & topic_name, const rclcpp::QoS & qos);

namespace internal
{

using BridgeFactoryFn = std::function<std::shared_ptr<PubsubBridgeBase>(
  rclcpp::Node::SharedPtr, const std::string &, const rclcpp::QoS &)>;

struct BridgeFactoryEntry
{
  BridgeFactoryFn a2r;
  BridgeFactoryFn r2a;
};

class BridgeFactoryRegistry
{
public:
  static BridgeFactoryRegistry & instance()
  {
    static BridgeFactoryRegistry registry;
    return registry;
  }

  void register_entry(const std::string & type_name, BridgeFactoryEntry entry)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[type_name] = std::move(entry);
  }

  bool lookup(const std::string & type_name, BridgeFactoryEntry & out) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(type_name);
    if (it == map_.end()) {
      return false;
    }
    out = it->second;
    return true;
  }

private:
  BridgeFactoryRegistry() = default;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, BridgeFactoryEntry> map_;
};

// Mirrors a successful local `register_bridge_factory<T>()` to the
// Standard-mode bridge_manager via a small POSIX MQ write.
//
// We can't send raw function pointers because the function may live in a
// shared library dlopen()'d *after* the bridge_manager was forked
// (composable_node case). Instead we resolve the function via `dladdr`
// to (shared_lib_path, offset-from-base) and let the bridge_manager
// dlopen the same library and reconstruct the address. This mirrors the
// existing intra-NS MqMsgBridge / BridgeFactoryInfo flow.
//
// Best-effort: any failure (dladdr / mq_open / mq_send) is logged once and
// the user process continues. The daemon path will silently no-op for
// this type until a working pre-registration lands.
//
// TODO: Replace with a `need-minor-update` kmod path that exposes the
// message type alongside the existing topic / node info so the
// bridge_manager can pre-populate its registry directly from the kmod and
// retire this MQ entirely.
template <typename MessageT>
inline void notify_bridge_manager_of_factory(const std::string & type_name)
{
  if (standard_bridge_manager_pid <= 0) {
    // Either bridge_manager has not been spawned yet (early static init?)
    // or this process is running in Performance mode where the
    // bridge_manager is a different process model.
    return;
  }

  // Resolve the function templates to (lib_path, offset-from-base) so the
  // bridge_manager can reconstruct the address in its own process. dladdr
  // on a template instantiation returns the .so (or main executable) that
  // owns the instantiation.
  auto fn_a2r_addr = reinterpret_cast<uintptr_t>(&start_a2r_pubsub_node<MessageT>);
  auto fn_r2a_addr = reinterpret_cast<uintptr_t>(&start_r2a_pubsub_node<MessageT>);
  Dl_info info_a2r{};
  Dl_info info_r2a{};
  if (
    dladdr(reinterpret_cast<void *>(fn_a2r_addr), &info_a2r) == 0 ||
    info_a2r.dli_fname == nullptr || info_a2r.dli_fbase == nullptr) {
    RCLCPP_WARN_ONCE(
      rclcpp::get_logger("agnocast"),
      "dladdr failed for start_a2r_pubsub_node<%s>; skipping factory pre-register.",
      type_name.c_str());
    return;
  }
  if (
    dladdr(reinterpret_cast<void *>(fn_r2a_addr), &info_r2a) == 0 ||
    info_r2a.dli_fname == nullptr || info_r2a.dli_fbase == nullptr) {
    RCLCPP_WARN_ONCE(
      rclcpp::get_logger("agnocast"),
      "dladdr failed for start_r2a_pubsub_node<%s>; skipping factory pre-register.",
      type_name.c_str());
    return;
  }
  // The two factories should come from the same translation unit / library
  // (they're both template instantiations in the same scope). If they
  // disagree something pathological is going on.
  if (std::strcmp(info_a2r.dli_fname, info_r2a.dli_fname) != 0) {
    RCLCPP_WARN_ONCE(
      rclcpp::get_logger("agnocast"),
      "start_a2r and start_r2a for type '%s' resolved to different libraries (%s vs %s);"
      " skipping factory pre-register.",
      type_name.c_str(), info_a2r.dli_fname, info_r2a.dli_fname);
    return;
  }

  const std::string mq_name = create_mq_name_for_factory_register(standard_bridge_manager_pid);

  struct mq_attr attr = {};
  attr.mq_maxmsg = FACTORY_REGISTER_MQ_MAX_MESSAGES;
  attr.mq_msgsize = FACTORY_REGISTER_MQ_MESSAGE_SIZE;

  // O_CREAT so that the user process can race the bridge_manager — whichever
  // side opens first creates the MQ. The kernel returns the existing MQ on
  // subsequent calls.
  mqd_t fd = mq_open(
    mq_name.c_str(), O_WRONLY | O_CREAT | O_NONBLOCK | O_CLOEXEC, BRIDGE_MQ_PERMS, &attr);
  if (fd == (mqd_t)-1) {
    RCLCPP_WARN_ONCE(
      rclcpp::get_logger("agnocast"),
      "Failed to open factory register MQ '%s': %s. Cross-IPC-NS bridges for "
      "this process will not be auto-generated until this succeeds.",
      mq_name.c_str(), std::strerror(errno));
    return;
  }

  MqMsgFactoryRegister msg{};
  std::strncpy(msg.type_name, type_name.c_str(), sizeof(msg.type_name) - 1);
  std::strncpy(msg.shared_lib_path, info_a2r.dli_fname, sizeof(msg.shared_lib_path) - 1);
  msg.fn_offset_a2r = fn_a2r_addr - reinterpret_cast<uintptr_t>(info_a2r.dli_fbase);
  msg.fn_offset_r2a = fn_r2a_addr - reinterpret_cast<uintptr_t>(info_r2a.dli_fbase);

  if (mq_send(fd, reinterpret_cast<const char *>(&msg), sizeof(msg), 0) == -1) {
    RCLCPP_WARN_ONCE(
      rclcpp::get_logger("agnocast"),
      "Failed to send factory register msg for type '%s' on MQ '%s': %s.",
      type_name.c_str(), mq_name.c_str(), std::strerror(errno));
  }
  if (mq_close(fd) == -1) {
    RCLCPP_WARN_ONCE(
      rclcpp::get_logger("agnocast"), "Failed to close factory register MQ '%s': %s.",
      mq_name.c_str(), std::strerror(errno));
  }
}

// Called from `Publisher<T>` / `Subscription<T>` constructors. Idempotent.
// No-op when instantiated for non-message types (e.g. service types pulled in
// by `BasicService<ServiceT>`'s internal subscription / publisher).
template <typename MessageT>
void register_bridge_factory()
{
  if constexpr (rosidl_generator_traits::is_message<MessageT>::value) {
    const std::string type_name = rosidl_generator_traits::name<MessageT>();
    BridgeFactoryEntry entry{
      [](rclcpp::Node::SharedPtr node, const std::string & topic_name, const rclcpp::QoS & qos)
        -> std::shared_ptr<PubsubBridgeBase> {
        return start_a2r_pubsub_node<MessageT>(node, topic_name, qos);
      },
      [](rclcpp::Node::SharedPtr node, const std::string & topic_name, const rclcpp::QoS & qos)
        -> std::shared_ptr<PubsubBridgeBase> {
        return start_r2a_pubsub_node<MessageT>(node, topic_name, qos);
      },
    };
    BridgeFactoryRegistry::instance().register_entry(type_name, std::move(entry));
    notify_bridge_manager_of_factory<MessageT>(type_name);
  }
}

}  // namespace internal
}  // namespace agnocast

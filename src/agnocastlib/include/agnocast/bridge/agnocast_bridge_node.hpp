#pragma once

#include "agnocast/agnocast_client.hpp"
#include "agnocast/agnocast_ioctl.hpp"
#include "agnocast/agnocast_mq.hpp"
#include "agnocast/agnocast_publisher.hpp"
#include "agnocast/agnocast_subscription.hpp"
#include "agnocast/bridge/agnocast_bridge_utils.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp/version.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace agnocast

{

static constexpr size_t DEFAULT_QOS_DEPTH = 10;

template <typename MessageT>
void send_performance_pubsub_bridge_registration(
  const std::string & topic_name, topic_local_id_t id, BridgeDirection direction);
inline void send_performance_pubsub_bridge_registration_by_type_name(
  const std::string & topic_name, topic_local_id_t id, const std::string & message_type_name,
  BridgeDirection direction);
template <typename ServiceT>
void send_performance_service_bridge_registration(
  const std::string & service_name, BridgeDirection direction,
  const std::optional<std::pair<std::string, std::string>> & shadow_node_identity);

template <typename MessageT>
void register_pubsub_bridge_core(
  const std::string & topic_name, topic_local_id_t id, BridgeDirection direction)
{
  auto bridge_mode = get_bridge_mode();
  if (bridge_mode == BridgeMode::On) {
    send_performance_pubsub_bridge_registration<MessageT>(topic_name, id, direction);
  }
}

inline void register_pubsub_bridge_by_type_name(
  const std::string & topic_name, topic_local_id_t id, const std::string & message_type,
  BridgeDirection direction)
{
  auto bridge_mode = get_bridge_mode();
  if (bridge_mode == BridgeMode::On) {
    send_performance_pubsub_bridge_registration_by_type_name(
      topic_name, id, message_type, direction);
  }
}

template <typename ServiceT>
void register_service_bridge_core(
  const std::string & service_name, BridgeDirection direction,
  const std::optional<std::pair<std::string, std::string>> & shadow_node_identity)
{
  auto bridge_mode = get_bridge_mode();
  if (bridge_mode == BridgeMode::On) {
    send_performance_service_bridge_registration<ServiceT>(
      service_name, direction, shadow_node_identity);
  }
}

// Policy for agnocast::Subscription.
// Registers a bridge that forwards messages from ROS 2 to Agnocast (R2A).
struct RosToAgnocastPubsubRegistrationPolicy
{
  template <typename MessageT>
  static void register_bridge(const std::string & topic_name, topic_local_id_t id)
  {
    register_pubsub_bridge_core<MessageT>(topic_name, id, BridgeDirection::ROS2_TO_AGNOCAST);
  }
};

// Policy for agnocast::Publisher.
// Registers a bridge that forwards messages from Agnocast to ROS 2 (A2R).
struct AgnocastToRosPubsubRegistrationPolicy
{
  template <typename MessageT>
  static void register_bridge(const std::string & topic_name, topic_local_id_t id)
  {
    register_pubsub_bridge_core<MessageT>(topic_name, id, BridgeDirection::AGNOCAST_TO_ROS2);
  }
};

// Policy for agnocast::Service.
// Registers a bridge that forwards requests from ROS 2 to Agnocast (R2A).
struct RosToAgnocastServiceRegistrationPolicy
{
  template <typename NodeT, typename ServiceT>
  static void register_bridge(NodeT * node, const std::string & service_name)
  {
    std::optional<std::pair<std::string, std::string>> shadow_node_identity{std::nullopt};
    if constexpr (std::is_same_v<std::remove_cv_t<NodeT>, agnocast::Node>) {
      shadow_node_identity =
        std::make_pair(std::string(node->get_namespace()), std::string(node->get_name()));
    }
    register_service_bridge_core<ServiceT>(
      service_name, BridgeDirection::ROS2_TO_AGNOCAST, shadow_node_identity);
  }
};

// Dummy policy to avoid circular header dependencies.
// Used internally by BridgeNode, Service, and Client where bridge registrations
// are not needed and would cause include cycles.
struct NoBridgeRegistrationPolicy
{
  template <typename T, typename... Args>
  static void register_bridge(Args &&... args)
  {
    register_bridge_impl(std::forward<Args>(args)...);
  }

private:
  static void register_bridge_impl(const std::string &, topic_local_id_t) {}
  template <typename NodeT>
  static void register_bridge_impl(NodeT *, const std::string &)
  {
  }
};

// Hand a Bridge-bound message to the kmod's per-IPC-ns FIFO via the
// AGNOCAST_SEND_MSG_TO_BRIDGE_CMD ioctl. The kmod treats the payload as an
// opaque byte sequence, so any MqMsg* struct that fits in MAX_BRIDGE_MSG_SIZE
// can be routed through here.
//
// ENOSPC means the per-IPC-ns queue is temporarily full (the Bridge Manager
// has not drained it fast enough). This is the kmod-side analogue of the
// old mq_send EAGAIN, so we retry on the same 100 × 100ms budget the old
// implementation used. Any other errno is treated as fatal (logged once).
template <typename MsgStruct>
void send_msg_to_bridge_via_kmod(const MsgStruct & msg, const rclcpp::Logger & logger)
{
  static_assert(
    sizeof(MsgStruct) <= MAX_BRIDGE_MSG_SIZE, "bridge message exceeds kmod MAX_BRIDGE_MSG_SIZE");

  ioctl_send_msg_to_bridge_args args{};
  args.size = static_cast<uint32_t>(sizeof(MsgStruct));
  std::memcpy(args.payload, &msg, sizeof(MsgStruct));

  constexpr int BRIDGE_SEND_MAX_RETRIES = 100;
  constexpr useconds_t BRIDGE_SEND_RETRY_INTERVAL_US = 100000;  // 100ms

  int send_result = -1;
  int last_errno = 0;
  for (int retry = 0; retry <= BRIDGE_SEND_MAX_RETRIES; ++retry) {
    send_result = ioctl(agnocast_fd, AGNOCAST_SEND_MSG_TO_BRIDGE_CMD, &args);
    if (send_result == 0) break;
    last_errno = errno;
    if (last_errno != ENOSPC) break;
    if (retry < BRIDGE_SEND_MAX_RETRIES) {
      usleep(BRIDGE_SEND_RETRY_INTERVAL_US);
    }
  }
  if (send_result < 0) {
    if (last_errno == ENOSPC) {
      RCLCPP_ERROR(
        logger,
        "AGNOCAST_SEND_MSG_TO_BRIDGE_CMD dropped a message: queue full after %d retries "
        "(size=%zu)",
        BRIDGE_SEND_MAX_RETRIES, sizeof(MsgStruct));
    } else {
      RCLCPP_ERROR(
        logger, "AGNOCAST_SEND_MSG_TO_BRIDGE_CMD failed: %s (errno: %d)", strerror(last_errno),
        last_errno);
    }
  }
}

template <typename MessageT>
void send_performance_pubsub_bridge_registration(
  const std::string & topic_name, topic_local_id_t id, BridgeDirection direction)
{
  const std::string message_type_name = rosidl_generator_traits::name<MessageT>();
  send_performance_pubsub_bridge_registration_by_type_name(
    topic_name, id, message_type_name, direction);
}

inline void send_performance_pubsub_bridge_registration_by_type_name(
  const std::string & topic_name, topic_local_id_t id, const std::string & message_type_name,
  BridgeDirection direction)
{
  static const auto logger = rclcpp::get_logger("agnocast_performance_bridge_registrar");

  auto [msg, reason] = BridgeRegistrationMsgBuilder()
                         .set_direction(direction)
                         .set_is_service(false)
                         .set_message_type(message_type_name.c_str())
                         .set_topic_name(topic_name.c_str())
                         .set_pubsub_target_id(id)
                         .build_performance_message();
  if (!reason.empty()) {
    RCLCPP_ERROR(
      logger, "Failed to build performance pubsub bridge registration: %s", reason.c_str());
    close(agnocast_fd);
    exit(EXIT_FAILURE);
  }

  send_msg_to_bridge_via_kmod(msg, logger);
}

template <typename ServiceT>
void send_performance_service_bridge_registration(
  const std::string & service_name, BridgeDirection direction,
  const std::optional<std::pair<std::string, std::string>> & shadow_node_identity)
{
  static const auto logger = rclcpp::get_logger("agnocast_performance_service_bridge_registrar");

  const std::string service_type_name = rosidl_generator_traits::name<ServiceT>();

  auto [msg, reason] = BridgeRegistrationMsgBuilder()
                         .set_direction(direction)
                         .set_is_service(true)
                         .set_service_type(service_type_name.c_str())
                         .set_service_name(service_name.c_str())
                         .set_shadow_node_identity(shadow_node_identity)
                         .build_performance_message();
  if (!reason.empty()) {
    RCLCPP_ERROR(
      logger, "Failed to build performance service bridge registration: %s", reason.c_str());
    close(agnocast_fd);
    exit(EXIT_FAILURE);
  }

  send_msg_to_bridge_via_kmod(msg, logger);
}

}  // namespace agnocast

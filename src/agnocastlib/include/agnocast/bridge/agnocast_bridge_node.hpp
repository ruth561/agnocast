#pragma once

#include "agnocast/agnocast_client.hpp"
#include "agnocast/agnocast_mq.hpp"
#include "agnocast/agnocast_publisher.hpp"
#include "agnocast/agnocast_subscription.hpp"
#include "agnocast/bridge/agnocast_bridge_uds.hpp"
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
// NodeT is needed so the shadow_node_identity payload can differ between
// rclcpp::Node and agnocast::Node.
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
  // Pubsub variant: register_bridge<MessageT>(topic_name, id).
  template <typename MessageT>
  static void register_bridge(const std::string &, topic_local_id_t)
  {
  }

  // Service variant: register_bridge<NodeT, ServiceT>(node, service_name).
  template <typename NodeT, typename ServiceT>
  static void register_bridge(NodeT *, const std::string &)
  {
  }
};

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

  const std::string uds_addr = create_uds_addr_for_bridge(PERFORMANCE_BRIDGE_VIRTUAL_PID);
  // send_bridge_uds_message() handles its own shutdown-driven abort by
  // sampling rclcpp::ok() / agnocast::ok() at entry and bailing on a watched
  // true->false transition; no predicate needs to be plumbed through.
  (void)send_bridge_uds_message(uds_addr, msg, logger);
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

  const std::string uds_addr = create_uds_addr_for_bridge(PERFORMANCE_BRIDGE_VIRTUAL_PID);
  (void)send_bridge_uds_message(uds_addr, msg, logger);
}

}  // namespace agnocast

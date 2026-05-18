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

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>

#include <rclcpp/node.hpp>
#include <rclcpp/qos.hpp>
#include <rosidl_runtime_cpp/traits.hpp>

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

// Called from `Publisher<T>` / `Subscription<T>` constructors. Idempotent.
// No-op when instantiated for non-message types (e.g. service types pulled in
// by `BasicService<ServiceT>`'s internal subscription / publisher).
template <typename MessageT>
void register_bridge_factory()
{
  if constexpr (rosidl_generator_traits::is_message<MessageT>::value) {
    const std::string type_name = rosidl_generator_traits::name<MessageT>();
    BridgeFactoryEntry entry{
      [](rclcpp::Node::SharedPtr node, const std::string & topic_name,
         const rclcpp::QoS & qos) -> std::shared_ptr<PubsubBridgeBase> {
        return start_a2r_pubsub_node<MessageT>(node, topic_name, qos);
      },
      [](rclcpp::Node::SharedPtr node, const std::string & topic_name,
         const rclcpp::QoS & qos) -> std::shared_ptr<PubsubBridgeBase> {
        return start_r2a_pubsub_node<MessageT>(node, topic_name, qos);
      },
    };
    BridgeFactoryRegistry::instance().register_entry(type_name, std::move(entry));
  }
}

}  // namespace internal
}  // namespace agnocast

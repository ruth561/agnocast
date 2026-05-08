#include "agnocast/agnocast.hpp"

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/serialized_message.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>

PerformancePubsubBridgeResult create_r2a_generic_bridge(
  rclcpp::Node::SharedPtr node,
  const std::string & topic_name,
  const rclcpp::QoS & sub_qos,
  const std::string & type_name,
  std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> callback);

std::pair<std::shared_ptr<rclcpp::GenericPublisher>, rclcpp::CallbackGroup::SharedPtr>
create_a2r_generic_publisher(
  rclcpp::Node::SharedPtr node,
  const std::string & topic_name,
  const std::string & type_name,
  const rclcpp::QoS & qos);

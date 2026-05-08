#include "generic_functions.hpp"

PerformancePubsubBridgeResult create_r2a_generic_bridge(
  rclcpp::Node::SharedPtr node,
  const std::string & topic_name,
  const rclcpp::QoS & sub_qos,
  const std::string & type_name,
  std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> callback)
{
  auto cb_group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  rclcpp::SubscriptionOptions opts;
  opts.ignore_local_publications = true;
  opts.callback_group = cb_group;
  auto sub = node->create_generic_subscription(topic_name, type_name, sub_qos, callback, opts);
  return {sub, cb_group};
}

std::pair<std::shared_ptr<rclcpp::GenericPublisher>, rclcpp::CallbackGroup::SharedPtr>
create_a2r_generic_publisher(
  rclcpp::Node::SharedPtr node,
  const std::string & topic_name,
  const std::string & type_name,
  const rclcpp::QoS & qos)
{
  auto pub = node->create_generic_publisher(topic_name, type_name, qos);
  auto cb_group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  return {pub, cb_group};
}

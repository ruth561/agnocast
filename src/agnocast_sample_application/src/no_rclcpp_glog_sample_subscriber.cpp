#include "agnocast/node/agnocast_node.hpp"
#include "agnocast_sample_interfaces/msg/dynamic_size_array.hpp"

#include <rclcpp_components/register_node_macro.hpp>

#include <glog/logging.h>

#include <map>
#include <string>

namespace
{

void cause_sigsegv()
{
  (void)std::raise(SIGSEGV);
}

using std::placeholders::_1;

class NoRclcppGlogSampleSubscriber : public agnocast::Node
{
  agnocast::Subscription<agnocast_sample_interfaces::msg::DynamicSizeArray>::SharedPtr sub_dynamic_;

  void callback(
    const agnocast::ipc_shared_ptr<agnocast_sample_interfaces::msg::DynamicSizeArray> & message)
  {
    LOG(INFO) << "[NoRclcppGlogSampleSubscriber] received message: id=" << message->id
              << ", data_size=" << message->data.size();
    RCLCPP_INFO(
      get_logger(), "I heard dynamic size array message with size: %zu", message->data.size());

    RCLCPP_INFO(get_logger(), "Causing SIGSEGV in callback...");
    cause_sigsegv();
  }

public:
  explicit NoRclcppGlogSampleSubscriber(const rclcpp::NodeOptions & options)
  : agnocast::Node("no_rclcpp_glog_sample_subscriber", options)
  {
    constexpr auto qos_queue_size = 5;
    declare_parameter("topic_name", rclcpp::ParameterValue(std::string("/my_topic")));
    declare_parameter("qos.queue_size", rclcpp::ParameterValue(qos_queue_size));

    std::string topic_name;
    get_parameter("topic_name", topic_name);

    std::map<std::string, rclcpp::Parameter> qos_parameters;
    get_parameters("qos", qos_parameters);
    const auto queue_size = static_cast<size_t>(qos_parameters["queue_size"].as_int());

    auto resolved_topic = get_node_topics_interface()->resolve_topic_name(topic_name);
    rclcpp::QoS qos{rclcpp::KeepLast(queue_size)};

    sub_dynamic_ = this->create_subscription<agnocast_sample_interfaces::msg::DynamicSizeArray>(
      resolved_topic, qos,
      [this](const agnocast::ipc_shared_ptr<agnocast_sample_interfaces::msg::DynamicSizeArray> &
               message) { callback(message); });

    LOG(INFO) << "[NoRclcppGlogSampleSubscriber] subscribed to " << resolved_topic
              << " with queue_size=" << queue_size;
    RCLCPP_INFO(get_logger(), "NoRclcppGlogSampleSubscriber initialized");
  }
};

}  // namespace

RCLCPP_COMPONENTS_REGISTER_NODE(NoRclcppGlogSampleSubscriber)

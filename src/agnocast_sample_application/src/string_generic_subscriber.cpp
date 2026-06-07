#include "agnocast/agnocast.hpp"
#include "rclcpp/rclcpp.hpp"

#include <rclcpp/serialization.hpp>
#include <rclcpp/serialized_message.hpp>

#include <std_msgs/msg/string.hpp>

using std::placeholders::_1;

class StringGenericSubscriber : public rclcpp::Node
{
  agnocast::GenericSubscription::SharedPtr sub_;

  void callback(std::shared_ptr<rclcpp::SerializedMessage> serialized_msg)
  {
    rclcpp::Serialization<std_msgs::msg::String> serializer;
    std_msgs::msg::String msg;
    serializer.deserialize_message(serialized_msg.get(), &msg);

    RCLCPP_INFO(this->get_logger(), "GenericSubscription received: '%s'", msg.data.c_str());
  }

public:
  StringGenericSubscriber() : Node("string_generic_subscriber")
  {
    rclcpp::CallbackGroup::SharedPtr group =
      create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    agnocast::SubscriptionOptions agnocast_options;
    agnocast_options.callback_group = group;

    sub_ = agnocast::create_generic_subscription(
      this, "/my_topic", "std_msgs/msg/String", rclcpp::QoS{1},
      std::bind(&StringGenericSubscriber::callback, this, _1), agnocast_options);
  }
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  agnocast::SingleThreadedAgnocastExecutor executor;
  auto node = std::make_shared<StringGenericSubscriber>();
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}

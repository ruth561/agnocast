#include "agnocast/agnocast.hpp"
#include "rclcpp/rclcpp.hpp"

#include <std_msgs/msg/string.hpp>

using std::placeholders::_1;

class StringSubscriber : public rclcpp::Node
{
  agnocast::Subscription<std_msgs::msg::String>::SharedPtr sub_;

  void callback(const agnocast::ipc_shared_ptr<const std_msgs::msg::String> & msg)
  {
    RCLCPP_INFO(this->get_logger(), "Subscription received: '%s'", msg->data.c_str());
  }

public:
  StringSubscriber() : Node("string_subscriber")
  {
    rclcpp::CallbackGroup::SharedPtr group =
      create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    agnocast::SubscriptionOptions agnocast_options;
    agnocast_options.callback_group = group;

    sub_ = agnocast::create_subscription<std_msgs::msg::String>(
      this, "/my_topic", rclcpp::QoS{1}, std::bind(&StringSubscriber::callback, this, _1),
      agnocast_options);
  }
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  agnocast::SingleThreadedAgnocastExecutor executor;
  auto node = std::make_shared<StringSubscriber>();
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}

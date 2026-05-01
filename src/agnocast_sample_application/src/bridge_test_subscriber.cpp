#include "agnocast/agnocast.hpp"
#include "rclcpp/rclcpp.hpp"

#include "std_msgs/msg/string.hpp"

#include <memory>

namespace
{

class MinimalSubscriber : public rclcpp::Node
{
  agnocast::Subscription<std_msgs::msg::String>::SharedPtr sub_dynamic_;

public:
  explicit MinimalSubscriber() : Node("cie_subscriber")
  {
    rclcpp::CallbackGroup::SharedPtr group =
      create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    agnocast::SubscriptionOptions agnocast_options;
    agnocast_options.callback_group = group;

    std::cout << "🚀 Creating subscription\n";
    sub_dynamic_ = agnocast::create_subscription<std_msgs::msg::String>(
      this, "/my_topic", 1,
      [this](const agnocast::ipc_shared_ptr<std_msgs::msg::String> & message) {
        RCLCPP_INFO(this->get_logger(), "subscribe message: %s", message->data.c_str());
      },
      agnocast_options);
  }
};

}  // namespace

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  agnocast::MultiThreadedAgnocastExecutor executor;
  std::cout << "🚀 Creating node\n";
  auto node = std::make_shared<MinimalSubscriber>();
  std::cout << "🚀 Adding node to executor\n";
  executor.add_node(node);
  std::cout << "🚀 Starting spinning\n";
  executor.spin();
  std::cout << "🚀 Stopped spinning\n";

  rclcpp::shutdown();
  return 0;
}

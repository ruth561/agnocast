#include "agnocast/agnocast.hpp"
#include "rclcpp/rclcpp.hpp"

#include <rclcpp/serialization.hpp>
#include <rclcpp/serialized_message.hpp>

#include <std_msgs/msg/string.hpp>

using namespace std::chrono_literals;

class StringGenericPublisher : public rclcpp::Node
{
  int64_t count_;
  rclcpp::TimerBase::SharedPtr timer_;
  agnocast::GenericPublisher::SharedPtr publisher_;

  void timer_callback()
  {
    std_msgs::msg::String msg;
    msg.data = "Hello from GenericPublisher! #" + std::to_string(count_++);

    rclcpp::Serialization<std_msgs::msg::String> serializer;
    rclcpp::SerializedMessage serialized;
    serializer.serialize_message(&msg, &serialized);

    publisher_->publish(serialized);
    RCLCPP_INFO(this->get_logger(), "GenericPublisher published: '%s'", msg.data.c_str());
  }

public:
  StringGenericPublisher() : Node("string_generic_publisher"), count_(0)
  {
    publisher_ =
      agnocast::create_generic_publisher(this, "/my_topic", "std_msgs/msg/String", rclcpp::QoS{1});
    timer_ =
      this->create_wall_timer(100ms, std::bind(&StringGenericPublisher::timer_callback, this));
  }
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  agnocast::SingleThreadedAgnocastExecutor executor;
  auto node = std::make_shared<StringGenericPublisher>();
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}

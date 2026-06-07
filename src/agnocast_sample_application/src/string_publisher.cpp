#include "agnocast/agnocast.hpp"
#include "rclcpp/rclcpp.hpp"

#include <std_msgs/msg/string.hpp>

using namespace std::chrono_literals;

class StringPublisher : public rclcpp::Node
{
  int64_t count_;
  rclcpp::TimerBase::SharedPtr timer_;
  agnocast::Publisher<std_msgs::msg::String>::SharedPtr publisher_;

  void timer_callback()
  {
    auto message = publisher_->borrow_loaned_message();
    message->data = "Hello, GenericSubscription! #" + std::to_string(count_);

    publisher_->publish(std::move(message));
    RCLCPP_INFO(this->get_logger(), "publish message: id=%ld", count_++);
  }

public:
  StringPublisher() : Node("string_publisher"), count_(0)
  {
    publisher_ = agnocast::create_publisher<std_msgs::msg::String>(this, "/my_topic", 1);
    timer_ = this->create_wall_timer(100ms, std::bind(&StringPublisher::timer_callback, this));
  }
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  agnocast::SingleThreadedAgnocastExecutor executor;
  auto node = std::make_shared<StringPublisher>();
  executor.add_node(node);
  executor.spin();

  rclcpp::shutdown();
  return 0;
}

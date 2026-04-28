#include "agnocast/agnocast.hpp"
#include "agnocast_sample_interfaces/msg/dynamic_size_array.hpp"

#include <glog/logging.h>

#include <csignal>
#include <cstdint>
#include <iostream>
#include <thread>

namespace
{

void cause_sigsegv()
{
  (void)std::raise(SIGSEGV);
}

using namespace std::chrono_literals;
const int64_t MESSAGE_SIZE = 1000LL * 1024;

class NoRclcppPublisher : public agnocast::Node
{
  int64_t count_{0};
  agnocast::Publisher<agnocast_sample_interfaces::msg::DynamicSizeArray>::SharedPtr pub_;
  agnocast::TimerBase::SharedPtr timer_;

  void timer_callback()
  {
    auto message = pub_->borrow_loaned_message();

    message->id = count_;
    message->data.reserve(MESSAGE_SIZE / sizeof(int64_t));
    for (size_t i = 0; i < MESSAGE_SIZE / sizeof(int64_t); i++) {
      message->data.push_back(static_cast<int64_t>(i) + count_);
    }

    pub_->publish(std::move(message));
    RCLCPP_INFO(get_logger(), "publish message: id=%ld", count_++);
  }

public:
  explicit NoRclcppPublisher() : Node("no_rclcpp_publisher")
  {
    pub_ =
      this->create_publisher<agnocast_sample_interfaces::msg::DynamicSizeArray>("/my_topic", 1);

    timer_ = agnocast::create_timer(
      this, std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME), rclcpp::Duration(100ms),
      [this]() { this->timer_callback(); });
  }
};

}  // namespace

int main(int argc, char ** argv)
{
  (void)argc;

  google::InitGoogleLogging(argv[0]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  google::InstallFailureSignalHandler();

  std::cout << "Hello, glog!\n";

  agnocast::init(argc, argv);
  agnocast::AgnocastOnlySingleThreadedExecutor executor;
  auto node = std::make_shared<NoRclcppPublisher>();
  executor.add_node(node);

  std::thread spin_thread([&executor]() { executor.spin(); });

  std::this_thread::sleep_for(1s);
  cause_sigsegv();

  return 0;
}

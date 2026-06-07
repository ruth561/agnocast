// Integration tests for agnocast::GenericSubscription (rclcpp::Node variant).
//
// Requires:
//   - Agnocast kernel module loaded (/dev/agnocast must be accessible)
//   - libagnocast_heaphook.so preloaded (LD_PRELOAD) for shared-memory allocation
//
// Test 1 (round_trip): Publish a std_msgs::msg::String via agnocast::Publisher,
//   receive it via GenericSubscription, and verify the SerializedMessage
//   round-trips back to the original string via
//   rclcpp::Serialization<std_msgs::msg::String>::deserialize_message.
//
// Test 2 (lifecycle): Verify that the GenericSubscription constructor registers
//   an entry in id2_callback_info and the destructor removes it.

#include "agnocast/agnocast.hpp"
#include "agnocast/agnocast_callback_info.hpp"
#include "agnocast/agnocast_single_threaded_executor.hpp"
#include "agnocast/agnocast_subscription.hpp"
#include "rclcpp/serialization.hpp"

#include "std_msgs/msg/string.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using StringMsg = std_msgs::msg::String;

class GenericSubscriptionIntegrationTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    rclcpp::init(0, nullptr);
    node_ = std::make_shared<rclcpp::Node>("test_generic_sub");
    executor_ = std::make_shared<agnocast::SingleThreadedAgnocastExecutor>();
    executor_->add_node(node_);
    spin_thread_ = std::thread([this]() { executor_->spin(); });
  }

  void TearDown() override
  {
    executor_->cancel();
    if (spin_thread_.joinable()) {
      spin_thread_.join();
    }
    node_.reset();
    executor_.reset();
    rclcpp::shutdown();
  }

  template <typename Predicate>
  bool wait_for(Predicate pred, std::chrono::milliseconds timeout = 3000ms)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (pred()) return true;
      std::this_thread::sleep_for(10ms);
    }
    return pred();
  }

  std::shared_ptr<rclcpp::Node> node_;
  std::shared_ptr<agnocast::SingleThreadedAgnocastExecutor> executor_;
  std::thread spin_thread_;
};

// ---------------------------------------------------------------------------
// Test 1: round-trip serialization
// ---------------------------------------------------------------------------
TEST_F(GenericSubscriptionIntegrationTest, round_trip_serialization)
{
  const std::string topic = "/test_generic_sub_round_trip";
  const std::string type = "std_msgs/msg/String";
  const std::string expected_data = "hello generic subscription";
  rclcpp::QoS qos{1};

  // Publisher
  auto pub = agnocast::create_publisher<StringMsg>(node_.get(), topic, qos);

  // GenericSubscription (use a dedicated callback group to avoid mixing with
  // ROS 2 callbacks in the default group).
  auto cbg = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  agnocast::SubscriptionOptions sub_opts;
  sub_opts.callback_group = cbg;

  std::shared_ptr<rclcpp::SerializedMessage> received;
  std::mutex mtx;
  auto sub = std::make_shared<agnocast::GenericSubscription>(
    node_.get(), topic, type, qos,
    [&received, &mtx](std::shared_ptr<rclcpp::SerializedMessage> msg) {
      std::lock_guard<std::mutex> lock(mtx);
      received = std::move(msg);
    },
    sub_opts);

  // Publish
  auto loaned = pub->borrow_loaned_message();
  loaned->data = expected_data;
  pub->publish(std::move(loaned));

  // Wait for delivery
  const bool delivered = wait_for([&] {
    std::lock_guard<std::mutex> lock(mtx);
    return received != nullptr;
  });

  ASSERT_TRUE(delivered) << "Timed out waiting for GenericSubscription callback";

  // Round-trip: deserialize back to String and compare
  rclcpp::Serialization<StringMsg> serializer;
  StringMsg decoded;
  std::lock_guard<std::mutex> lock(mtx);
  serializer.deserialize_message(received.get(), &decoded);
  EXPECT_EQ(decoded.data, expected_data);
}

// ---------------------------------------------------------------------------
// Test 2: destructor unregisters from id2_callback_info
// ---------------------------------------------------------------------------
TEST_F(GenericSubscriptionIntegrationTest, destructor_unregisters_callback_info)
{
  const std::string topic = "/test_generic_sub_lifecycle";
  const std::string type = "std_msgs/msg/String";
  rclcpp::QoS qos{1};

  const size_t size_before = [&] {
    std::lock_guard<std::mutex> lock(agnocast::id2_callback_info_mtx);
    return agnocast::id2_callback_info.size();
  }();

  {
    auto cbg = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    agnocast::SubscriptionOptions sub_opts;
    sub_opts.callback_group = cbg;
    auto sub = std::make_shared<agnocast::GenericSubscription>(
      node_.get(), topic, type, qos, [](std::shared_ptr<rclcpp::SerializedMessage>) {}, sub_opts);

    const size_t size_during = [&] {
      std::lock_guard<std::mutex> lock(agnocast::id2_callback_info_mtx);
      return agnocast::id2_callback_info.size();
    }();

    EXPECT_EQ(size_during, size_before + 1)
      << "GenericSubscription constructor must register one entry in id2_callback_info";
  }  // sub destroyed here

  const size_t size_after = [&] {
    std::lock_guard<std::mutex> lock(agnocast::id2_callback_info_mtx);
    return agnocast::id2_callback_info.size();
  }();

  EXPECT_EQ(size_after, size_before)
    << "GenericSubscription destructor must erase its entry from id2_callback_info";
}

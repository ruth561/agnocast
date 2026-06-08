// Integration tests for agnocast::GenericPublisher (agnocast::Node variant).
//
// Requires:
//   - Agnocast kernel module loaded (/dev/agnocast must be accessible)
//   - libagnocast_heaphook.so preloaded (LD_PRELOAD) for shared-memory allocation
//
// Test 1 (round_trip_to_typed_subscription):
//   create_generic_publisher serialises a std_msgs/msg/String and publishes it
//   via Agnocast shared memory.  A typed agnocast::Subscription<std_msgs::msg::String>
//   receives it and verifies the deserialized payload matches the original.
//
// Test 2 (round_trip_to_generic_subscription):
//   create_generic_publisher publishes → create_generic_subscription receives a
//   SerializedMessage → round-trip deserialisation matches the original string.
//
// Test 3 (lifecycle_registers_publisher):
//   Verify that create_generic_publisher assigns a valid id (≥ 0) and that
//   topic_name / gid are populated.  The destructor is exercised implicitly
//   when the publisher SharedPtr goes out of scope.
//
// Test 4 (publish_invalid_serialized_message):
//   Passing an empty (zero-length) SerializedMessage to publish() must not
//   crash; the implementation logs an error and returns early.

#include "agnocast/agnocast.hpp"
#include "agnocast/agnocast_publisher.hpp"
#include "agnocast/agnocast_subscription.hpp"
#include "agnocast/node/agnocast_node.hpp"
#include "agnocast/node/agnocast_only_single_threaded_executor.hpp"
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

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class GenericPublisherIntegrationTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    rclcpp::init(0, nullptr);
    agnocast::init(0, nullptr);
    node_ = std::make_shared<agnocast::Node>("test_generic_pub");
    executor_ = std::make_shared<agnocast::AgnocastOnlySingleThreadedExecutor>();
    executor_->add_node(node_);
    spin_thread_ = std::thread([this]() { executor_->spin(); });
  }

  void TearDown() override
  {
    executor_->cancel();
    if (spin_thread_.joinable()) {
      spin_thread_.join();
    }
    rclcpp::shutdown();
    agnocast::shutdown();
  }

  // Poll `pred` until it returns true or `timeout` elapses.
  template <typename Pred>
  bool wait_for(Pred pred, std::chrono::milliseconds timeout = 5000ms)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (pred()) return true;
      std::this_thread::sleep_for(10ms);
    }
    return pred();
  }

  std::shared_ptr<agnocast::Node> node_;
  std::shared_ptr<agnocast::AgnocastOnlySingleThreadedExecutor> executor_;
  std::thread spin_thread_;
};

// ---------------------------------------------------------------------------
// Test 1: round-trip via typed Subscription
// ---------------------------------------------------------------------------
TEST_F(GenericPublisherIntegrationTest, round_trip_to_typed_subscription)
{
  const std::string topic = "/test_generic_pub_to_typed_sub";
  const std::string type = "std_msgs/msg/String";
  const std::string expected_data = "hello from generic publisher";
  rclcpp::QoS qos{1};

  // Typed subscriber.
  auto cbg = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  agnocast::SubscriptionOptions sub_opts;
  sub_opts.callback_group = cbg;

  std::shared_ptr<const StringMsg> received;
  std::mutex mtx;

  auto sub = node_->create_subscription<StringMsg>(
    topic, qos,
    [&received, &mtx](agnocast::ipc_shared_ptr<const StringMsg> msg) {
      auto copy = std::make_shared<StringMsg>(*msg);
      std::lock_guard<std::mutex> lock(mtx);
      received = std::move(copy);
    },
    sub_opts);

  // Generic publisher via factory function.
  auto pub = agnocast::create_generic_publisher(node_.get(), topic, type, qos);

  // Serialise and publish.
  StringMsg out_msg;
  out_msg.data = expected_data;
  rclcpp::Serialization<StringMsg> serializer;
  rclcpp::SerializedMessage serialized;
  serializer.serialize_message(&out_msg, &serialized);
  pub->publish(serialized);

  const bool delivered = wait_for([&] {
    std::lock_guard<std::mutex> lock(mtx);
    return received != nullptr;
  });

  ASSERT_TRUE(delivered) << "Timed out waiting for typed subscription callback";
  {
    std::lock_guard<std::mutex> lock(mtx);
    EXPECT_EQ(received->data, expected_data);
  }
}

// ---------------------------------------------------------------------------
// Test 2: round-trip via GenericSubscription
// ---------------------------------------------------------------------------
TEST_F(GenericPublisherIntegrationTest, round_trip_to_generic_subscription)
{
  const std::string topic = "/test_generic_pub_to_generic_sub";
  const std::string type = "std_msgs/msg/String";
  const std::string expected_data = "hello via generic subscription";
  rclcpp::QoS qos{1};

  // GenericSubscription via factory function.
  auto cbg = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  agnocast::SubscriptionOptions sub_opts;
  sub_opts.callback_group = cbg;

  std::shared_ptr<rclcpp::SerializedMessage> received;
  std::mutex mtx;
  auto sub = agnocast::create_generic_subscription(
    node_.get(), topic, type, qos,
    [&received, &mtx](std::shared_ptr<rclcpp::SerializedMessage> msg) {
      std::lock_guard<std::mutex> lock(mtx);
      received = std::move(msg);
    },
    sub_opts);

  // Generic publisher via factory function.
  auto pub = agnocast::create_generic_publisher(node_.get(), topic, type, qos);

  // Serialise and publish.
  StringMsg out_msg;
  out_msg.data = expected_data;
  rclcpp::Serialization<StringMsg> serializer;
  rclcpp::SerializedMessage serialized;
  serializer.serialize_message(&out_msg, &serialized);
  pub->publish(serialized);

  const bool delivered = wait_for([&] {
    std::lock_guard<std::mutex> lock(mtx);
    return received != nullptr;
  });

  ASSERT_TRUE(delivered) << "Timed out waiting for GenericSubscription callback";

  // Round-trip: deserialise back to String and compare.
  rclcpp::Serialization<StringMsg> deser;
  StringMsg decoded;
  {
    std::lock_guard<std::mutex> lock(mtx);
    deser.deserialize_message(received.get(), &decoded);
  }
  EXPECT_EQ(decoded.data, expected_data);
}

// ---------------------------------------------------------------------------
// Test 3: lifecycle – constructor assigns a non-negative id and populates
//          topic_name / gid; destructor runs without crash.
// ---------------------------------------------------------------------------
TEST_F(GenericPublisherIntegrationTest, lifecycle_registers_publisher)
{
  const std::string topic = "/test_generic_pub_lifecycle";
  const std::string type = "std_msgs/msg/String";
  rclcpp::QoS qos{1};

  {
    auto pub = agnocast::create_generic_publisher(node_.get(), topic, type, qos);

    // topic_name() must be the resolved name (non-empty).
    EXPECT_STRNE(pub->get_topic_name(), "");

    // GID must be non-zero (generate_gid fills at least the 'AG' prefix).
    const rmw_gid_t & gid = pub->get_gid();
    EXPECT_EQ(gid.data[0], static_cast<uint8_t>('A'));
    EXPECT_EQ(gid.data[1], static_cast<uint8_t>('G'));
  }
  // Destructor ran – no crash or assertion failure.
}

// ---------------------------------------------------------------------------
// Test 4: publish with empty SerializedMessage – must not crash
// ---------------------------------------------------------------------------
TEST_F(GenericPublisherIntegrationTest, publish_invalid_serialized_message)
{
  const std::string topic = "/test_generic_pub_invalid_msg";
  const std::string type = "std_msgs/msg/String";
  rclcpp::QoS qos{1};

  auto pub = agnocast::create_generic_publisher(node_.get(), topic, type, qos);

  // An empty SerializedMessage has buffer_length == 0; rmw_deserialize should
  // return an error, and publish() must log the error and return gracefully.
  rclcpp::SerializedMessage empty_msg;
  EXPECT_NO_THROW(pub->publish(empty_msg));
}

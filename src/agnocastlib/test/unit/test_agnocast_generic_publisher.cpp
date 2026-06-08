// Tests for agnocast::GenericPublisher.
//
// These tests run entirely in userspace without the kernel module.  The
// agnocast internals (initialize_publisher, publish_core, etc.) are replaced
// by the mocks defined in test_mocked_agnocast.cpp which is compiled into the
// same test target.
//
// Two test cases are covered here:
//   1. typesupport_error_unknown_type  – Passing an unrecognised topic_type
//      string throws std::runtime_error before any kernel state is created.
//   2. publish_calls_publish_core       – Calling publish() with a valid
//      serialised std_msgs/msg/String forwards the message to publish_core
//      exactly once.
//
// Note: rmw_serialize / rmw_deserialize are real RMW calls; rclcpp::init /
// rclcpp::shutdown are called around each fixture to ensure the RMW layer is
// initialised (following the same pattern used by test_agnocast_generic_callback.cpp).

#include "agnocast/agnocast.hpp"
#include "agnocast/agnocast_publisher.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp/serialization.hpp"
#include "rosidl_typesupport_cpp/message_type_support.hpp"

#include "std_msgs/msg/string.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

// Defined in test_mocked_agnocast.cpp (same compilation unit / test target).
extern int publish_core_mock_called_count;

namespace
{
const std::string kKnownType = "std_msgs/msg/String";
const std::string kUnknownType = "totally_fake_package/msg/DoesNotExist";
}  // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class GenericPublisherUnitTest : public ::testing::Test
{
protected:
  void SetUp() override { rclcpp::init(0, nullptr); }
  void TearDown() override { rclcpp::shutdown(); }
};

// ---------------------------------------------------------------------------
// Test 1: unknown topic type → std::runtime_error before ioctl
// ---------------------------------------------------------------------------
TEST_F(GenericPublisherUnitTest, typesupport_error_unknown_type)
{
  auto node = std::make_shared<rclcpp::Node>("test_generic_pub_unknown_type");

  EXPECT_THROW(
    agnocast::GenericPublisher(node.get(), "/test_topic", kUnknownType, rclcpp::QoS{1}),
    std::runtime_error);
}

// ---------------------------------------------------------------------------
// Test 2: publish() forwards a valid serialised message to publish_core once
// ---------------------------------------------------------------------------
TEST_F(GenericPublisherUnitTest, publish_calls_publish_core)
{
  auto node = std::make_shared<rclcpp::Node>("test_generic_pub_publish");

  agnocast::GenericPublisher pub(
    node.get(), "/test_generic_pub_string", kKnownType, rclcpp::QoS{1});

  // Serialise a std_msgs/msg/String message.
  std_msgs::msg::String msg;
  msg.data = "hello from generic publisher unit test";
  rclcpp::Serialization<std_msgs::msg::String> serializer;
  rclcpp::SerializedMessage serialized;
  serializer.serialize_message(&msg, &serialized);

  publish_core_mock_called_count = 0;
  pub.publish(serialized);

  EXPECT_EQ(publish_core_mock_called_count, 1);
}

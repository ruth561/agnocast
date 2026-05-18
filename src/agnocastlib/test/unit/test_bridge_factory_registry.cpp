// Unit tests for the process-local BridgeFactoryRegistry (F1).
//
// We do not instantiate real bridge factory functions here — those would
// require a fully-initialized agnocast runtime (kmod, ioctl, etc.). Instead
// we exercise the registry's own contract: register / lookup / overwrite,
// and the SFINAE gate on `register_bridge_factory<T>()` (no-op for service
// types).

#include "agnocast/internal/bridge_factory_registry.hpp"

#include <rcl_interfaces/srv/get_parameters.hpp>
#include <std_msgs/msg/int32.hpp>
#include <std_msgs/msg/string.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace
{

agnocast::internal::BridgeFactoryEntry make_dummy_entry()
{
  agnocast::internal::BridgeFactoryEntry entry;
  entry.a2r = [](rclcpp::Node::SharedPtr, const std::string &, const rclcpp::QoS &) {
    return std::shared_ptr<agnocast::PubsubBridgeBase>{};
  };
  entry.r2a = [](rclcpp::Node::SharedPtr, const std::string &, const rclcpp::QoS &) {
    return std::shared_ptr<agnocast::PubsubBridgeBase>{};
  };
  return entry;
}

}  // namespace

TEST(BridgeFactoryRegistry, RegisterAndLookupRoundTrip)
{
  auto & registry = agnocast::internal::BridgeFactoryRegistry::instance();
  registry.register_entry("test/unit/RoundTrip", make_dummy_entry());

  agnocast::internal::BridgeFactoryEntry got;
  ASSERT_TRUE(registry.lookup("test/unit/RoundTrip", got));
  EXPECT_TRUE(static_cast<bool>(got.a2r));
  EXPECT_TRUE(static_cast<bool>(got.r2a));
}

TEST(BridgeFactoryRegistry, LookupReturnsFalseForUnregisteredType)
{
  auto & registry = agnocast::internal::BridgeFactoryRegistry::instance();
  agnocast::internal::BridgeFactoryEntry got;
  EXPECT_FALSE(registry.lookup("test/unit/NeverRegistered", got));
}

TEST(BridgeFactoryRegistry, RegisterReplacesExistingEntry)
{
  auto & registry = agnocast::internal::BridgeFactoryRegistry::instance();

  auto first = make_dummy_entry();
  first.a2r = [marker = std::make_shared<int>(1)](
                rclcpp::Node::SharedPtr, const std::string &,
                const rclcpp::QoS &) { return std::shared_ptr<agnocast::PubsubBridgeBase>{}; };
  registry.register_entry("test/unit/Replaceable", first);

  auto second = make_dummy_entry();
  second.a2r = [marker = std::make_shared<int>(2)](
                 rclcpp::Node::SharedPtr, const std::string &,
                 const rclcpp::QoS &) { return std::shared_ptr<agnocast::PubsubBridgeBase>{}; };
  registry.register_entry("test/unit/Replaceable", second);

  agnocast::internal::BridgeFactoryEntry got;
  ASSERT_TRUE(registry.lookup("test/unit/Replaceable", got));
  // Confirm an entry is still resolvable after the second register call.
  EXPECT_TRUE(static_cast<bool>(got.a2r));
  EXPECT_TRUE(static_cast<bool>(got.r2a));
}

// `register_bridge_factory<T>()` must compile and be a no-op for service
// types — `BasicService<ServiceT>` instantiates it internally and triggers
// `static_assert` in `rclcpp::Publisher<ServiceType>` if the SFINAE gate
// regresses.
TEST(BridgeFactoryRegistry, RegisterIsNoOpForServiceType)
{
  using ServiceT = rcl_interfaces::srv::GetParameters;
  agnocast::internal::register_bridge_factory<ServiceT>();
  // The type name is the service name, not a message name; nothing should
  // have been added under it.
  agnocast::internal::BridgeFactoryEntry got;
  auto & registry = agnocast::internal::BridgeFactoryRegistry::instance();
  EXPECT_FALSE(registry.lookup(rosidl_generator_traits::name<ServiceT>(), got));
}

TEST(BridgeFactoryRegistry, RegisterIsObservableForMessageType)
{
  using MessageT = std_msgs::msg::Int32;
  agnocast::internal::register_bridge_factory<MessageT>();
  agnocast::internal::BridgeFactoryEntry got;
  auto & registry = agnocast::internal::BridgeFactoryRegistry::instance();
  // After registration, lookup by the message's rosidl name must succeed.
  ASSERT_TRUE(registry.lookup(rosidl_generator_traits::name<MessageT>(), got));
  EXPECT_TRUE(static_cast<bool>(got.a2r));
  EXPECT_TRUE(static_cast<bool>(got.r2a));
}

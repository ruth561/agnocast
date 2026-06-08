#pragma once

#include "agnocast/agnocast_ioctl.hpp"
#include "agnocast/agnocast_mq.hpp"
#include "agnocast/agnocast_public_api.hpp"
#include "agnocast/agnocast_smart_pointer.hpp"
#include "agnocast/agnocast_tracepoint_wrapper.h"
#include "agnocast/agnocast_utils.hpp"
#include "rclcpp/detail/qos_parameters.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp/serialized_message.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"

#include <fcntl.h>
#include <mqueue.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace agnocast
{
class Node;

const void * get_node_base_address(Node * node);

// These are cut out of the class for information hiding.
topic_local_id_t initialize_publisher(
  const std::string & topic_name, const std::string & node_name, const rclcpp::QoS & qos,
  const bool is_bridge, const std::string & type_name);
union ioctl_publish_msg_args publish_core(
  [[maybe_unused]] const void * publisher_handle, /* for CARET */ const std::string & topic_name,
  const topic_local_id_t publisher_id, const uint64_t msg_virtual_address,
  std::unordered_map<topic_local_id_t, std::tuple<mqd_t, bool>> & opened_mqs);
uint32_t get_subscription_count_core(const std::string & topic_name);
uint32_t get_intra_subscription_count_core(const std::string & topic_name);
void increment_borrowed_publisher_num();
void decrement_borrowed_publisher_num();

extern int agnocast_fd;
extern "C" uint32_t agnocast_get_borrowed_publisher_num();

/**
 * @brief Options for configuring an Agnocast publisher.
 */
AGNOCAST_PUBLIC
struct PublisherOptions
{
  /// @deprecated Use the `AGNOCAST_BRIDGE_MODE` environment variable instead.
  bool do_always_ros2_publish = false;
  /// QoS parameter override options (same semantics as rclcpp).
  rclcpp::QosOverridingOptions qos_overriding_options{};
};

// Internal implementation — users should use agnocast::Publisher<MessageT> instead.
template <typename MessageT, typename BridgeRequestPolicy>
class BasicPublisher
{
  topic_local_id_t id_ = -1;
  std::string topic_name_;
  std::unordered_map<topic_local_id_t, std::tuple<mqd_t, bool>> opened_mqs_;
  std::mutex opened_mqs_mtx_;
  rmw_gid_t gid_;

  void generate_gid()
  {
    std::memset(gid_.data, 0, RMW_GID_STORAGE_SIZE);

    // [0-1]: Agnocast identifier
    gid_.data[0] = 'A';
    gid_.data[1] = 'G';

    // [2-5]: Process ID
    pid_t pid = getpid();
    std::memcpy(gid_.data + 2, &pid, sizeof(pid));

    // [6-11]: topic_name hash (upper 6 bytes)
    size_t topic_hash = std::hash<std::string>{}(topic_name_);
    std::memcpy(gid_.data + 6, &topic_hash, 6);

    // [12-15]: publisher id
    std::memcpy(gid_.data + 12, &id_, sizeof(id_));

    // [16-23]: reserved

    gid_.implementation_identifier = "agnocast";
  }

  template <typename NodeT>
  rclcpp::QoS constructor_impl(
    NodeT * node, const std::string & topic_name, const rclcpp::QoS & qos,
    const PublisherOptions & options, const bool is_bridge)
  {
    if (options.do_always_ros2_publish) {
      RCLCPP_ERROR(
        logger,
        "The 'do_always_ros2_publish' option is deprecated. "
        "Use the AGNOCAST_BRIDGE_MODE environment variable instead.");
    }

    topic_name_ = node->get_node_topics_interface()->resolve_topic_name(topic_name);

    auto node_parameters = node->get_node_parameters_interface();
    const rclcpp::QoS actual_qos =
      options.qos_overriding_options.get_policy_kinds().size()
        ? rclcpp::detail::declare_qos_parameters(
            options.qos_overriding_options, node_parameters, topic_name_, qos,
            rclcpp::detail::PublisherQosParametersTraits{})
        : qos;

    validate_publisher_qos(actual_qos);

    const std::string node_name = node->get_fully_qualified_name();
    // Gated to message types only — service types pulled in by
    // BasicService<ServiceT> have no rosidl message name. The empty string
    // signals "skip registry" to initialize_publisher.
    std::string type_name;
    if constexpr (rosidl_generator_traits::is_message<MessageT>::value) {
      type_name = rosidl_generator_traits::name<MessageT>();
    }
    id_ = initialize_publisher(topic_name_, node_name, actual_qos, is_bridge, type_name);
    generate_gid();
    BridgeRequestPolicy::template request_bridge<MessageT>(topic_name_, id_);

    return actual_qos;
  }

public:
  using SharedPtr = std::shared_ptr<BasicPublisher<MessageT, BridgeRequestPolicy>>;

  BasicPublisher(
    rclcpp::Node * node, const std::string & topic_name, const rclcpp::QoS & qos,
    const PublisherOptions & options, const bool is_bridge = false)
  {
    const rclcpp::QoS actual_qos = constructor_impl(node, topic_name, qos, options, is_bridge);

    TRACEPOINT(
      agnocast_publisher_init, static_cast<const void *>(this),
      static_cast<const void *>(
        node->get_node_base_interface()->get_shared_rcl_node_handle().get()),
      topic_name_.c_str(), actual_qos.depth());
  }

  BasicPublisher(
    agnocast::Node * node, const std::string & topic_name, const rclcpp::QoS & qos,
    const PublisherOptions & options = PublisherOptions{})
  {
    const rclcpp::QoS actual_qos = constructor_impl(node, topic_name, qos, options, false);

    TRACEPOINT(
      agnocast_publisher_init, static_cast<const void *>(this),
      static_cast<const void *>(get_node_base_address(node)), topic_name_.c_str(),
      actual_qos.depth());
  }

  ~BasicPublisher()
  {
    for (auto & [_, t] : opened_mqs_) {
      mqd_t mq = std::get<0>(t);
      if (mq_close(mq) == -1) {
        RCLCPP_ERROR_STREAM(
          logger, "mq_close failed for topic '" << topic_name_ << "': " << strerror(errno));
      }
    }

    // NOTE: When a publisher is destroyed, subscribers should unmap its memory, but this is not yet
    // implemented. Since multiple publishers in the same process share a mempool, process-level
    // reference counting in kmod is needed. Leaving memory mapped causes no functional issues, so
    // this is left as future work.
    struct ioctl_remove_publisher_args remove_publisher_args
    {
    };
    remove_publisher_args.topic_name = {topic_name_.c_str(), topic_name_.size()};
    remove_publisher_args.publisher_id = id_;
    if (ioctl(agnocast_fd, AGNOCAST_REMOVE_PUBLISHER_CMD, &remove_publisher_args) < 0) {
      RCLCPP_WARN(logger, "Failed to remove publisher (id=%d) from kernel.", id_);
    }
  }

  /**
   * @brief Allocate a new default-constructed message in shared memory. The caller must either
   * pass the returned pointer to publish() or let it go out of scope (which frees the memory).
   *
   * @return Owned pointer to the newly allocated message in shared memory.
   */
  AGNOCAST_PUBLIC
  ipc_shared_ptr<MessageT> borrow_loaned_message()
  {
    increment_borrowed_publisher_num();
    MessageT * ptr = new MessageT();
    return ipc_shared_ptr<MessageT>(ptr, topic_name_.c_str(), id_);
  }

  /**
   * @brief Publish a message via zero-copy IPC. Ownership is transferred: after this call, the
   * passed-in ipc_shared_ptr and all copies sharing its control block are invalidated —
   * dereferencing them calls std::terminate().
   *
   * @param message Message obtained from borrow_loaned_message(). Must be moved in.
   */
  AGNOCAST_PUBLIC
  void publish(ipc_shared_ptr<MessageT> && message)
  {
    if (!message || topic_name_ != message.get_topic_name()) {
      RCLCPP_ERROR(logger, "Invalid message to publish.");
      close(agnocast_fd);
      exit(EXIT_FAILURE);
    }

    // Capture raw pointer BEFORE invalidation (get() returns nullptr after invalidation).
    const uint64_t msg_virtual_address = reinterpret_cast<uint64_t>(message.get());

    // Invalidate all references sharing this handle's control block.
    // Any remaining copies held elsewhere will fail-fast on dereference.
    message.invalidate_all_references();

    decrement_borrowed_publisher_num();

    union ioctl_publish_msg_args publish_msg_args;
    {
      std::lock_guard<std::mutex> lock(opened_mqs_mtx_);
      publish_msg_args = publish_core(this, topic_name_, id_, msg_virtual_address, opened_mqs_);
    }

    for (uint32_t i = 0; i < publish_msg_args.ret_released_num; i++) {
      MessageT * release_ptr = reinterpret_cast<MessageT *>(publish_msg_args.ret_released_addrs[i]);
      delete release_ptr;
    }

    message.reset();
  }

  /**
   * @brief Return the total subscriber count for this topic (Agnocast + ROS 2 via bridge).
   * @return Total subscriber count.
   */
  AGNOCAST_PUBLIC
  uint32_t get_subscription_count() const { return get_subscription_count_core(topic_name_); }

  /**
   * @brief Return the GID of this publisher, unique across both Agnocast and ROS 2.
   * @return Publisher GID.
   */
  AGNOCAST_PUBLIC
  const rmw_gid_t & get_gid() const { return gid_; }

  /**
   * @brief Return the number of Agnocast intra-process subscribers only (excludes ROS 2).
   * @return Agnocast subscriber count.
   */
  AGNOCAST_PUBLIC
  uint32_t get_intra_subscription_count() const
  {
    return get_intra_subscription_count_core(topic_name_);
  }

  /**
   * @brief Return the fully-resolved topic name.
   * @return Null-terminated topic name string.
   */
  AGNOCAST_PUBLIC
  const char * get_topic_name() const { return topic_name_.c_str(); }
};

struct AgnocastToRosPubsubRequestPolicy;

/**
 * @brief The user-facing Agnocast publisher type.
 * Alias for `BasicPublisher<MessageT>`. Use this type (not BasicPublisher directly) when declaring
 * publisher variables.
 * @tparam MessageT  ROS message type.
 */
AGNOCAST_PUBLIC
template <typename MessageT>
using Publisher = agnocast::BasicPublisher<MessageT, agnocast::AgnocastToRosPubsubRequestPolicy>;

/// @brief Event-driven generic (type-erased) publisher that accepts an
/// `rclcpp::SerializedMessage`, deserializes it into Agnocast shared memory,
/// and publishes via zero-copy IPC.
///
/// Mirrors `rclcpp::GenericPublisher` semantics: the topic type is supplied as
/// a runtime string (e.g. `"std_msgs/msg/String"`) rather than a compile-time
/// template argument. Two typesupport libraries are loaded eagerly in the
/// constructor and held for the publisher's lifetime:
///   - `rosidl_typesupport_cpp`               — used by `rmw_deserialize`
///   - `rosidl_typesupport_introspection_cpp`  — provides `size_of_`,
///     `init_function`, and `fini_function` for type-erased allocation
///
/// **A2R bridge behavior** (forwards messages from Agnocast to a ROS 2
/// subscriber on the same topic):
/// - `BridgeMode::Performance`: an A2R bridge is requested using the runtime
///   `topic_type` string.
/// - `BridgeMode::Standard`: not yet supported. A warning is logged and the
///   bridge request is skipped.
/// - `BridgeMode::Off`: no bridge request is made.
///
/// The `is_bridge` constructor parameter must be set to `true` only when this
/// publisher is the sending end of a bridge node (i.e. a generic A2R bridge).
/// End users should leave it at the default (`false`).
AGNOCAST_PUBLIC
class GenericPublisher
{
  topic_local_id_t id_{-1};
  std::string topic_name_;
  std::unordered_map<topic_local_id_t, std::tuple<mqd_t, bool>> opened_mqs_;
  std::mutex opened_mqs_mtx_;
  rmw_gid_t gid_;

  /// Keeps the `rosidl_typesupport_cpp` .so alive — used by `rmw_deserialize`.
  std::shared_ptr<rcpputils::SharedLibrary> ts_lib_;
  /// Points into ts_lib_ — valid as long as ts_lib_ is alive.
  const rosidl_message_type_support_t * type_support_handle_{nullptr};

  /// Keeps the `rosidl_typesupport_introspection_cpp` .so alive.
  std::shared_ptr<rcpputils::SharedLibrary> ts_lib_introspection_;
  /// Points into ts_lib_introspection_ — provides size_of_ / init_function / fini_function.
  const rosidl_typesupport_introspection_cpp::MessageMembers * members_{nullptr};

  void generate_gid();

  template <typename NodeT>
  rclcpp::QoS constructor_impl(
    NodeT * node, const std::string & topic_type, const rclcpp::QoS & qos,
    const agnocast::PublisherOptions & options, bool is_bridge);

public:
  using SharedPtr = std::shared_ptr<GenericPublisher>;

  AGNOCAST_PUBLIC
  GenericPublisher(
    rclcpp::Node * node, const std::string & topic_name, const std::string & topic_type,
    const rclcpp::QoS & qos, agnocast::PublisherOptions options = agnocast::PublisherOptions{},
    bool is_bridge = false);

  AGNOCAST_PUBLIC
  GenericPublisher(
    agnocast::Node * node, const std::string & topic_name, const std::string & topic_type,
    const rclcpp::QoS & qos, agnocast::PublisherOptions options = agnocast::PublisherOptions{});

  ~GenericPublisher();

  /// @brief Publish a serialized message. The message is deserialized into
  /// Agnocast shared memory and forwarded to all subscribers.
  AGNOCAST_PUBLIC
  void publish(const rclcpp::SerializedMessage & serialized_msg);

  /// @brief Convenience overload — delegates to `publish(*serialized_msg)`.
  AGNOCAST_PUBLIC
  void publish(std::shared_ptr<rclcpp::SerializedMessage> serialized_msg);

  /// @brief Return the fully-resolved topic name.
  AGNOCAST_PUBLIC
  const char * get_topic_name() const { return topic_name_.c_str(); }

  /// @brief Return the total subscriber count for this topic (Agnocast + ROS 2 via bridge).
  AGNOCAST_PUBLIC
  uint32_t get_subscription_count() const { return get_subscription_count_core(topic_name_); }

  /// @brief Return the number of Agnocast intra-process subscribers only.
  AGNOCAST_PUBLIC
  uint32_t get_intra_subscription_count() const
  {
    return get_intra_subscription_count_core(topic_name_);
  }

  /// @brief Return the GID of this publisher, unique across both Agnocast and ROS 2.
  AGNOCAST_PUBLIC
  const rmw_gid_t & get_gid() const { return gid_; }
};

}  // namespace agnocast

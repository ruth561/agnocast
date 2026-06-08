#include "agnocast/agnocast.hpp"
#include "agnocast/internal/type_registry_writer.hpp"
#include "agnocast/node/agnocast_node.hpp"
#include "rclcpp/typesupport_helpers.hpp"
#include "rcpputils/shared_library.hpp"

namespace agnocast
{

SubscriptionBase::SubscriptionBase(rclcpp::Node * node, const std::string & topic_name)
: id_(0), topic_name_(node->get_node_topics_interface()->resolve_topic_name(topic_name))
{
  validate_ld_preload();
}

SubscriptionBase::SubscriptionBase(
  agnocast::Node * node, const std::string & topic_name)  // NOLINT(modernize-pass-by-value)
: id_(0), topic_name_(node->get_node_topics_interface()->resolve_topic_name(topic_name))
{
  validate_ld_preload();
}

union ioctl_add_subscriber_args SubscriptionBase::initialize(
  const rclcpp::QoS & qos, const bool is_take_sub, const bool ignore_local_publications,
  const bool is_bridge, const std::string & node_name, const std::string & type_name)
{
  // Announce to the per-IPC-namespace discovery agent before the kmod call so
  // the registry line is in place whenever a later snapshot sees the
  // ioctl-side endpoint. Empty `type_name` (e.g. service types) skips this.
  if (!type_name.empty()) {
    internal::TypeRegistryWriter::instance().register_type(
      topic_name_, type_name, "sub", node_name);
  }

  union ioctl_add_subscriber_args add_subscriber_args = {};
  add_subscriber_args.topic_name = {topic_name_.c_str(), topic_name_.size()};
  add_subscriber_args.node_name = {node_name.c_str(), node_name.size()};
  add_subscriber_args.qos_depth = static_cast<uint32_t>(qos.depth());
  add_subscriber_args.qos_is_transient_local =
    qos.durability() == rclcpp::DurabilityPolicy::TransientLocal;
  add_subscriber_args.qos_is_reliable = qos.reliability() == rclcpp::ReliabilityPolicy::Reliable;
  add_subscriber_args.is_take_sub = is_take_sub;
  add_subscriber_args.ignore_local_publications = ignore_local_publications;
  add_subscriber_args.is_bridge = is_bridge;
  if (ioctl(agnocast_fd, AGNOCAST_ADD_SUBSCRIBER_CMD, &add_subscriber_args) < 0) {
    RCLCPP_ERROR(logger, "AGNOCAST_ADD_SUBSCRIBER_CMD failed: %s", strerror(errno));
    close(agnocast_fd);
    exit(EXIT_FAILURE);
  }

  return add_subscriber_args;
}

uint32_t get_publisher_count_core(const std::string & topic_name)
{
  union ioctl_get_publisher_num_args args = {};
  args.topic_name = {topic_name.c_str(), topic_name.size()};
  if (ioctl(agnocast_fd, AGNOCAST_GET_PUBLISHER_NUM_CMD, &args) < 0) {
    RCLCPP_ERROR(logger, "AGNOCAST_GET_PUBLISHER_NUM_CMD failed: %s", strerror(errno));
    close(agnocast_fd);
    exit(EXIT_FAILURE);
  }

  uint32_t count = args.ret_publisher_num;
  // If an R2A bridge exists, exclude the agnocast publisher created by the bridge
  if (args.ret_r2a_bridge_exist && count > 0) {
    count--;
  }

  uint32_t ros2_count = args.ret_ros2_publisher_num;
  // If an A2R bridge exists, exclude the ROS 2 publisher created by the bridge
  if (args.ret_a2r_bridge_exist && ros2_count > 0) {
    ros2_count--;
  }

  return count + ros2_count;
}

mqd_t open_mq_for_subscription(
  const std::string & topic_name, const topic_local_id_t subscriber_id,
  std::pair<mqd_t, std::string> & mq_subscription)
{
  std::string mq_name = create_mq_name_for_agnocast_publish(topic_name, subscriber_id);
  struct mq_attr attr = {};
  attr.mq_flags = 0;                        // Blocking queue
  attr.mq_msgsize = sizeof(MqMsgAgnocast);  // Maximum message size
  attr.mq_curmsgs = 0;  // Number of messages currently in the queue (not set by mq_open)
  attr.mq_maxmsg = 1;

  const int mq_mode = 0666;
  mqd_t mq = mq_open(mq_name.c_str(), O_CREAT | O_RDONLY | O_NONBLOCK, mq_mode, &attr);
  if (mq == -1) {
    RCLCPP_ERROR_STREAM(
      logger, "mq_open failed for topic '" << topic_name << "' (subscriber_id=" << subscriber_id
                                           << ", mq_name='" << mq_name
                                           << "'): " << strerror(errno));
    close(agnocast_fd);
    exit(EXIT_FAILURE);
  }
  mq_subscription = std::make_pair(mq, mq_name);

  return mq;
}

void remove_mq(const std::pair<mqd_t, std::string> & mq_subscription)
{
  /* The message queue is destroyed after all the publisher processes close it. */
  if (mq_close(mq_subscription.first) == -1) {
    RCLCPP_ERROR_STREAM(
      logger,
      "mq_close failed for mq_name='" << mq_subscription.second << "': " << strerror(errno));
  }
  if (mq_unlink(mq_subscription.second.c_str()) == -1) {
    RCLCPP_ERROR_STREAM(
      logger,
      "mq_unlink failed for mq_name='" << mq_subscription.second << "': " << strerror(errno));
  }
}

rclcpp::CallbackGroup::SharedPtr get_default_callback_group_for_tracepoint(agnocast::Node * node)
{
  return node->get_node_base_interface()->get_default_callback_group();
}

// ---------------------------------------------------------------------------
// GenericSubscription
// ---------------------------------------------------------------------------

template <typename NodeT>
rclcpp::QoS GenericSubscription::constructor_impl(
  NodeT * node, const std::string & topic_type, const rclcpp::QoS & qos,
  std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> callback,
  rclcpp::CallbackGroup::SharedPtr callback_group, const agnocast::SubscriptionOptions & options,
  bool is_bridge)
{
  const bool override_qos = options.qos_overriding_options.get_policy_kinds().size() > 0;
  rclcpp::node_interfaces::NodeParametersInterface::SharedPtr node_parameters =
    override_qos ? node->get_node_parameters_interface() : nullptr;
  const rclcpp::QoS actual_qos = override_qos
                                   ? rclcpp::detail::declare_qos_parameters(
                                       options.qos_overriding_options, node_parameters, topic_name_,
                                       qos, rclcpp::detail::SubscriptionQosParametersTraits{})
                                   : qos;

  validate_subscription_qos(actual_qos);

  const std::string node_name = node->get_fully_qualified_name();

  // Load the typesupport library BEFORE calling initialize() (which registers
  // the subscriber with the kernel via ioctl). Both get_typesupport_library and
  // get_message_typesupport_handle throw std::runtime_error for unknown types.
  // By loading eagerly here, any such exception escapes the constructor before
  // any kernel-side or MQ state has been created, keeping the system clean.
  // If we deferred loading until after initialize(), a throw would leave a
  // half-registered subscriber in the kernel with no corresponding userspace
  // callback or message queue.
  ts_lib_ = rclcpp::get_typesupport_library(topic_type, "rosidl_typesupport_cpp");
  type_support_handle_ =
    rclcpp::get_message_typesupport_handle(topic_type, "rosidl_typesupport_cpp", *ts_lib_);

  union ioctl_add_subscriber_args add_subscriber_args = initialize(
    actual_qos, false, options.ignore_local_publications, is_bridge, node_name, topic_type);

  id_ = add_subscriber_args.ret_id;

  // Request an R2A bridge for this generic subscription. Skipped when this
  // subscription is itself the agnocast-side endpoint of a bridge node
  // (`is_bridge=true`); otherwise it would request a bridge against itself,
  // causing a self-loop or duplicate bridge.
  //
  // The non-template entry point is required because we only have a runtime
  // `topic_type` string here, not a compile-time `MessageT`. Behavior by mode:
  //   - Off:         no-op.
  //   - Performance: enqueues an MqMsgPerformanceBridge carrying the runtime
  //                  message type. The performance bridge manager will load
  //                  the corresponding `agnocast_bridge_plugins` factory.
  //   - Standard:    not yet supported (warn-and-skip). See the comment on
  //                  `request_pubsub_bridge_core_by_type_name` for the
  //                  rationale (function-pointer factories require
  //                  compile-time `MessageT`).
  if (!is_bridge) {
    request_pubsub_bridge_core_by_type_name(
      topic_name_, id_, topic_type, BridgeDirection::ROS2_TO_AGNOCAST);
  }

  mqd_t mq = open_mq_for_subscription(topic_name_, id_, mq_subscription_);

  const bool is_transient_local =
    actual_qos.durability() == rclcpp::DurabilityPolicy::TransientLocal;
  callback_info_id_ = agnocast::register_generic_callback(
    std::move(callback), type_support_handle_, topic_name_, id_, is_transient_local, mq,
    callback_group);

  return actual_qos;
}

GenericSubscription::GenericSubscription(
  rclcpp::Node * node, const std::string & topic_name, const std::string & topic_type,
  const rclcpp::QoS & qos, std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> callback,
  agnocast::SubscriptionOptions options, bool is_bridge)
: SubscriptionBase(node, topic_name)
{
  rclcpp::CallbackGroup::SharedPtr callback_group = get_valid_callback_group(node, options);

  const void * callback_addr = static_cast<const void *>(&callback);
  const char * callback_symbol = tracetools::get_symbol(callback);

  const rclcpp::QoS actual_qos = constructor_impl(
    node, topic_type, qos, std::move(callback), callback_group, options, is_bridge);

  {
    uint64_t pid_callback_info_id = (static_cast<uint64_t>(getpid()) << 32) | callback_info_id_;
    TRACEPOINT(
      agnocast_subscription_init, static_cast<const void *>(this),
      static_cast<const void *>(
        node->get_node_base_interface()->get_shared_rcl_node_handle().get()),
      callback_addr, static_cast<const void *>(callback_group.get()), callback_symbol,
      topic_name_.c_str(), actual_qos.depth(), pid_callback_info_id);
  }
}

GenericSubscription::GenericSubscription(
  agnocast::Node * node, const std::string & topic_name, const std::string & topic_type,
  const rclcpp::QoS & qos, std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> callback,
  agnocast::SubscriptionOptions options)
: SubscriptionBase(node, topic_name)
{
  rclcpp::CallbackGroup::SharedPtr callback_group = get_valid_callback_group(node, options);

  const void * callback_addr = static_cast<const void *>(&callback);
  const char * callback_symbol = tracetools::get_symbol(callback);

  const rclcpp::QoS actual_qos =
    constructor_impl(node, topic_type, qos, std::move(callback), callback_group, options, false);

  {
    uint64_t pid_callback_info_id = (static_cast<uint64_t>(getpid()) << 32) | callback_info_id_;
    TRACEPOINT(
      agnocast_subscription_init, static_cast<const void *>(this),
      static_cast<const void *>(get_node_base_address(node)), callback_addr,
      static_cast<const void *>(callback_group.get()), callback_symbol, topic_name_.c_str(),
      actual_qos.depth(), pid_callback_info_id);
  }
}

GenericSubscription::~GenericSubscription()
{
  {
    std::lock_guard<std::mutex> lock(id2_callback_info_mtx);
    id2_callback_info.erase(callback_info_id_);
  }
  remove_mq(mq_subscription_);
}

}  // namespace agnocast

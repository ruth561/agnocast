#pragma once

#include "agnocast/agnocast_ioctl.hpp"

#include <cstddef>
#include <cstdint>

namespace agnocast
{

inline pid_t standard_bridge_manager_pid = 0;
inline constexpr pid_t PERFORMANCE_BRIDGE_VIRTUAL_PID = -1;

inline constexpr size_t SHARED_LIB_PATH_BUFFER_SIZE = 4096;  // Linux PATH_MAX is 4096
inline constexpr size_t SYMBOL_NAME_BUFFER_SIZE = 256;
inline constexpr size_t SERVICE_NAME_BUFFER_SIZE = 256;
inline constexpr size_t MESSAGE_TYPE_BUFFER_SIZE = 256;
inline constexpr size_t SERVICE_TYPE_BUFFER_SIZE = 256;

inline constexpr const char * MAIN_EXECUTABLE_SYMBOL = "__MAIN_EXECUTABLE__";

enum class BridgeDirection : uint32_t { ROS2_TO_AGNOCAST = 0, AGNOCAST_TO_ROS2 = 1 };

struct MqMsgAgnocast
{
};

struct MqMsgROS2Publish
{
  bool should_terminate;
};

struct BridgeFactoryInfo
{
  char shared_lib_path[SHARED_LIB_PATH_BUFFER_SIZE];
  char symbol_name[SYMBOL_NAME_BUFFER_SIZE];
  uintptr_t fn_offset;
  uintptr_t fn_offset_reverse;
};

struct PubsubBridgeTargetInfo
{
  char topic_name[TOPIC_NAME_BUFFER_SIZE];
  topic_local_id_t target_id;
};

struct ServiceBridgeTargetInfo
{
  char service_name[SERVICE_NAME_BUFFER_SIZE];
  bool create_shadow_node;
  char shadow_node_namespace[NODE_NAME_BUFFER_SIZE];
  char shadow_node_name[NODE_NAME_BUFFER_SIZE];
};

struct MqMsgBridge
{
  BridgeFactoryInfo factory;
  union {
    PubsubBridgeTargetInfo pubsub_target;
    ServiceBridgeTargetInfo srv_target;
  };
  BridgeDirection direction;
  bool is_service;
};

struct PubsubBridgeTargetInfoWithType
{
  char message_type[MESSAGE_TYPE_BUFFER_SIZE];
  char topic_name[TOPIC_NAME_BUFFER_SIZE];
  topic_local_id_t target_id;
};

struct ServiceBridgeTargetInfoWithType
{
  char service_type[SERVICE_TYPE_BUFFER_SIZE];
  char service_name[SERVICE_NAME_BUFFER_SIZE];
  bool create_shadow_node;
  char shadow_node_namespace[NODE_NAME_BUFFER_SIZE];
  char shadow_node_name[NODE_NAME_BUFFER_SIZE];
};

struct MqMsgPerformanceBridge
{
  union {
    PubsubBridgeTargetInfoWithType pubsub_target;
    ServiceBridgeTargetInfoWithType srv_target;
  };
  BridgeDirection direction;
  bool is_service;
};

// Daemon-originated bridge request. Sent by the per-IPC daemon to a
// bridge_manager in the same IPC namespace. The msg is type-name based so the
// daemon can fill it without process-specific factory pointers (Standard mode
// resolves via process-local bridge factory registry; Performance mode resolves
// via the existing plugin loader).
//
// QoS is supplied explicitly because the receiving bridge_manager cannot
// always resolve it locally (the kernel exposes subscriber QoS by id but not
// publisher QoS without an id). The daemon already knows the QoS of every
// local endpoint via the same procfs read used to build the gossip payload.
struct MqMsgDaemonBridge
{
  char topic_name[TOPIC_NAME_BUFFER_SIZE];
  char type_name[MESSAGE_TYPE_BUFFER_SIZE];
  BridgeDirection direction;
  uint32_t qos_depth;
  bool qos_is_transient_local;
  bool qos_is_reliable;
};

// User process → Standard-mode bridge_manager pre-registration of the
// factory pair backing each `register_bridge_factory<T>()` call. The
// bridge_manager is forked from the user process by `agnocast::init()`
// *before* Publisher / Subscription ctors run, so it inherits an empty
// BridgeFactoryRegistry and needs to be told about every type the parent
// later registers. We don't send raw function pointers because
// composable_node libraries (e.g. `libagnocast_listener_component.so`)
// are typically dlopen()'d in the parent *after* the fork, so the
// addresses live in pages the bridge_manager has never mapped; we send
// (shared_lib_path, fn_offset_*) so the bridge_manager can dlopen the
// same library and reconstruct the address as `base + offset`. Same
// shape as the existing intra-NS MqMsgBridge / BridgeFactoryInfo path.
//
// TODO: When the next `need-minor-update` release bumps the
// agnocastlib ↔ kmod ABI, carry the message type through the kmod
// alongside topic / node info; the bridge_manager can then pre-populate
// its registry directly from the kmod and this MQ can go away.
struct MqMsgFactoryRegister
{
  char type_name[MESSAGE_TYPE_BUFFER_SIZE];
  char shared_lib_path[SHARED_LIB_PATH_BUFFER_SIZE];
  uintptr_t fn_offset_a2r;
  uintptr_t fn_offset_r2a;
};

constexpr int64_t BRIDGE_MQ_MAX_MESSAGES = 2;
constexpr int64_t PERFORMANCE_BRIDGE_MQ_MAX_MESSAGES = 256;
// Kept at or below the kernel default `/proc/sys/fs/mqueue/msg_max = 10`,
// otherwise mq_open() returns EINVAL on hosts that have not raised the
// limit (which would require CAP_SYS_RESOURCE). 8 is enough headroom for
// the daemon's burst write pattern: even at Autoware-scale (~100
// processes), a single per-NS daemon tick can only enqueue a handful of
// bridge requests before the bridge_manager drains them.
constexpr int64_t DAEMON_BRIDGE_MQ_MAX_MESSAGES = 8;
constexpr int64_t BRIDGE_MQ_MESSAGE_SIZE = sizeof(MqMsgBridge);
constexpr int64_t PERFORMANCE_BRIDGE_MQ_MESSAGE_SIZE = sizeof(MqMsgPerformanceBridge);
constexpr int64_t DAEMON_BRIDGE_MQ_MESSAGE_SIZE = sizeof(MqMsgDaemonBridge);
// Same kernel-default ceiling reasoning as DAEMON_BRIDGE_MQ_MAX_MESSAGES.
// One write per `Publisher<T>` / `Subscription<T>` construction; bursts at
// node startup are bounded by the number of pub/sub endpoints in the node.
constexpr int64_t FACTORY_REGISTER_MQ_MAX_MESSAGES = 8;
constexpr int64_t FACTORY_REGISTER_MQ_MESSAGE_SIZE = sizeof(MqMsgFactoryRegister);
constexpr mode_t BRIDGE_MQ_PERMS = 0600;

// MQ name conventions for daemon-originated bridge requests.
// - Standard mode: `/agnocast_daemon_bridge@<pid>` (one MQ per user process; the
//   daemon picks the target pid based on gossip data).
// - Performance mode: `/agnocast_daemon_bridge_perf` (one MQ per IPC namespace).
inline constexpr const char * DAEMON_BRIDGE_MQ_PREFIX = "/agnocast_daemon_bridge";
inline constexpr const char * PERFORMANCE_DAEMON_BRIDGE_MQ_NAME = "/agnocast_daemon_bridge_perf";

// MQ name for the user → bridge_manager factory pre-registration channel.
// One MQ per Standard-mode bridge_manager (= per user process).
// TODO: Replace with kmod-side type info when MqMsgFactoryRegister itself
// becomes unnecessary (see comment on the struct above).
inline constexpr const char * FACTORY_REGISTER_MQ_PREFIX = "/agnocast_factory_register";

}  // namespace agnocast

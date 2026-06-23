#pragma once

#include "agnocast/agnocast_ioctl.hpp"

#include <cstddef>
#include <cstdint>

namespace agnocast
{

inline constexpr pid_t PERFORMANCE_BRIDGE_VIRTUAL_PID = -1;

inline constexpr size_t SERVICE_NAME_BUFFER_SIZE = 256;
inline constexpr size_t MESSAGE_TYPE_BUFFER_SIZE = 256;
inline constexpr size_t SERVICE_TYPE_BUFFER_SIZE = 256;

enum class BridgeDirection : uint32_t { ROS2_TO_AGNOCAST = 0, AGNOCAST_TO_ROS2 = 1 };

struct MqMsgAgnocast
{
};

struct MqMsgROS2Publish
{
  bool should_terminate;
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

// Cross-IPC-namespace bridge request from the per-NS daemon to a same-NS
// bridge_manager. The daemon holds no process-local factory pointers, so it
// names the target by topic (standard mode reuses the factory the manager
// already cached from this process's intra-NS requests) and by type
// (performance mode resolves a plugin). QoS is sent explicitly since the
// bridge_manager cannot query the originating endpoint's QoS on its own.
struct MqMsgDaemonBridge
{
  char topic_name[TOPIC_NAME_BUFFER_SIZE];
  char type_name[MESSAGE_TYPE_BUFFER_SIZE];
  BridgeDirection direction;
  uint32_t qos_depth;
  bool qos_is_transient_local;
  bool qos_is_reliable;
};

// Wire-format payload sizes used over the abstract-namespace UDS transport.
constexpr int64_t PERFORMANCE_BRIDGE_MSG_SIZE = sizeof(MqMsgPerformanceBridge);
constexpr int64_t DAEMON_BRIDGE_MSG_SIZE = sizeof(MqMsgDaemonBridge);

// Abstract-namespace UDS addresses (sun_path[0] == '\0', rest is the name).
// Scope is the network namespace; one bridge_manager per IPC namespace listens
// on each address. The optional `_d<ROS_DOMAIN_ID>` suffix mirrors the old MQ
// naming convention.
//
// Standard mode: one address per user process,
//   `\0agnocast_daemon_bridge@<pid>`.
// Performance mode: one address per IPC namespace,
//   `\0agnocast_daemon_bridge_perf{_d<DOMAIN>}`.
inline constexpr const char * BRIDGE_UDS_PREFIX = "agnocast_bridge_manager";
inline constexpr const char * DAEMON_BRIDGE_UDS_PREFIX = "agnocast_daemon_bridge";
inline constexpr const char * PERFORMANCE_DAEMON_BRIDGE_UDS_NAME = "agnocast_daemon_bridge_perf";

}  // namespace agnocast

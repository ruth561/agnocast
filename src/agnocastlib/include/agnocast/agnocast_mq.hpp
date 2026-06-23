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

// Discriminator tag for BridgeMsg.
enum class BridgeMsgType : uint32_t {
  PubSub = 0,
  Service = 1,
  Daemon = 2,
};

// Payload for BridgeMsgType::PubSub.
struct BridgeMsgPubSubPayload
{
  BridgeDirection direction;
  char message_type[MESSAGE_TYPE_BUFFER_SIZE];
  char topic_name[TOPIC_NAME_BUFFER_SIZE];
  topic_local_id_t target_id;
};

// Payload for BridgeMsgType::Service.
struct BridgeMsgServicePayload
{
  BridgeDirection direction;
  char service_type[SERVICE_TYPE_BUFFER_SIZE];
  char service_name[SERVICE_NAME_BUFFER_SIZE];
  bool create_shadow_node;
  char shadow_node_namespace[NODE_NAME_BUFFER_SIZE];
  char shadow_node_name[NODE_NAME_BUFFER_SIZE];
};

// Payload for BridgeMsgType::Daemon.
// Cross-IPC-namespace bridge request from the per-NS daemon to a same-NS
// bridge_manager. The daemon holds no process-local factory pointers, so it
// names the target by topic and type. QoS is sent explicitly since the
// bridge_manager cannot query the originating endpoint's QoS on its own.
// Wire layout (524 bytes) is mirrored in bridge_decider.py; the unit test
// asserts it stays in sync.
struct BridgeMsgDaemonPayload
{
  char topic_name[TOPIC_NAME_BUFFER_SIZE];
  char type_name[MESSAGE_TYPE_BUFFER_SIZE];
  BridgeDirection direction;
  uint32_t qos_depth;
  bool qos_is_transient_local;
  bool qos_is_reliable;
};

// Unified bridge message. All bridge endpoints (intra-NS subscribers, publishers,
// service servers) and the per-NS discovery daemon write to the single
// bridge_manager MQ using this type. Senders transmit only the bytes for the
// active payload (offsetof(BridgeMsg, payload) + sizeof(active_variant)); the
// receiver opens the MQ with sizeof(BridgeMsg) as mq_msgsize so any variant
// fits. All payload members share 4-byte alignment so the union itself is
// 4-byte aligned and `type` sits at offset 0 with no padding before `payload`.
struct BridgeMsg
{
  BridgeMsgType type;
  union Payload {
    BridgeMsgPubSubPayload pubsub;
    BridgeMsgServicePayload service;
    BridgeMsgDaemonPayload daemon;
  } payload;
};

constexpr int64_t BRIDGE_MQ_MAX_MESSAGES = 256;
constexpr int64_t BRIDGE_MQ_MESSAGE_SIZE = sizeof(BridgeMsg);
constexpr mode_t BRIDGE_MQ_PERMS = 0600;

// Wire size of a BridgeMsg carrying a specific payload variant: the tag plus
// just the active variant's bytes. Used both for `mq_send` and for sizing
// auxiliary buffers.
template <typename PayloadT>
constexpr size_t bridge_msg_wire_size()
{
  return offsetof(BridgeMsg, payload) + sizeof(PayloadT);
}

}  // namespace agnocast

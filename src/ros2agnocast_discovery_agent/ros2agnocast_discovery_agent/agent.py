"""Per-IPC-namespace discovery agent.

Reads the local Agnocast state via the existing NS-scoped ioctl wrapper
(libagnocast_ioctl_wrapper.so) and publishes it as AgnocastDaemonState on
``/_agnocast_discovery`` so other namespaces and ECUs running ros2agnocast
tooling can observe and make bridge generation decisions.

One daemon process is intended to run per IPC namespace. Lifecycle is the
user's responsibility (systemd unit, ros2 launch include, container
entrypoint, etc.).
"""

import ctypes
import os
import socket
import sys
import uuid

import rclpy
from rclpy.executors import ExternalShutdownException
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, HistoryPolicy, LivelinessPolicy, QoSProfile, ReliabilityPolicy
from rclpy.duration import Duration

from builtin_interfaces.msg import Time
from ros2agnocast_discovery_msgs.msg import (
    AgnocastDaemonState,
    AgnocastEndpoint,
    AgnocastTopic,
)

from . import bridge_decider


GOSSIP_TOPIC = '/_agnocast_discovery'
SCHEMA_VERSION = 1
PUBLISH_INTERVAL_SEC = 1.0
LIVELINESS_LEASE_SEC = 30.0
MACHINE_ID_PATH = '/etc/machine-id'
SELF_IPC_NS_PATH = '/proc/self/ns/ipc'
# Kept separate from TOPIC_NAME_BUFFER_SIZE because the C side (`agnocast_kmod`)
# defines NODE_NAME_BUFFER_SIZE and TOPIC_NAME_BUFFER_SIZE as independent
# constants — they happen to share the value 256 today.
NODE_NAME_BUFFER_SIZE = 256
TOPIC_NAME_BUFFER_SIZE = 256
# How long a remote daemon's last snapshot may sit in _remote_states without
# being refreshed before we drop it. Matches the gossip Liveliness lease so
# DDS-side liveliness loss and local prune happen on the same timescale.
REMOTE_STATE_STALE_SEC = 30.0


class TopicInfoRet(ctypes.Structure):
    """Mirror of ``struct topic_info_ret`` in agnocast_ioctl.hpp."""

    _fields_ = [
        ('node_name', ctypes.c_char * NODE_NAME_BUFFER_SIZE),
        ('qos_depth', ctypes.c_uint32),
        ('qos_is_transient_local', ctypes.c_bool),
        ('qos_is_reliable', ctypes.c_bool),
        ('is_bridge', ctypes.c_bool),
    ]


def _load_ioctl_wrapper():
    """Load libagnocast_ioctl_wrapper.so and set argtypes for the symbols we use."""
    lib = ctypes.CDLL('libagnocast_ioctl_wrapper.so')

    lib.get_agnocast_topics.argtypes = [ctypes.POINTER(ctypes.c_int)]
    lib.get_agnocast_topics.restype = ctypes.POINTER(ctypes.POINTER(ctypes.c_char))
    lib.free_agnocast_topics.argtypes = [
        ctypes.POINTER(ctypes.POINTER(ctypes.c_char)),
        ctypes.c_int,
    ]
    lib.free_agnocast_topics.restype = None

    lib.get_agnocast_sub_nodes.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
    lib.get_agnocast_sub_nodes.restype = ctypes.POINTER(TopicInfoRet)
    lib.get_agnocast_pub_nodes.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
    lib.get_agnocast_pub_nodes.restype = ctypes.POINTER(TopicInfoRet)
    lib.free_agnocast_topic_info_ret.argtypes = [ctypes.POINTER(TopicInfoRet)]
    lib.free_agnocast_topic_info_ret.restype = None

    return lib


def _ioctl_to_endpoint(info: TopicInfoRet) -> AgnocastEndpoint:
    """Convert one ``topic_info_ret`` row to an AgnocastEndpoint msg.

    ``pid`` is best-effort 0 because the existing ioctl does not expose it; the
    bridge decider may fill it via ``/proc`` walk when routing bridge requests.
    """
    ep = AgnocastEndpoint()
    ep.node_name = info.node_name.decode('utf-8', errors='replace')
    ep.pid = 0
    ep.qos_depth = info.qos_depth
    ep.qos_is_transient_local = info.qos_is_transient_local
    ep.qos_is_reliable = info.qos_is_reliable
    ep.is_bridge = info.is_bridge
    return ep


def read_local_topics(lib) -> list:
    """Snapshot the current namespace's Agnocast topics via the ioctl wrapper.

    Returns a list of AgnocastTopic msgs. The ioctl returns only the caller's
    IPC namespace, so the daemon process just being inside that namespace is
    sufficient to scope the result.
    """
    topic_count = ctypes.c_int()
    topic_names_ptr = lib.get_agnocast_topics(ctypes.byref(topic_count))
    topics = []
    if not topic_names_ptr:
        return topics

    try:
        for i in range(topic_count.value):
            topic_name_b = ctypes.cast(topic_names_ptr[i], ctypes.c_char_p).value
            topic_name = topic_name_b.decode('utf-8', errors='replace')

            agnocast_topic = AgnocastTopic()
            agnocast_topic.topic_name = topic_name
            # type_name is best-effort empty here; resolved by future work
            # (procfs/topic_info exposing message_type, or kmod ioctl extension)
            agnocast_topic.type_name = ''
            agnocast_topic.domain_id = 0
            agnocast_topic.publishers = _collect_endpoints(lib.get_agnocast_pub_nodes, lib, topic_name_b)
            agnocast_topic.subscribers = _collect_endpoints(lib.get_agnocast_sub_nodes, lib, topic_name_b)
            topics.append(agnocast_topic)
    finally:
        lib.free_agnocast_topics(topic_names_ptr, topic_count.value)

    return topics


def _collect_endpoints(getter, lib, topic_name_b: bytes) -> list:
    count = ctypes.c_int()
    array = getter(topic_name_b, ctypes.byref(count))
    endpoints = []
    if not array:
        return endpoints
    try:
        for i in range(count.value):
            endpoints.append(_ioctl_to_endpoint(array[i]))
    finally:
        lib.free_agnocast_topic_info_ret(array)
    return endpoints


def _read_machine_id() -> str:
    """Return /etc/machine-id formatted as a UUID, fallback to a random UUID."""
    try:
        with open(MACHINE_ID_PATH) as fp:
            raw = fp.read().strip()
        if len(raw) == 32 and all(c in '0123456789abcdef' for c in raw):
            return str(uuid.UUID(raw))
    except OSError:
        pass
    return str(uuid.uuid4())


def _read_ipc_ns_inode() -> int:
    """Return the inode number of the daemon's own IPC namespace."""
    return os.stat(SELF_IPC_NS_PATH).st_ino


def _gossip_qos() -> QoSProfile:
    return QoSProfile(
        reliability=ReliabilityPolicy.RELIABLE,
        durability=DurabilityPolicy.TRANSIENT_LOCAL,
        history=HistoryPolicy.KEEP_LAST,
        depth=1,
        liveliness=LivelinessPolicy.AUTOMATIC,
        liveliness_lease_duration=Duration(seconds=LIVELINESS_LEASE_SEC),
    )


class DiscoveryAgent(Node):
    """rclpy Node that publishes the local Agnocast state every PUBLISH_INTERVAL_SEC.

    Also subscribes to its own gossip topic so the bridge decider can observe
    remote namespace state; the callback records the latest snapshot keyed by
    ``(host_uuid, ipc_ns_inode)``.
    """

    def __init__(self, ioctl_lib=None):
        super().__init__('agnocast_discovery_agent')
        self._lib = ioctl_lib if ioctl_lib is not None else _load_ioctl_wrapper()
        self._host_uuid = _read_machine_id()
        self._host_hostname = socket.gethostname()
        self._ipc_ns_inode = _read_ipc_ns_inode()
        self._agnocast_version = os.environ.get('AGNOCAST_VERSION', '')

        qos = _gossip_qos()
        self._pub = self.create_publisher(AgnocastDaemonState, GOSSIP_TOPIC, qos)
        self._sub = self.create_subscription(
            AgnocastDaemonState, GOSSIP_TOPIC, self._on_remote_state, qos)
        self._remote_states = {}

        self._timer = self.create_timer(PUBLISH_INTERVAL_SEC, self._on_tick)

        self.get_logger().info(
            f'agnocast_discovery_agent up: host_uuid={self._host_uuid} '
            f'hostname={self._host_hostname} ipc_ns_inode={self._ipc_ns_inode}')

    def _on_tick(self) -> None:
        self._prune_stale_remote_states()
        snapshot = self.publish_snapshot()
        self._dispatch_bridge_requests(snapshot)

    def _prune_stale_remote_states(
            self, now_sec: float | None = None,
            stale_after_sec: float = REMOTE_STATE_STALE_SEC) -> None:
        """Drop remote-snapshot entries whose timestamp is older than the threshold.

        Without this, a daemon that disappears (host dies, NS torn down, etc.)
        would leave its last snapshot in ``_remote_states`` forever, slowly
        growing memory in long-running deployments. The DDS Liveliness lease
        already forces the publisher off the topic at the same timescale; we
        mirror it here so the in-memory cache agrees.

        ``now_sec`` is exposed for tests; production code lets the method read
        ``self.get_clock()`` directly.
        """
        if now_sec is None:
            now_sec = self.get_clock().now().nanoseconds / 1e9
        stale_keys = [
            key for key, msg in self._remote_states.items()
            if now_sec - (msg.timestamp.sec + msg.timestamp.nanosec / 1e9)
            > stale_after_sec
        ]
        for key in stale_keys:
            del self._remote_states[key]

    def publish_snapshot(self) -> AgnocastDaemonState:
        """Build and publish the current local AgnocastDaemonState."""
        msg = self.build_state()
        self._pub.publish(msg)
        return msg

    def _dispatch_bridge_requests(self, local_state: AgnocastDaemonState) -> None:
        """Compare local vs remote gossip state and send bridge requests."""
        if not self._remote_states:
            return
        requests = bridge_decider.decide_bridges(local_state, self._remote_states)
        if requests:
            bridge_decider.dispatch_requests(requests, logger=self.get_logger())

    def build_state(self) -> AgnocastDaemonState:
        msg = AgnocastDaemonState()
        msg.schema_version = SCHEMA_VERSION
        msg.agnocast_version = self._agnocast_version
        msg.host_uuid = self._host_uuid
        msg.host_hostname = self._host_hostname
        now = self.get_clock().now().to_msg()
        msg.timestamp = Time(sec=now.sec, nanosec=now.nanosec)
        msg.ipc_ns_inode = self._ipc_ns_inode
        msg.topics = read_local_topics(self._lib)
        return msg

    def _on_remote_state(self, msg: AgnocastDaemonState) -> None:
        # Skip our own messages.
        if msg.host_uuid == self._host_uuid and msg.ipc_ns_inode == self._ipc_ns_inode:
            return
        self._remote_states[(msg.host_uuid, msg.ipc_ns_inode)] = msg

    @property
    def remote_states(self) -> dict:
        """Latest gossip snapshots from other (host, namespace) pairs."""
        return self._remote_states


def main(argv=None) -> int:
    rclpy.init(args=argv)
    node = DiscoveryAgent()
    try:
        rclpy.spin(node)
    except (KeyboardInterrupt, ExternalShutdownException):
        pass
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

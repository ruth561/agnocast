"""Decide and dispatch cross-namespace bridge requests for the discovery agent.

Each tick the agent compares the local Agnocast state with the remote
snapshots gathered over gossip. When a topic has an Agnocast endpoint locally
and the opposite-role endpoint in another namespace, a bridge is needed here
so the two reach each other through ROS 2 (DDS):

  * local publisher  + remote subscriber -> A2R bridge (publish to DDS)
  * local subscriber + remote publisher  -> R2A bridge (reinject from DDS)

The request is sent as ``MqMsgDaemonBridge`` to the per-namespace bridge_manager
over an abstract-namespace UNIX domain socket. The struct layout is mirrored
here so the daemon stays decoupled from libagnocast's C++ headers;
``agnocast_mq.hpp`` owns the source of truth and a test asserts the size stays
in sync.
"""

from dataclasses import dataclass
import errno
import os
import socket
import struct
from typing import Iterable, Optional

TOPIC_NAME_BUFFER_SIZE = 256
MESSAGE_TYPE_BUFFER_SIZE = 256

# char topic_name[256]; char type_name[256]; uint32 direction; uint32 qos_depth;
# bool qos_is_transient_local; bool qos_is_reliable; + 2 bytes tail padding so
# the total matches sizeof(MqMsgDaemonBridge) == 524 on the C++ side.
_MSG_PACK_FORMAT = '=256s256sIIBB2x'

DIRECTION_ROS2_TO_AGNOCAST = 0
DIRECTION_AGNOCAST_TO_ROS2 = 1

# One bridge_manager per IPC namespace listens on this abstract-namespace UDS.
# Mirrors ``PERFORMANCE_DAEMON_BRIDGE_UDS_NAME`` in ``agnocast_mq.hpp``.
_PERFORMANCE_UDS_NAME = 'agnocast_daemon_bridge_perf'


@dataclass(frozen=True)
class BridgeRequest:
    topic_name: str
    type_name: str
    direction: int
    qos_depth: int
    qos_is_transient_local: bool
    qos_is_reliable: bool


def serialize_request(req: BridgeRequest) -> bytes:
    topic = req.topic_name.encode('utf-8')[: TOPIC_NAME_BUFFER_SIZE - 1]
    type_name = req.type_name.encode('utf-8')[: MESSAGE_TYPE_BUFFER_SIZE - 1]
    return struct.pack(
        _MSG_PACK_FORMAT,
        topic,
        type_name,
        req.direction,
        req.qos_depth,
        1 if req.qos_is_transient_local else 0,
        1 if req.qos_is_reliable else 0,
    )


def _resolve_types(local_state, remote_states) -> dict:
    """Resolve each topic's message type, preferring local then any remote.

    A bridge is deduped per ``(topic, direction)``, so its type must be resolved
    per-topic across local + *all* remotes: the remote that supplies the
    opposite-role endpoint may lack the type while another snapshot has it.
    """
    types = {t.topic_name: t.type_name for t in local_state.topics if t.type_name}
    for remote in remote_states.values():
        for t in remote.topics:
            if t.type_name:
                types.setdefault(t.topic_name, t.type_name)
    return types


def decide_bridges(local_state, remote_states) -> list:
    """Return the bridge requests this namespace should issue this tick.

    ``remote_states`` maps ``(host_uuid, ipc_ns_inode)`` to AgnocastDaemonState.
    Requests are collapsed to one per ``(topic, direction)``.
    """
    requests = {}

    local_by_topic = {t.topic_name: t for t in local_state.topics}
    types = _resolve_types(local_state, remote_states)

    for (host_uuid, ipc_ns_inode), remote in remote_states.items():
        if host_uuid == local_state.host_uuid and ipc_ns_inode == local_state.ipc_ns_inode:
            continue
        for remote_topic in remote.topics:
            local_topic = local_by_topic.get(remote_topic.topic_name)
            if local_topic is None:
                continue

            local_pubs = [p for p in local_topic.publishers if not p.is_bridge]
            local_subs = [s for s in local_topic.subscribers if not s.is_bridge]
            remote_pubs = [p for p in remote_topic.publishers if not p.is_bridge]
            remote_subs = [s for s in remote_topic.subscribers if not s.is_bridge]

            type_name = types.get(local_topic.topic_name)
            if not type_name:
                continue

            if local_pubs and remote_subs:
                pub = local_pubs[0]
                key = (local_topic.topic_name, DIRECTION_AGNOCAST_TO_ROS2)
                requests.setdefault(key, BridgeRequest(
                    topic_name=local_topic.topic_name,
                    type_name=type_name,
                    direction=DIRECTION_AGNOCAST_TO_ROS2,
                    qos_depth=pub.qos_depth,
                    qos_is_transient_local=pub.qos_is_transient_local,
                    qos_is_reliable=pub.qos_is_reliable,
                ))

            if local_subs and remote_pubs:
                sub = local_subs[0]
                key = (local_topic.topic_name, DIRECTION_ROS2_TO_AGNOCAST)
                requests.setdefault(key, BridgeRequest(
                    topic_name=local_topic.topic_name,
                    type_name=type_name,
                    direction=DIRECTION_ROS2_TO_AGNOCAST,
                    qos_depth=sub.qos_depth,
                    qos_is_transient_local=sub.qos_is_transient_local,
                    qos_is_reliable=sub.qos_is_reliable,
                ))

    return list(requests.values())


def _performance_uds_addr() -> str:
    """Return the abstract-namespace UDS address the bridge_manager listens on.

    Python's socket module treats addresses starting with ``\\x00`` as
    abstract, mirroring the C++ side's ``\\0agnocast_daemon_bridge_perf...``.
    """
    name = _PERFORMANCE_UDS_NAME
    domain_id = os.environ.get('ROS_DOMAIN_ID')
    if domain_id:
        name += '_d' + domain_id
    return '\x00' + name


def send_request(uds_addr: str, payload: bytes) -> Optional[str]:
    """Send ``payload`` to ``uds_addr`` as a single datagram; return an error or None.

    The bridge_manager binds the abstract-namespace UDS only after it starts,
    so ``ECONNREFUSED`` is silently absorbed: the request is re-issued
    idempotently next tick. ``EAGAIN`` (receiver buffer momentarily full) is
    likewise absorbed because the request is idempotent. Everything else is
    reported so misconfigurations don't go silent.
    """
    # SOCK_DGRAM: no connect()/accept() handshake; sendto() either delivers
    # the whole payload atomically or fails. Non-blocking so the daemon's
    # tick loop is never stalled by a slow consumer.
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sock.setblocking(False)
    try:
        try:
            sock.sendto(payload, uds_addr)
        except (ConnectionRefusedError, FileNotFoundError):
            # bridge_manager not yet up; the daemon will retry next tick.
            return None
        except BlockingIOError:
            # Receiver buffer full; the daemon will retry next tick.
            return None
        except OSError as e:
            if e.errno in (errno.ENOENT, errno.ENOBUFS):
                return None
            return f'sendto({uds_addr!r}): {os.strerror(e.errno) if e.errno else str(e)}'
    finally:
        sock.close()
    return None


def dispatch_requests(requests: Iterable[BridgeRequest], logger=None) -> None:
    """Deliver each request to the per-namespace bridge_manager UDS.

    The listener is absent until a bridge_manager is up; ``send_request``
    swallows ECONNREFUSED/ENOENT so a missing peer never stalls the daemon, and
    the request is re-issued idempotently next tick.
    """
    addr = _performance_uds_addr()
    for req in requests:
        err = send_request(addr, serialize_request(req))
        if err is not None and logger is not None:
            logger.warn('daemon bridge dispatch failed: %s', err)

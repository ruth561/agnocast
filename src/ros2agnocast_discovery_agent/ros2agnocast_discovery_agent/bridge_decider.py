"""Bridge decider for the per-IPC-namespace discovery agent.

Compares the local AgnocastDaemonState with remote (cross-NS / cross-ECU)
snapshots gathered through gossip and emits `MqMsgDaemonBridge` requests
to the in-namespace bridge_manager MQs:

  * Standard mode:  `/agnocast_daemon_bridge@<pid>` (broadcast to every
    running bridge_manager so the one whose process registered the type
    factory picks it up).
  * Performance mode: `/agnocast_daemon_bridge_perf[_d<ROS_DOMAIN_ID>]`
    (one MQ per IPC namespace).

The struct layout for `MqMsgDaemonBridge` is hard-coded here so the
daemon stays decoupled from libagnocast's C++ headers — the layout is
intentionally simple and additive (see `agnocast_mq.hpp`).
"""

import ctypes
import errno
import os
import struct
from dataclasses import dataclass
from typing import Iterable, Optional

# Mirrors the C++ constants in `agnocast_mq.hpp`. Keep in sync if those
# struct layouts change (a CI lint or test should guard this).
TOPIC_NAME_BUFFER_SIZE = 256
MESSAGE_TYPE_BUFFER_SIZE = 256
BRIDGE_MQ_PERMS = 0o600

# Daemon MQ layout: char topic_name[256]; char type_name[256];
# uint32_t direction; uint32_t qos_depth; bool qos_is_transient_local;
# bool qos_is_reliable;
# struct.pack format with explicit standard sizes / no padding handled
# below by emitting native sizes and matching the C++ struct's natural
# layout. The struct is plain POD so native packing matches.
# Native order, fixed sizes. Trailing `2x` is the alignment padding the C++
# compiler adds so the total matches `sizeof(MqMsgDaemonBridge) == 524`.
_MSG_PACK_FORMAT = '=256s256sIIBB2x'

DIRECTION_ROS2_TO_AGNOCAST = 0
DIRECTION_AGNOCAST_TO_ROS2 = 1

# Lazy-loaded librt mq_open / mq_send / mq_close. We avoid posix_ipc to
# keep the daemon dependency surface to "rclpy + stdlib".
_librt = None


def _load_librt():
    global _librt
    if _librt is not None:
        return _librt
    lib = ctypes.CDLL('librt.so.1', use_errno=True)
    lib.mq_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
    lib.mq_open.restype = ctypes.c_int
    lib.mq_send.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint]
    lib.mq_send.restype = ctypes.c_int
    lib.mq_close.argtypes = [ctypes.c_int]
    lib.mq_close.restype = ctypes.c_int
    _librt = lib
    return _librt


O_WRONLY = os.O_WRONLY
O_NONBLOCK = os.O_NONBLOCK


@dataclass(frozen=True)
class BridgeRequest:
    """A single bridge request that needs to be dispatched to a bridge_manager."""

    topic_name: str
    type_name: str
    direction: int  # DIRECTION_ROS2_TO_AGNOCAST | DIRECTION_AGNOCAST_TO_ROS2
    qos_depth: int
    qos_is_transient_local: bool
    qos_is_reliable: bool
    target_pid: int  # Standard-mode target bridge_manager pid; 0 = unknown.


def serialize_request(req: BridgeRequest) -> bytes:
    """Pack a `BridgeRequest` into the wire format expected by bridge_manager.

    The trailing bytes after the 4 packed fields are padded with NULs to
    match `sizeof(MqMsgDaemonBridge)`.
    """
    # Use truncation to stay within fixed-size char arrays.
    topic = req.topic_name.encode('utf-8')[: TOPIC_NAME_BUFFER_SIZE - 1]
    type_name = req.type_name.encode('utf-8')[: MESSAGE_TYPE_BUFFER_SIZE - 1]
    packed = struct.pack(
        _MSG_PACK_FORMAT,
        topic,
        type_name,
        req.direction,
        req.qos_depth,
        1 if req.qos_is_transient_local else 0,
        1 if req.qos_is_reliable else 0,
    )
    return packed


def decide_bridges(local_state, remote_states) -> list:
    """Compute the bridges that should be requested in the local namespace.

    Args:
        local_state: AgnocastDaemonState for this IPC namespace (the one
            the daemon just built from procfs / ioctl).
        remote_states: mapping ``(host_uuid, ipc_ns_inode) -> AgnocastDaemonState``
            collected from the gossip subscription.

    Returns:
        list of `BridgeRequest` to dispatch on this tick. Duplicates are
        collapsed (one request per (topic_name, direction)).
    """
    requests = {}

    local_by_topic = {t.topic_name: t for t in local_state.topics}

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

            type_name = local_topic.type_name or remote_topic.type_name
            if not type_name:
                continue

            # Local Agnocast publisher + remote Agnocast subscriber -> A2R
            # bridge here so the local publisher's data reaches ROS 2 (DDS).
            # Target pid = local publisher pid so Standard-mode dispatch
            # hits exactly the user process holding the factory.
            if local_pubs and remote_subs:
                qos_source = local_pubs[0]
                key = (local_topic.topic_name, DIRECTION_AGNOCAST_TO_ROS2)
                requests.setdefault(
                    key,
                    BridgeRequest(
                        topic_name=local_topic.topic_name,
                        type_name=type_name,
                        direction=DIRECTION_AGNOCAST_TO_ROS2,
                        qos_depth=qos_source.qos_depth,
                        qos_is_transient_local=qos_source.qos_is_transient_local,
                        qos_is_reliable=qos_source.qos_is_reliable,
                        target_pid=qos_source.pid,
                    ),
                )

            # Local Agnocast subscriber + remote Agnocast publisher -> R2A
            # bridge here so DDS data is reinjected for the local subscriber.
            if local_subs and remote_pubs:
                qos_source = local_subs[0]
                key = (local_topic.topic_name, DIRECTION_ROS2_TO_AGNOCAST)
                requests.setdefault(
                    key,
                    BridgeRequest(
                        topic_name=local_topic.topic_name,
                        type_name=type_name,
                        direction=DIRECTION_ROS2_TO_AGNOCAST,
                        qos_depth=qos_source.qos_depth,
                        qos_is_transient_local=qos_source.qos_is_transient_local,
                        qos_is_reliable=qos_source.qos_is_reliable,
                        target_pid=qos_source.pid,
                    ),
                )

    return list(requests.values())


def _standard_mq_name(pid: int) -> str:
    """Return the Standard-mode bridge_manager MQ name for a given user pid."""
    return f'/agnocast_daemon_bridge@{pid}'


def _performance_mq_name() -> str:
    name = '/agnocast_daemon_bridge_perf'
    domain_id = os.environ.get('ROS_DOMAIN_ID')
    if domain_id is not None:
        name += '_d' + domain_id
    return name


def send_request(mq_name: str, payload: bytes) -> Optional[str]:
    """Send `payload` to the POSIX MQ at `mq_name`.

    Returns an error string on failure, None on success. ``O_NONBLOCK`` is
    used so a full queue does not block the daemon — the next tick will
    retry idempotently.
    """
    lib = _load_librt()
    fd = lib.mq_open(mq_name.encode('utf-8'), O_WRONLY | O_NONBLOCK)
    if fd == -1:
        err = ctypes.get_errno()
        if err == errno.ENOENT:
            return None  # MQ not present; nothing to send to.
        return f'mq_open({mq_name}): {os.strerror(err)}'

    try:
        rc = lib.mq_send(fd, payload, len(payload), 0)
        if rc == -1:
            err = ctypes.get_errno()
            if err == errno.EAGAIN:
                return None  # Queue full; drop and retry next tick.
            return f'mq_send({mq_name}): {os.strerror(err)}'
    finally:
        lib.mq_close(fd)
    return None


def dispatch_requests(requests: Iterable[BridgeRequest], logger=None) -> None:
    """Send every request to the Performance MQ and to the target Standard MQ.

    Standard-mode targeting: each request carries the pid of the local
    user process whose `Publisher<T>` / `Subscription<T>` registered the
    factory in this process (via the tmpfs type registry). The daemon
    sends only to that pid's `/agnocast_daemon_bridge@<pid>` MQ —
    avoiding the broadcast hack that the previous design needed when
    pid was unknown.

    Performance mode uses a single per-NS MQ (`pid` is irrelevant there).
    A request with `target_pid == 0` (no matching registry entry) is
    delivered only to the Performance MQ; Standard-mode managers don't
    receive it.
    """
    requests = list(requests)
    if not requests:
        return

    perf_mq = _performance_mq_name()
    for req in requests:
        payload = serialize_request(req)
        if (err := send_request(perf_mq, payload)) is not None and logger is not None:
            logger.warning('daemon bridge dispatch failed: %s', err)
        if req.target_pid > 0:
            std_mq = _standard_mq_name(req.target_pid)
            if (err := send_request(std_mq, payload)) is not None and logger is not None:
                logger.warning('daemon bridge dispatch failed: %s', err)

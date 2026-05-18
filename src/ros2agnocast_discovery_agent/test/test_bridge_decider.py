"""Unit tests for the F1 bridge decider.

These tests do not require the kmod, DDS, or any POSIX MQ: the decider's
pure-logic part (`decide_bridges`) is exercised directly, and the wire
format is checked against a hand-built byte layout that mirrors
`sizeof(MqMsgDaemonBridge) == 524` in `agnocast_mq.hpp`.
"""

from ros2agnocast_discovery_agent.bridge_decider import (
    DIRECTION_AGNOCAST_TO_ROS2,
    DIRECTION_ROS2_TO_AGNOCAST,
    BridgeRequest,
    decide_bridges,
    serialize_request,
)
from ros2agnocast_discovery_msgs.msg import (
    AgnocastDaemonState,
    AgnocastEndpoint,
    AgnocastTopic,
)


def _endpoint(node, depth=10, transient=False, reliable=True, is_bridge=False, pid=0):
    ep = AgnocastEndpoint()
    ep.node_name = node
    ep.pid = pid
    ep.qos_depth = depth
    ep.qos_is_transient_local = transient
    ep.qos_is_reliable = reliable
    ep.is_bridge = is_bridge
    return ep


def _topic(name, type_name='std_msgs/msg/Int32', pubs=None, subs=None):
    t = AgnocastTopic()
    t.topic_name = name
    t.type_name = type_name
    t.domain_id = 0
    t.publishers = pubs or []
    t.subscribers = subs or []
    return t


def _state(host_uuid='HOST', ipc_ns=111, topics=None):
    s = AgnocastDaemonState()
    s.schema_version = 1
    s.agnocast_version = ''
    s.host_uuid = host_uuid
    s.host_hostname = ''
    s.ipc_ns_inode = ipc_ns
    s.topics = topics or []
    return s


def test_decide_emits_a2r_when_local_pub_remote_sub():
    local = _state(
        topics=[_topic('/x', pubs=[_endpoint('/pub')])],
    )
    remote = _state(
        host_uuid='OTHER',
        ipc_ns=222,
        topics=[_topic('/x', subs=[_endpoint('/sub')])],
    )

    reqs = decide_bridges(local, {('OTHER', 222): remote})
    assert len(reqs) == 1
    assert reqs[0].topic_name == '/x'
    assert reqs[0].direction == DIRECTION_AGNOCAST_TO_ROS2
    assert reqs[0].type_name == 'std_msgs/msg/Int32'


def test_decide_emits_r2a_when_local_sub_remote_pub():
    local = _state(topics=[_topic('/x', subs=[_endpoint('/sub')])])
    remote = _state(
        host_uuid='OTHER',
        ipc_ns=222,
        topics=[_topic('/x', pubs=[_endpoint('/pub')])],
    )

    reqs = decide_bridges(local, {('OTHER', 222): remote})
    assert len(reqs) == 1
    assert reqs[0].direction == DIRECTION_ROS2_TO_AGNOCAST


def test_decide_emits_both_directions_when_both_sides_have_both():
    local = _state(topics=[_topic('/x',
                                  pubs=[_endpoint('/lp')],
                                  subs=[_endpoint('/ls')])])
    remote = _state(
        host_uuid='OTHER',
        ipc_ns=222,
        topics=[_topic('/x',
                       pubs=[_endpoint('/rp')],
                       subs=[_endpoint('/rs')])],
    )

    reqs = decide_bridges(local, {('OTHER', 222): remote})
    directions = sorted(r.direction for r in reqs)
    assert directions == sorted([DIRECTION_ROS2_TO_AGNOCAST, DIRECTION_AGNOCAST_TO_ROS2])


def test_decide_skips_bridge_only_endpoints():
    local = _state(topics=[_topic('/x',
                                  pubs=[_endpoint('/lp', is_bridge=True)])])
    remote = _state(
        host_uuid='OTHER',
        ipc_ns=222,
        topics=[_topic('/x', subs=[_endpoint('/rs')])],
    )

    reqs = decide_bridges(local, {('OTHER', 222): remote})
    assert reqs == []


def test_decide_skips_self_namespace():
    local = _state(topics=[_topic('/x',
                                  pubs=[_endpoint('/lp')],
                                  subs=[_endpoint('/ls')])])
    # Same (host_uuid, ipc_ns) — should be ignored.
    reqs = decide_bridges(local, {('HOST', 111): local})
    assert reqs == []


def test_decide_skips_when_no_overlap():
    local = _state(topics=[_topic('/x', pubs=[_endpoint('/lp')])])
    remote = _state(
        host_uuid='OTHER',
        ipc_ns=222,
        topics=[_topic('/y', subs=[_endpoint('/rs')])],
    )
    reqs = decide_bridges(local, {('OTHER', 222): remote})
    assert reqs == []


def test_decide_skips_when_type_name_unknown_on_both_sides():
    local = _state(topics=[_topic('/x', type_name='', pubs=[_endpoint('/lp')])])
    remote = _state(
        host_uuid='OTHER',
        ipc_ns=222,
        topics=[_topic('/x', type_name='', subs=[_endpoint('/rs')])],
    )
    reqs = decide_bridges(local, {('OTHER', 222): remote})
    assert reqs == []


def test_decide_uses_remote_type_when_local_missing():
    local = _state(topics=[_topic('/x', type_name='', pubs=[_endpoint('/lp')])])
    remote = _state(
        host_uuid='OTHER',
        ipc_ns=222,
        topics=[_topic('/x', type_name='std_msgs/msg/String',
                       subs=[_endpoint('/rs')])],
    )
    reqs = decide_bridges(local, {('OTHER', 222): remote})
    assert len(reqs) == 1
    assert reqs[0].type_name == 'std_msgs/msg/String'


def test_decide_collapses_duplicates_across_remotes():
    local = _state(topics=[_topic('/x', pubs=[_endpoint('/lp')])])
    remote_a = _state(host_uuid='A', ipc_ns=1,
                      topics=[_topic('/x', subs=[_endpoint('/sa')])])
    remote_b = _state(host_uuid='B', ipc_ns=2,
                      topics=[_topic('/x', subs=[_endpoint('/sb')])])

    reqs = decide_bridges(local, {('A', 1): remote_a, ('B', 2): remote_b})
    assert len(reqs) == 1
    assert reqs[0].direction == DIRECTION_AGNOCAST_TO_ROS2


def test_serialize_wire_format_matches_cpp_struct_size():
    req = BridgeRequest(
        topic_name='/x',
        type_name='std_msgs/msg/Int32',
        direction=DIRECTION_AGNOCAST_TO_ROS2,
        qos_depth=10,
        qos_is_transient_local=False,
        qos_is_reliable=True,
    )
    payload = serialize_request(req)
    # sizeof(MqMsgDaemonBridge) on x86_64 Linux with GCC: 524 bytes.
    assert len(payload) == 524


def test_serialize_truncates_oversized_topic_name():
    req = BridgeRequest(
        topic_name='/' + 'a' * 1000,
        type_name='std_msgs/msg/Int32',
        direction=DIRECTION_AGNOCAST_TO_ROS2,
        qos_depth=10,
        qos_is_transient_local=False,
        qos_is_reliable=True,
    )
    payload = serialize_request(req)
    # The topic_name field is the first 256 bytes; the last byte must be a
    # NUL because we truncate to 255 + NUL terminator.
    assert payload[255] == 0


def test_serialize_packs_qos_flags():
    req = BridgeRequest(
        topic_name='/x',
        type_name='T',
        direction=DIRECTION_ROS2_TO_AGNOCAST,
        qos_depth=7,
        qos_is_transient_local=True,
        qos_is_reliable=True,
    )
    payload = serialize_request(req)
    # direction is uint32_t at offset 512; qos_depth at 516; flags at 520/521.
    import struct
    direction, depth = struct.unpack_from('=II', payload, 512)
    transient, reliable = struct.unpack_from('=BB', payload, 520)
    assert direction == DIRECTION_ROS2_TO_AGNOCAST
    assert depth == 7
    assert transient == 1
    assert reliable == 1

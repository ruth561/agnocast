import contextlib
import os
import time
from typing import Callable

import rclpy
from ros2cli.node.strategy import NodeStrategy
from ros2agnocast.discovery import (
    DEFAULT_COLLECT_TIMEOUT_SEC,
    collect_announcements,
    filter_fresh,
)

DEFAULT_BRIDGE_TIMEOUT = 10.0   # seconds
POLL_INTERVAL = 0.2             # seconds between publisher-count checks

_AGNOCAST_BRIDGE_NOTE = (
    '\n\nnote:\n'
    '  A dummy ros2 subscriber is first created to trigger the A2R bridge, '
    'then the standard ros2 topic command begins.'
)


def add_bridge_arguments(parser) -> None:
    """Add --bridge-timeout argument and append an Agnocast note to parser.description."""
    if parser.description:
        parser.description += _AGNOCAST_BRIDGE_NOTE
    else:
        parser.description = _AGNOCAST_BRIDGE_NOTE.lstrip()
    parser.add_argument(
        '--bridge-timeout',
        dest='bridge_timeout', type=float, default=DEFAULT_BRIDGE_TIMEOUT,
        metavar='SEC',
        help='Seconds to wait for the Agnocast bridge (ROS 2 publisher) to appear '
             '(default: %.1f). '
             'If no publisher appears within this time, the topic is assumed to not '
             'exist in Agnocast.' % DEFAULT_BRIDGE_TIMEOUT)
    parser.add_argument(
        '--gossip-timeout',
        type=float,
        default=DEFAULT_COLLECT_TIMEOUT_SEC,
        help='Seconds to wait for /_agnocast_discovery gossip from peer '
             'namespaces / ECUs (default: %(default)ss).')


def wait_for_ros2_publisher(node, topic_name: str, bridge_timeout: float, *, context=None) -> bool:
    """Poll until a ROS 2 publisher appears on topic_name.

    Returns True if a publisher is detected, False on timeout.
    """
    deadline = time.monotonic() + bridge_timeout
    while rclpy.ok(context=context) and time.monotonic() < deadline:
        if node.get_publishers_info_by_topic(topic_name):
            return True
        time.sleep(POLL_INTERVAL)
    return False


def lookup_topic_msg_type(topic_name: str, gossip_timeout: float):
    """Resolve the ROS message class for topic_name.

    First checks the live ROS 2 graph for the topic type.  If the topic is not
    found there, falls back to /_agnocast_discovery gossip to resolve the type
    for Agnocast-only topics.

    Returns the message class on success, or None if the type name cannot be
    resolved or the message class cannot be loaded (error is printed to stdout).
    """
    with NodeStrategy(None) as proxy_node:
        # First, check if the topic already exists in the ROS 2 graph.
        type_name = next(
            (types[0]
             for name, types in proxy_node.get_topic_names_and_types()
             if name == topic_name and types),
            '')

        if not type_name:
            # If not found, fall back to /_agnocast_discovery to look up the Agnocast topic.
            raw_snapshots = collect_announcements(proxy_node, timeout_sec=gossip_timeout)
            snapshots = filter_fresh(raw_snapshots, node=proxy_node)
            type_name = next(
                (topic.type_name
                 for snap in snapshots
                 for topic in snap.topics
                 if topic.topic_name == topic_name and topic.type_name),
                '')

    if not type_name:
        print(
            "ERROR: Could not resolve message type for '%s'. "
            'The topic was not found in the ROS 2 graph or via /_agnocast_discovery. '
            'Make sure the topic exists and, for Agnocast topics, the discovery agent is running.'
            % topic_name)
        return None

    try:
        from rosidl_runtime_py.utilities import get_message
        return get_message(type_name)
    except Exception as e:
        print(
            "ERROR: Could not load message class for type '%s': %s" % (type_name, e))
        return None


@contextlib.contextmanager
def _create_dummy_subscription(topic_name: str, msg_type):
    """Context manager that registers a dummy ROS 2 subscriber to trigger the A2R bridge.

    The subscription only needs to exist; no spinning is required to keep it alive.
    Uses a private rclpy context so the default context remains free for action_fn.

    Yields (node, ctx).
    """
    ctx = rclpy.Context()
    rclpy.init(context=ctx)
    node = None
    try:
        node = rclpy.create_node(
            '_ros2agnocast_cli_%d' % os.getpid(),
            context=ctx)
        node.create_subscription(msg_type, topic_name, lambda _msg: None, 10)
        yield node, ctx
    finally:
        if node is not None:
            try:
                node.destroy_node()
            except Exception:
                pass
        rclpy.try_shutdown(context=ctx)


def spawn_bridge_and_run(args, topic_name: str, action_fn: Callable) -> int:
    """Resolve the message type for topic_name, trigger the A2R bridge, and run action_fn(args).

    Returns the exit code from action_fn, or 1 on failure.
    """
    print(
        "[*] Triggering A2R bridge for '%s'. "
        'This may take a few seconds...' % topic_name)

    # Phase 1: resolve message type via the ROS 2 graph or /_agnocast_discovery gossip.
    msg_type = lookup_topic_msg_type(topic_name, args.gossip_timeout)
    if msg_type is None:
        return 1

    # Phase 2: start dummy subscription and wait for a ROS 2 publisher to appear.
    # TODO: Ideally, we should wait until bridges have come up for all Agnocast publishers
    #       across every namespace.  For simplicity, we currently only wait for a single
    #       publisher to appear.
    with _create_dummy_subscription(topic_name, msg_type) as (node, ctx):
        ready = wait_for_ros2_publisher(
            node, topic_name, args.bridge_timeout, context=ctx)

        if not ready:
            print(
                "ERROR: No ROS 2 publisher appeared on '%s' within %.1f s. "
                'The topic may not exist in Agnocast or the bridge failed to start.'
                % (topic_name, args.bridge_timeout))
            return 1

        print('[*] Starting ros2cli command...')

        # The dummy subscription stays alive (via the with block) while action_fn runs.
        return action_fn(args)

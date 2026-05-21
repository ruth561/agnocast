import time
from typing import Callable

import rclpy
from ros2cli.node.direct import DirectNode
from std_msgs.msg import String as _DummyMsgType

DEFAULT_BRIDGE_TIMEOUT = 10.0   # seconds
POLL_INTERVAL = 0.2             # seconds between publisher-count checks

_AGNOCAST_BRIDGE_NOTE = (
    '\n\nnote:\n'
    '  A dummy ros2 subscriber is first created to trigger the a2r bridge, '
    'then the standard ros2 topic command begins.'
)


def add_bridge_timeout_argument(parser) -> None:
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


def wait_for_bridge(node, topic_name: str, bridge_timeout: float) -> bool:
    """Spin until a ROS 2 publisher appears on topic_name.

    Returns True if a publisher is detected, False on timeout.
    """
    print(
        "Waiting up to %.1f s for Agnocast bridge "
        "(ROS 2 publisher on '%s') to appear..." % (bridge_timeout, topic_name))

    deadline = time.monotonic() + bridge_timeout
    while rclpy.ok() and time.monotonic() < deadline:
        rclpy.spin_once(node, timeout_sec=POLL_INTERVAL)
        if node.get_publishers_info_by_topic(topic_name):
            return True
    return False


def run_with_bridge(args, topic_name: str, action_fn: Callable) -> int:
    """Run the common bridge-wait flow and call action_fn(args) on success.

    1. Start a DirectNode and create a dummy subscriber to trigger the bridge.
    2. Wait for a ROS 2 publisher to appear via wait_for_bridge.
    3. Destroy the DirectNode (and the dummy subscriber).
       Note: action_fn (standard ros2 topic command) creates its own node,
       so destroying here is intentional.
    4. On timeout, print an error and return 1.
    5. On success, print "Bridge is ready." and return action_fn(args).
    """
    # Step 1: Register a dummy subscriber so that the Agnocast→ROS 2 bridge is created.
    # raw=True allows subscribing without knowing the actual message type at subscribe time;
    # the callback receives raw serialized bytes instead of a deserialized message object.
    # Once the bridge detects this subscriber, it spawns a ROS 2 publisher to forward
    # Agnocast messages, which we detect below as the readiness signal.
    with DirectNode(args) as node:
        _dummy_sub = node.node.create_subscription(
            _DummyMsgType,
            topic_name,
            lambda _msg: None,
            10,
            raw=True)
        bridge_ready = wait_for_bridge(node.node, topic_name, args.bridge_timeout)

        # DirectNode (and the dummy subscriber) is destroyed here.

        if not bridge_ready:
            print(
                "ERROR: No ROS 2 publisher appeared on '%s' within %.1f s. "
                'The topic may not exist in Agnocast or the bridge failed to start.' % (
                topic_name, args.bridge_timeout))
            return 1

        print('Bridge is ready.')

        # Step 2: Hand off to the standard ros2 topic implementation.
        return action_fn(args)

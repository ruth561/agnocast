import time

import rclpy
from ros2cli.node.direct import DirectNode
from ros2topic.verb.delay import DelayVerb
from ros2topic.verb.delay import main as delay_main
from std_msgs.msg import String as _DummyMsgType

DEFAULT_BRIDGE_TIMEOUT = 10.0   # seconds
POLL_INTERVAL = 0.2             # seconds between publisher-count checks


class TopicDelayAgnocastVerb(DelayVerb):
    """Display delay of topic from timestamp in header including Agnocast."""

    def add_arguments(self, parser, cli_name):
        super().add_arguments(parser, cli_name)
        parser.description = (
            'Display delay of topic from timestamp in header including Agnocast.\n\n'
            'note:\n'
            '  A dummy ROS 2 subscriber is first created to trigger the Agnocast→ROS 2 bridge, '
            'then the standard ros2 topic delay measurement begins.'
        )
        parser.add_argument(
            '--bridge-timeout',
            dest='bridge_timeout', type=float, default=DEFAULT_BRIDGE_TIMEOUT,
            metavar='SEC',
            help='Seconds to wait for the Agnocast bridge (ROS 2 publisher) to appear '
                 '(default: %.1f). '
                 'If no publisher appears within this time, the topic is assumed to not '
                 'exist in Agnocast.' % DEFAULT_BRIDGE_TIMEOUT)

    def main(self, *, args):
        # Step 1: Register a dummy subscriber so that the Agnocast→ROS 2 bridge is created.
        # raw=True allows subscribing without knowing the actual message type at subscribe time;
        # the callback receives raw serialized bytes instead of a deserialized message object.
        # Once the bridge detects this subscriber, it spawns a ROS 2 publisher to forward
        # Agnocast messages, which we detect below as the readiness signal.
        with DirectNode(args) as node:
            _dummy_sub = node.node.create_subscription(
                _DummyMsgType,
                args.topic,
                lambda _msg: None,
                10,
                raw=True)

            print(
                "Waiting up to %.1f s for Agnocast bridge "
                "(ROS 2 publisher on '%s') to appear..." % (
                    args.bridge_timeout, args.topic))

            deadline = time.monotonic() + args.bridge_timeout
            bridge_ready = False
            while rclpy.ok() and time.monotonic() < deadline:
                rclpy.spin_once(node.node, timeout_sec=POLL_INTERVAL)
                pubs = node.node.get_publishers_info_by_topic(args.topic)
                if pubs:
                    bridge_ready = True
                    break

        # DirectNode (and the dummy subscriber) is destroyed here.

        if not bridge_ready:
            print(
                "ERROR: No ROS 2 publisher appeared on '%s' within %.1f s. "
                'The topic may not exist in Agnocast or the bridge failed to start.' % (
                    args.topic, args.bridge_timeout))
            return 1

        print("Bridge is ready. Starting delay measurement...")

        # Step 2: Hand off to the standard ros2 topic delay implementation.
        return delay_main(args)

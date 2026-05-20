import time

from ros2cli.node.direct import DirectNode
from ros2topic.verb.hz import HzVerb
from ros2topic.verb.hz import main as hz_main
from std_msgs.msg import String as _DummyMsgType

BRIDGE_WAIT_TIME = 2.0  # seconds to wait for the Agnocast bridge to be created


class TopicHzAgnocastVerb(HzVerb):
    """Print the average receiving rate to screen including Agnocast."""

    def add_arguments(self, parser, cli_name):
        super().add_arguments(parser, cli_name)
        parser.description = (
            'Print the average receiving rate to screen including Agnocast.\n\n'
            'note:\n'
            '  A dummy ROS 2 subscriber is first created to trigger the Agnocast→ROS 2 bridge, '
            'then the standard ros2 topic hz measurement begins.'
        )

    def main(self, *, args):
        # Step 1: Register a dummy subscriber so that the Agnocast→ROS 2 bridge is created.
        # raw=True allows subscribing without knowing the actual message type; the callback
        # receives raw serialized bytes instead of a deserialized message object.
        with DirectNode(args) as node:
            _dummy_sub = node.node.create_subscription(
                _DummyMsgType,
                args.topic_name,
                lambda _msg: None,
                10,
                raw=True)

            print(
                'Dummy subscriber created. '
                'Waiting %.1f s for Agnocast bridge to be established...' % BRIDGE_WAIT_TIME)
            time.sleep(BRIDGE_WAIT_TIME)
        # DirectNode (and the dummy subscriber) is destroyed here.

        # Step 2: Hand off to the standard ros2 topic hz implementation.
        return hz_main(args)

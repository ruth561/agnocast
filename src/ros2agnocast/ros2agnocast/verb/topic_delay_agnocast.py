from ros2topic.verb.delay import DelayVerb
from ros2agnocast.verb._agnocast_verb_util import add_bridge_timeout_argument
from ros2agnocast.verb._agnocast_verb_util import run_with_bridge

class TopicDelayAgnocastVerb(DelayVerb):

    def add_arguments(self, parser, cli_name):
        super().add_arguments(parser, cli_name)
        add_bridge_timeout_argument(parser)

    def main(self, *, args):
        return run_with_bridge(
            args, topic_name=args.topic, action_fn=lambda a: DelayVerb.main(self, args=a))

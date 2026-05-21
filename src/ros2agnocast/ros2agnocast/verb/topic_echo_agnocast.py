from ros2topic.verb.echo import EchoVerb
from ros2agnocast.verb._agnocast_verb_util import add_bridge_timeout_argument
from ros2agnocast.verb._agnocast_verb_util import run_with_bridge

class TopicEchoAgnocastVerb(EchoVerb):

    def add_arguments(self, parser, cli_name):
        super().add_arguments(parser, cli_name)
        add_bridge_timeout_argument(parser)

    def main(self, *, args):
        return run_with_bridge(
            args, topic_name=args.topic_name, action_fn=lambda a: EchoVerb.main(self, args=a))

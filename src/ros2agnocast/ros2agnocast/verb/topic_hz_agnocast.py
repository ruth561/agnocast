from ros2cli.node.strategy import add_arguments as add_strategy_node_arguments
from ros2topic.api import TopicNameCompleter
from ros2node.verb import VerbExtension


class TopicHzAgnocastVerb(VerbExtension):
    """Print the average receiving rate to screen including Agnocast."""

    def add_arguments(self, parser, cli_name):
        add_strategy_node_arguments(parser)
        arg = parser.add_argument(
            'topic_name',
            help="Name of the ROS topic to listen to (e.g. '/chatter') including Agnocast.")
        parser.add_argument(
            '--window', '-w',
            type=int, default=10000,
            help='window size, in # of messages, for calculating rate (default: 10000)')
        parser.add_argument(
            '--filter',
            dest='filter_expr', metavar='EXPR', default=None,
            help='only measure messages matching the specified Python expression')
        parser.add_argument(
            '--wall-time',
            action='store_true',
            help='calculates rate using wall time which can be helpful when clock is not '
                 'published during simulation')
        arg.completer = TopicNameCompleter(
            include_hidden_topics_key='include_hidden_topics')

    def main(self, *, args):
        print('hello world')
        print('topic_name: %s' % args.topic_name)
        print('window: %d' % args.window)
        print('filter: %s' % args.filter_expr)
        print('wall_time: %s' % args.wall_time)
        print('spin_time: %s' % args.spin_time)
        print('use_sim_time: %s' % args.use_sim_time)
        return 0

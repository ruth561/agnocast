from ros2node.verb import VerbExtension

class TopicHzAgnocastVerb(VerbExtension):
    """Print hz information for a topic including Agnocast."""

    def add_arguments(self, parser, cli_name):
        pass

    def main(self, *, args):
        print('hello world')
        return 0

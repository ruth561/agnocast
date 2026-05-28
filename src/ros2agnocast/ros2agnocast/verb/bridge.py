import os
import socket
import glob

from ros2cli.verb import VerbExtension

CONTROL_SOCKET_BASE_DIR = '/dev/shm/agnocast_bridge_control'
PING_PAYLOAD = b'PING'
PONG_PAYLOAD = b'PONG'
DEFAULT_TIMEOUT_SEC = 1.0


def _get_self_ipc_namespace_inode() -> int:
    return os.stat('/proc/self/ns/ipc').st_ino


def _find_sock_files(ipc_inode: int) -> list[str]:
    pattern = os.path.join(CONTROL_SOCKET_BASE_DIR, str(ipc_inode), '*.sock')
    return sorted(glob.glob(pattern))


def _ping(sock_path: str, timeout_sec: float) -> str:
    """Return 'healthy', 'timeout', or 'dead'."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout_sec)
        sock.connect(sock_path)
        sock.sendall(PING_PAYLOAD)
        data = sock.recv(4)
        if data == PONG_PAYLOAD:
            return 'healthy'
        return 'dead'
    except socket.timeout:
        return 'timeout'
    except (ConnectionRefusedError, FileNotFoundError, OSError):
        return 'dead'
    finally:
        sock.close()


class BridgeVerb(VerbExtension):
    """Check liveness of Agnocast Performance Bridge processes via UDS control socket."""

    def add_arguments(self, parser, cli_name):
        parser.add_argument(
            '--namespace',
            type=int,
            default=None,
            metavar='IPC_INODE',
            help=(
                'IPC namespace inode number to check. '
                'Defaults to the caller\'s own IPC namespace.'
            ),
        )
        parser.add_argument(
            '--timeout',
            type=float,
            default=DEFAULT_TIMEOUT_SEC,
            metavar='SECONDS',
            help=f'Timeout in seconds for each PING (default: {DEFAULT_TIMEOUT_SEC})',
        )

    def main(self, *, args):
        ipc_inode = args.namespace if args.namespace is not None else _get_self_ipc_namespace_inode()
        timeout_sec = args.timeout

        sock_files = _find_sock_files(ipc_inode)

        if not sock_files:
            print(
                f'No bridge control sockets found under '
                f'{CONTROL_SOCKET_BASE_DIR}/{ipc_inode}/'
            )
            return 0

        print(f'IPC namespace inode: {ipc_inode}')
        print(f'Found {len(sock_files)} bridge(s):\n')

        any_unhealthy = False
        for sock_path in sock_files:
            pid = os.path.splitext(os.path.basename(sock_path))[0]
            status = _ping(sock_path, timeout_sec)

            if status == 'healthy':
                label = '[Healthy  ]'
            elif status == 'timeout':
                label = '[Unhealthy / Stalled]'
                any_unhealthy = True
            else:
                label = '[Dead             ]'
                any_unhealthy = True

            print(f'  PID {pid:>8s}  {label}  {sock_path}')

        return 1 if any_unhealthy else 0

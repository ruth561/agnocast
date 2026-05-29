import json
import os
import socket
import glob

from ros2cli.verb import VerbExtension

CONTROL_SOCKET_BASE_DIR = os.path.join(
    os.environ.get('AGNOCAST_TMPFS_DIR') or '/dev/shm',
    'agnocast_bridge_control',
)
DEFAULT_TIMEOUT_SEC = 1.0


def _get_self_ipc_namespace_inode() -> int:
    return os.stat('/proc/self/ns/ipc').st_ino


def _find_sock_files(ipc_inode: int) -> list[str]:
    pattern = os.path.join(CONTROL_SOCKET_BASE_DIR, str(ipc_inode), '*.sock')
    return sorted(glob.glob(pattern))


def _is_process_alive(pid: int) -> bool:
    return os.path.isdir(f'/proc/{pid}')


def _ping(sock_path: str, timeout_sec: float) -> tuple[str, dict]:
    """Connect to the control socket and return (status, info).

    The daemon sends a JSON payload immediately on connection:
    ``{"type":"standard"|"performance","ipc_ns":<int>,"pid":<int>}``

    status is one of 'healthy', 'timeout', 'dead'.
    info is the parsed JSON dict on success, or {} otherwise.
    """
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout_sec)
        sock.connect(sock_path)
        chunks = []
        while True:
            chunk = sock.recv(256)
            if not chunk:
                break
            chunks.append(chunk)
        info = json.loads(b''.join(chunks).decode())
        return 'healthy', info
    except socket.timeout:
        return 'timeout', {}
    except (ConnectionRefusedError, FileNotFoundError, OSError, json.JSONDecodeError):
        return 'dead', {}
    finally:
        sock.close()


class BridgeDaemonStatusVerb(VerbExtension):
    """Check liveness of Agnocast Bridge daemon processes via UDS control socket."""

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
            help=f'Timeout in seconds for each ping (default: {DEFAULT_TIMEOUT_SEC})',
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
            pid_str = os.path.splitext(os.path.basename(sock_path))[0]
            try:
                pid = int(pid_str)
            except ValueError:
                continue

            if not _is_process_alive(pid):
                label = '[Stale   ]'
                extra = 'process no longer exists'
                any_unhealthy = True
                print(f'  PID {pid_str:>8s}  {label}  {extra}  {sock_path}')
                continue

            status, info = _ping(sock_path, timeout_sec)

            if status == 'healthy':
                label = '[Healthy  ]'
                bridge_type = info.get('type', 'unknown')
                extra = f'type={bridge_type}  ipc_ns={info.get("ipc_ns")}  pid={info.get("pid")}'
            else:
                label = '[Unhealthy]'
                extra = 'process alive but ping failed'
                any_unhealthy = True

            print(f'  PID {pid_str:>8s}  {label}  {extra}  {sock_path}')

        return 1 if any_unhealthy else 0

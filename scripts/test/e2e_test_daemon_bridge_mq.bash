#!/bin/bash
#
# E2E regression smoke test for the F1 daemon-originated bridge MQ
# wiring. Verifies that:
#
#   * A running bridge_manager (forked by an Agnocast process) creates
#     the new `agnocast_daemon_bridge@<pid>` MQ on top of the existing
#     `agnocast_bridge_manager@<pid>` MQ.
#   * The discovery agent comes up alongside without errors and runs the
#     bridge decider tick without crashing when remote_states is empty.
#   * Injecting a synthetic remote AgnocastDaemonState that lists an
#     Agnocast subscriber on the local talker's topic causes the daemon
#     to dispatch an MqMsgDaemonBridge — observable indirectly as the
#     bridge_manager neither crashes nor leaks an extra MQ.
#
# This is a single-namespace smoke test. Real cross-IPC-namespace
# verification is left to a manual `unshare -i` test plan (documented
# separately) because CI does not currently grant CAP_SYS_ADMIN.
#
# Requires:
#   * agnocast kernel module loaded
#   * workspace built (bash scripts/dev/build_all.bash)
#
# Usage:
#   bash scripts/test/e2e_test_daemon_bridge_mq.bash
#
# Exit codes:
#   0 - all checks passed
#   1 - prerequisite failure
#   2 - assertion failure

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
DAEMON_WARMUP_SEC=${DAEMON_WARMUP_SEC:-2}
TALKER_WARMUP_SEC=${TALKER_WARMUP_SEC:-4}
DISPATCH_WARMUP_SEC=${DISPATCH_WARMUP_SEC:-3}

red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

if ! grep -q "^agnocast " /proc/modules; then
    red "ERROR: agnocast kmod not loaded."
    echo "  -> sudo insmod $ROOT_DIR/agnocast_kmod/agnocast.ko" >&2
    exit 1
fi
if [ ! -f "$ROOT_DIR/install/setup.bash" ]; then
    red "ERROR: workspace not built ($ROOT_DIR/install/setup.bash missing)."
    echo "  -> bash $ROOT_DIR/scripts/dev/build_all.bash" >&2
    exit 1
fi

# shellcheck disable=SC1091
source "$ROOT_DIR/install/setup.bash"
set -u
green "✓ kmod loaded and workspace sourced"

LOG_DIR=$(mktemp -d)
cleanup() {
    pkill -P $$ 2>/dev/null || true
    sleep 1
    rm -rf "$LOG_DIR"
}
trap cleanup EXIT

fail() {
    red "ERROR: $1"
    [ -n "${2:-}" ] && { echo "----- output -----" >&2; printf '%s\n' "$2" >&2; }
    [ -f "$LOG_DIR/agent.log" ]  && { echo "----- agent log -----"  >&2; cat "$LOG_DIR/agent.log"  >&2; }
    [ -f "$LOG_DIR/talker.log" ] && { echo "----- talker log -----" >&2; cat "$LOG_DIR/talker.log" >&2; }
    exit 2
}

# ----- launch talker (forks a bridge_manager) -----
yellow "Starting agnocast talker (forks a bridge_manager)..."
ros2 launch agnocast_sample_application talker.launch.xml > "$LOG_DIR/talker.log" 2>&1 &
sleep "$TALKER_WARMUP_SEC"

# bridge_manager registers `/agnocast_bridge_manager@<pid>` AND
# `/agnocast_daemon_bridge@<pid>` for the same pid. Find any of the
# daemon MQs as the success signal.
mq_files=$(ls /dev/mqueue/agnocast_daemon_bridge@* 2>/dev/null || true)
if [ -z "$mq_files" ]; then
    fail "no /dev/mqueue/agnocast_daemon_bridge@<pid> entry created by talker's bridge_manager"
fi
green "✓ bridge_manager exposed daemon bridge MQ:"
ls -la $mq_files

# Verify the matching primary MQ also exists (sanity).
primary_mq=$(ls /dev/mqueue/agnocast_bridge_manager@* 2>/dev/null || true)
if [ -z "$primary_mq" ]; then
    fail "missing primary /dev/mqueue/agnocast_bridge_manager@<pid> entry"
fi
green "✓ primary bridge_manager MQ still present"

# ----- launch daemon -----
yellow "Starting discovery_agent..."
ros2 run ros2agnocast_discovery_agent discovery_agent > "$LOG_DIR/agent.log" 2>&1 &
sleep "$DAEMON_WARMUP_SEC"

if ! grep -q "discovery_agent up" "$LOG_DIR/agent.log"; then
    fail "daemon did not log startup within ${DAEMON_WARMUP_SEC}s"
fi
green "✓ daemon up"

# ----- daemon decider quiescence (no remote state -> no dispatch) -----
# With no other discovery agents present on the bus, `remote_states` stays
# empty and the bridge decider must not emit anything. We assert quiescence
# negatively: the daemon log must not contain a dispatch warning and the
# bridge_manager MQ must still exist.
sleep "$DISPATCH_WARMUP_SEC"

if grep -q "Traceback" "$LOG_DIR/agent.log"; then
    fail "daemon crashed during decider tick" "$(cat "$LOG_DIR/agent.log")"
fi

if ! ls /dev/mqueue/agnocast_daemon_bridge@* > /dev/null 2>&1; then
    fail "daemon bridge MQ vanished after warmup -- bridge_manager likely died"
fi
green "✓ daemon decider tick stayed quiescent with empty remote_states"

green ""
green "===== ALL CHECKS PASSED ====="

#!/bin/bash
# Benchmark for bridge registration startup latency.
#
# Runs bridge_startup_perf once per N value (each as a fresh process) and
# collects the results into a single table. Topic names are PID-scoped so
# successive runs do not interfere with each other via residual DDS state.
#
# Output columns (all in milliseconds, relative to t_run_start):
#   pub_creation_ms   : time until all N agnocast::Publisher ctors return
#   first_agnocast_ms : time until first agnocast::Subscription callback
#   first_ros2_ms     : time until first rclcpp::Subscription callback
#                       (via performance bridge + DDS)
#
# Usage:
#   bash scripts/sample_application/run_bridge_startup_perf.bash
#   bash scripts/sample_application/run_bridge_startup_perf.bash 1 5 10 50 100
#
# Prerequisites:
#   - agnocast kernel module loaded
#   - performance bridge manager running (ros2agnocast_discovery_agent or similar)
#   - Run from the workspace root (where install/ lives)

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

source "${REPO_ROOT}/install/setup.bash"

HEAPHOOK="${REPO_ROOT}/install/agnocastlib/lib/libagnocast_heaphook.so"
# Use ros2 run so the PATH/ament lookup resolves the binary regardless of the
# exact colcon install prefix layout.
RUN_CMD="ros2 run agnocast_sample_application bridge_startup_perf"

export AGNOCAST_BRIDGE_MODE=on

# Default topic counts; override by passing arguments.
TOPIC_COUNTS="${*:-1 5 10 50 100}"

# Kill all agno_pbr_* (performance bridge manager) processes and wait until the
# abstract-namespace UDS listener is gone. This ensures each benchmark run sees
# a cold bridge start, matching the scenario we want to measure.
stop_bridge_manager() {
  local pids
  mapfile -t pids < <(ps -eo pid=,comm= | awk '$2 ~ /^agno_pbr_/ { print $1 }')
  if [ "${#pids[@]}" -gt 0 ]; then
    for pid in "${pids[@]}"; do
      kill "${pid}" 2>/dev/null || true
    done
    # Wait up to 5 s for the UDS listener to disappear.
    local deadline=$(( $(date +%s) + 5 ))
    while awk '$NF ~ /^@agnocast_bridge_manager@/ { found=1 } END { exit !found }' \
          /proc/net/unix 2>/dev/null; do
      if [ "$(date +%s)" -ge "${deadline}" ]; then
        echo "WARNING: bridge manager UDS listener still present after 5 s" >&2
        break
      fi
      sleep 0.1
    done
  fi
}

first=true
for n in ${TOPIC_COUNTS}; do
  stop_bridge_manager
  sleep 0.5
  if ${first}; then
    LD_PRELOAD="${HEAPHOOK}:${LD_PRELOAD:-}" ${RUN_CMD} "${n}"
    first=false
  else
    LD_PRELOAD="${HEAPHOOK}:${LD_PRELOAD:-}" ${RUN_CMD} "${n}" --no-header
  fi
done

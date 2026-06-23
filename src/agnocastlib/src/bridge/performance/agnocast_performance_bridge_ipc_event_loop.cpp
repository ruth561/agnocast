#include "agnocast/bridge/performance/agnocast_performance_bridge_ipc_event_loop.hpp"

#include "agnocast/agnocast_mq.hpp"
#include "agnocast/agnocast_utils.hpp"

#include <utility>
#include <vector>

namespace agnocast
{

PerformanceBridgeIpcEventLoop::PerformanceBridgeIpcEventLoop(const rclcpp::Logger & logger)
: IpcEventLoopBase(
    logger,
    // 1. Abstract-namespace UDS address of the bridge_manager listener.
    create_uds_addr_for_bridge(PERFORMANCE_BRIDGE_VIRTUAL_PID),
    // 2. Wire-format payload size received per connection.
    PERFORMANCE_BRIDGE_MSG_SIZE,
    // 3. Block Signals
    {SIGTERM, SIGINT},
    // 4. Ignore Signals
    {SIGPIPE, SIGHUP})
{
}

}  // namespace agnocast

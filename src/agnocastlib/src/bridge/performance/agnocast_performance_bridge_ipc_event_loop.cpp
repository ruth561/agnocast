#include "agnocast/bridge/performance/agnocast_performance_bridge_ipc_event_loop.hpp"

#include <utility>
#include <vector>

namespace agnocast
{

PerformanceBridgeIpcEventLoop::PerformanceBridgeIpcEventLoop(const rclcpp::Logger & logger)
: IpcEventLoopBase(
    logger,
    // Block Signals
    {SIGTERM, SIGINT},
    // Ignore Signals
    {SIGPIPE, SIGHUP})
{
}

}  // namespace agnocast

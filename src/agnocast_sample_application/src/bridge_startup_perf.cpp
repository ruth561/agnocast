// Bridge startup latency benchmark — one N value per invocation.
//
// Spawns N threads simultaneously. Each thread calls agnocast::create_publisher
// on a unique topic, which sends a bridge registration request via UDS to the
// performance bridge manager. The total wall time from "go" signal to the last
// thread completing its constructor is reported as `pub_creation_ms`.
//
// Parallel threads are used to expose contention in the bridge registration
// path that a single-threaded sequential loop would not reveal.
//
// Prerequisites
//   - agnocast kernel module loaded
//   - agnocast performance bridge manager running (or will start during the run)
//   - launched with LD_PRELOAD=<install>/lib/libagnocast_heaphook.so
//   - AGNOCAST_BRIDGE_MODE=on
//
// Usage
//   bridge_startup_perf <N> [--no-header]
//
// Flags
//   --no-header  suppress the two-line table header (for use in a loop)

#include "agnocast/agnocast.hpp"
#include "agnocast_sample_interfaces/msg/static_size_array.hpp"
#include "rclcpp/rclcpp.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>

using MsgT = agnocast_sample_interfaces::msg::StaticSizeArray;
using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;

namespace
{

double to_ms(Clock::duration d)
{
  return static_cast<double>(std::chrono::duration_cast<Ns>(d).count()) / 1e6;
}

// Unique topic per process and index so successive invocations do not share
// residual DDS discovery state.
std::string make_topic(int idx)
{
  return "/bench/pid" + std::to_string(getpid()) + "/topic_" + std::to_string(idx);
}

// ---- Per-N benchmark run ----------------------------------------------------
//
// All N publisher constructors run in parallel. A shared_future acts as the
// "go" signal so the threads are released as simultaneously as possible.
// Timing starts just before set_value() and ends after the last join().

double run_benchmark(int n)
{
  // One lightweight node shared across all publisher threads.
  // rclcpp::Node internals are thread-safe for concurrent publisher creation.
  auto node = std::make_shared<rclcpp::Node>("bridge_startup_perf");

  // Per-thread publisher storage (indexed so writes are non-overlapping).
  std::vector<agnocast::Publisher<MsgT>::SharedPtr> pubs(n);

  // Start-pistol: all threads wait on this before touching the bridge.
  std::promise<void> go_promise;
  auto go = go_promise.get_future().share();

  std::vector<std::thread> threads;
  threads.reserve(n);
  for (int i = 0; i < n; ++i) {
    threads.emplace_back([&, i]() {
      go.wait();  // spin until the go signal
      pubs[i] = agnocast::create_publisher<MsgT>(node.get(), make_topic(i), rclcpp::QoS(1));
    });
  }

  // Release all threads at once; start the clock in the same instant.
  const auto t_start = Clock::now();
  go_promise.set_value();

  for (auto & t : threads) t.join();
  const auto t_end = Clock::now();

  return to_ms(t_end - t_start);
}

// ---- Output helpers ---------------------------------------------------------

void print_header()
{
  std::printf("%-6s  %18s\n", "N", "pub_creation_ms");
  std::printf("%-6s  %18s\n", "------", "------------------");
  std::fflush(stdout);
}

void print_result(int n, double ms)
{
  std::printf("%-6d  %18.1f\n", n, ms);
  std::fflush(stdout);
}

}  // namespace

// ---- main -------------------------------------------------------------------

int main(int argc, char * argv[])
{
  int n = 0;
  bool no_header = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--ros-args") == 0) break;
    if (std::strcmp(argv[i], "--no-header") == 0) {
      no_header = true;
      continue;
    }
    if (n == 0) {
      const int v = std::atoi(argv[i]);
      if (v > 0) n = v;
    }
  }

  if (n <= 0) {
    std::fprintf(
      stderr,
      "Usage  : bridge_startup_perf <N> [--no-header]\n"
      "\n"
      "  N            number of parallel publisher threads (positive integer)\n"
      "  --no-header  suppress the two-line table header\n"
      "\n"
      "Prerequisites:\n"
      "  - agnocast kernel module loaded\n"
      "  - agnocast performance bridge manager running\n"
      "  - LD_PRELOAD=<install>/lib/libagnocast_heaphook.so\n"
      "  - AGNOCAST_BRIDGE_MODE=on\n"
      "\n"
      "Metric:\n"
      "  pub_creation_ms : wall time from 'go' signal to last thread completing\n"
      "                    agnocast::create_publisher() (includes UDS bridge\n"
      "                    registration retries if the bridge manager is starting)\n");
    return 1;
  }

  rclcpp::init(argc, argv);

  if (!no_header) print_header();
  print_result(n, run_benchmark(n));

  rclcpp::shutdown();
  return 0;
}

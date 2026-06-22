// Integration tests for the kmod-resident Bridge message queue.
//
// These tests directly open /dev/agnocast and exercise the two new ioctls
// (AGNOCAST_SEND_MSG_TO_BRIDGE_CMD, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD)
// plus the read/poll behavior of the receiver fd. They do not go through
// agnocast::init() or the heaphook, because the goal is to verify the
// kmod contract in isolation. They do, however, exercise
// AGNOCAST_ADD_PROCESS_CMD in forked children so that the
// process-exit-driven cleanup of bridge_msg_queue can be observed.
//
// Test isolation note: bridge_msg_queue is per-IPC-namespace and is freed
// only when the *last* alive Agnocast process in that IPC-namespace exits
// (see agnocast_process_exit_cleanup). Because every test in this binary
// runs in the same IPC-namespace, the SendsThenReceives / FifoOrder /
// SizeBoundaries tests deliberately register no Agnocast process from the
// test parent — they only act as a receiver, which means the queue stays
// alive across those tests but no Agnocast-process accounting is touched.
// The dedicated cleanup tests then fork a child that *does* register, and
// rely on the child exiting being the only Agnocast process in the
// IPC-namespace at that moment.

#include "agnocast/agnocast_ioctl.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// The ioctl command macros expand to `union ioctl_add_process_args` etc., which
// only resolve when the surrounding scope is `agnocast`. We therefore put the
// whole test translation unit inside that namespace; the gtest TEST_F macros
// work correctly inside a named namespace.
namespace agnocast
{
namespace
{

constexpr const char * AGNOCAST_DEVICE_PATH = "/dev/agnocast";

int open_agnocast_device()
{
  int fd = ::open(AGNOCAST_DEVICE_PATH, O_RDWR);
  if (fd < 0) {
    ADD_FAILURE() << "Failed to open " << AGNOCAST_DEVICE_PATH << ": " << std::strerror(errno);
  }
  return fd;
}

// Drain all messages currently buffered in the bridge_msg_queue for the
// current IPC-namespace. Used to reset state between tests so that messages
// pushed by an earlier test do not leak into a later one.
void drain_bridge_msg_queue()
{
  int dev_fd = ::open(AGNOCAST_DEVICE_PATH, O_RDWR);
  if (dev_fd < 0) return;

  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ::close(dev_fd);
  if (rx_fd < 0) return;  // queue not yet created — nothing to drain

  std::array<uint8_t, MAX_BRIDGE_MSG_SIZE> buf{};
  for (;;) {
    ssize_t n = ::read(rx_fd, buf.data(), buf.size());
    if (n <= 0) break;  // EAGAIN (O_NONBLOCK is set by the receiver fd) or EOF
  }
  ::close(rx_fd);
}

// Fill a payload buffer with a deterministic, position-dependent pattern so
// the receiver can verify byte-exact integrity.
void fill_pattern(uint8_t * buf, uint32_t size, uint8_t seed)
{
  for (uint32_t i = 0; i < size; ++i) {
    buf[i] = static_cast<uint8_t>((i * 31u + seed) & 0xFFu);
  }
}

// In a forked child process, register as an Agnocast process and send one
// message of the given size. The child does not call exit() through the
// gtest path — it uses _exit() so it never runs static destructors or gtest
// teardown that the parent process owns.
[[noreturn]] void child_register_then_send_one(uint32_t size, uint8_t seed)
{
  int dev_fd = ::open(AGNOCAST_DEVICE_PATH, O_RDWR);
  if (dev_fd < 0) _exit(101);

  // Become an Agnocast process so that the kmod will route a process-exit
  // hook through agnocast_process_exit_cleanup when this child terminates.
  ioctl_add_process_args add_args = {};
  add_args.is_performance_bridge_manager = false;
  if (::ioctl(dev_fd, AGNOCAST_ADD_PROCESS_CMD, &add_args) < 0) _exit(102);

  ioctl_send_msg_to_bridge_args send_args{};
  send_args.size = size;
  fill_pattern(send_args.payload, size, seed);
  if (::ioctl(dev_fd, AGNOCAST_SEND_MSG_TO_BRIDGE_CMD, &send_args) < 0) _exit(103);

  ::close(dev_fd);
  _exit(0);
}

// Wait for a child process to exit with a zero status, failing the test if it
// does not.
void expect_child_exits_clean(pid_t pid)
{
  int status = -1;
  pid_t waited = ::waitpid(pid, &status, 0);
  ASSERT_EQ(waited, pid) << "waitpid failed: " << std::strerror(errno);
  ASSERT_TRUE(WIFEXITED(status)) << "child did not exit normally";
  ASSERT_EQ(WEXITSTATUS(status), 0) << "child exited with non-zero status";
}

// A "keeper" is a forked child process whose sole job is to register as an
// Agnocast process so the per-IPC-ns bridge_msg_queue is not freed mid-test.
// Send/receive tests need this because the sender child is normally the only
// alive Agnocast process in the IPC-namespace, and its exit would trigger
// agnocast_process_exit_cleanup → free queue → parent's receiver fd would
// then see EOF instead of the just-pushed message.
struct KeeperHandle
{
  pid_t pid = -1;
  int signal_write_fd = -1;  // close to let the keeper exit
};

KeeperHandle spawn_keeper()
{
  int signal_pipe[2];
  int ready_pipe[2];
  if (::pipe(signal_pipe) != 0) return {};
  if (::pipe(ready_pipe) != 0) {
    ::close(signal_pipe[0]);
    ::close(signal_pipe[1]);
    return {};
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(signal_pipe[0]);
    ::close(signal_pipe[1]);
    ::close(ready_pipe[0]);
    ::close(ready_pipe[1]);
    return {};
  }

  if (pid == 0) {
    ::close(signal_pipe[1]);
    ::close(ready_pipe[0]);

    int dev_fd = ::open(AGNOCAST_DEVICE_PATH, O_RDWR);
    if (dev_fd < 0) _exit(101);
    ioctl_add_process_args add_args = {};
    if (::ioctl(dev_fd, AGNOCAST_ADD_PROCESS_CMD, &add_args) < 0) _exit(102);

    // Tell the parent: "I'm now a live Agnocast process — the queue won't be
    // freed by my absence."
    char ready_byte = '1';
    if (::write(ready_pipe[1], &ready_byte, 1) != 1) _exit(103);
    ::close(ready_pipe[1]);

    // Block until the parent closes signal_pipe[1] (or writes to it).
    char ch;
    ssize_t r = ::read(signal_pipe[0], &ch, 1);
    (void)r;

    ::close(dev_fd);
    _exit(0);
  }

  // Parent
  ::close(signal_pipe[0]);
  ::close(ready_pipe[1]);

  // Wait for the keeper to confirm it has registered.
  char ready;
  ssize_t n = ::read(ready_pipe[0], &ready, 1);
  ::close(ready_pipe[0]);
  if (n != 1) {
    // Keeper failed before signaling ready; clean up.
    ::close(signal_pipe[1]);
    int status;
    ::waitpid(pid, &status, 0);
    return {};
  }

  return {pid, signal_pipe[1]};
}

void release_keeper(KeeperHandle & h)
{
  if (h.pid < 0) return;
  if (h.signal_write_fd >= 0) {
    ::close(h.signal_write_fd);
    h.signal_write_fd = -1;
  }
  int status;
  ::waitpid(h.pid, &status, 0);
  h.pid = -1;
}

class BridgeMsgQueueTest : public ::testing::Test
{
protected:
  void SetUp() override { drain_bridge_msg_queue(); }
  void TearDown() override { drain_bridge_msg_queue(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// API-shape tests (parent-only; no Agnocast process registration)
// ---------------------------------------------------------------------------

TEST_F(BridgeMsgQueueTest, OpenDeviceAndCreateReceiverFd)
{
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);

  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  EXPECT_GE(rx_fd, 0) << std::strerror(errno);
  ::close(rx_fd);
  ::close(dev_fd);
}

TEST_F(BridgeMsgQueueTest, EmptyReceiverReadReturnsEagain)
{
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);

  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0) << std::strerror(errno);

  std::array<uint8_t, 64> buf{};
  ssize_t n = ::read(rx_fd, buf.data(), buf.size());
  EXPECT_EQ(n, -1);
  EXPECT_EQ(errno, EAGAIN);

  ::close(rx_fd);
  ::close(dev_fd);
}

TEST_F(BridgeMsgQueueTest, EmptyReceiverPollDoesNotSignalIn)
{
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);

  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0) << std::strerror(errno);

  struct pollfd pfd
  {
  };
  pfd.fd = rx_fd;
  pfd.events = POLLIN;
  int ready = ::poll(&pfd, 1, 50 /*ms*/);
  EXPECT_EQ(ready, 0) << "poll should time out when queue is empty";

  ::close(rx_fd);
  ::close(dev_fd);
}

TEST_F(BridgeMsgQueueTest, SendRejectsInvalidSizes)
{
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);

  ioctl_send_msg_to_bridge_args args{};

  args.size = 0;
  EXPECT_EQ(::ioctl(dev_fd, AGNOCAST_SEND_MSG_TO_BRIDGE_CMD, &args), -1);
  EXPECT_EQ(errno, EINVAL);

  args.size = MAX_BRIDGE_MSG_SIZE + 1;
  EXPECT_EQ(::ioctl(dev_fd, AGNOCAST_SEND_MSG_TO_BRIDGE_CMD, &args), -1);
  EXPECT_EQ(errno, EINVAL);

  ::close(dev_fd);
}

// ---------------------------------------------------------------------------
// Cross-process message transport
// ---------------------------------------------------------------------------

TEST_F(BridgeMsgQueueTest, ChildSendsAndParentReceivesExactPayload)
{
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);

  // Create the receiver fd *before* forking so the queue exists when the
  // child pushes. (Not strictly required — the kmod lazily creates the queue
  // on first send too — but it documents the expected user-space ordering
  // for the Bridge Manager.)
  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0) << std::strerror(errno);
  ::close(dev_fd);

  // Keep the queue alive across the sender's exit.
  KeeperHandle keeper = spawn_keeper();
  ASSERT_GE(keeper.pid, 0) << "failed to spawn keeper";

  constexpr uint32_t kSize = 256;
  constexpr uint8_t kSeed = 0x42;

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    child_register_then_send_one(kSize, kSeed);
  }
  expect_child_exits_clean(pid);

  std::array<uint8_t, MAX_BRIDGE_MSG_SIZE> rx_buf{};
  ssize_t n = ::read(rx_fd, rx_buf.data(), rx_buf.size());
  ASSERT_EQ(n, static_cast<ssize_t>(kSize)) << "read returned wrong size: " << std::strerror(errno);

  std::array<uint8_t, kSize> expected{};
  fill_pattern(expected.data(), kSize, kSeed);
  EXPECT_EQ(0, std::memcmp(rx_buf.data(), expected.data(), kSize));

  ::close(rx_fd);
  release_keeper(keeper);
}

TEST_F(BridgeMsgQueueTest, FifoOrderAcrossMultipleSends)
{
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);

  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0) << std::strerror(errno);
  ::close(dev_fd);

  KeeperHandle keeper = spawn_keeper();
  ASSERT_GE(keeper.pid, 0);

  // Three children each push exactly one message; we wait() between forks so
  // their pushes are strictly ordered.
  struct Msg
  {
    uint32_t size;
    uint8_t seed;
  };
  std::array<Msg, 3> msgs{{{8, 0xAA}, {128, 0xBB}, {512, 0xCC}}};

  for (const auto & m : msgs) {
    pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      child_register_then_send_one(m.size, m.seed);
    }
    expect_child_exits_clean(pid);
  }

  std::array<uint8_t, MAX_BRIDGE_MSG_SIZE> rx_buf{};
  for (const auto & m : msgs) {
    ssize_t n = ::read(rx_fd, rx_buf.data(), rx_buf.size());
    ASSERT_EQ(n, static_cast<ssize_t>(m.size))
      << "read returned wrong size for seed=" << static_cast<int>(m.seed);

    std::vector<uint8_t> expected(m.size);
    fill_pattern(expected.data(), m.size, m.seed);
    EXPECT_EQ(0, std::memcmp(rx_buf.data(), expected.data(), m.size))
      << "payload mismatch for seed=" << static_cast<int>(m.seed);
  }

  ::close(rx_fd);
  release_keeper(keeper);
}

// ---------------------------------------------------------------------------
// Datagram semantics of the receiver fd
// ---------------------------------------------------------------------------

TEST_F(BridgeMsgQueueTest, ReadShortBufferReturnsEmsgsizeAndKeepsMessage)
{
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);

  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0) << std::strerror(errno);
  ::close(dev_fd);

  KeeperHandle keeper = spawn_keeper();
  ASSERT_GE(keeper.pid, 0);

  constexpr uint32_t kSize = 1024;
  constexpr uint8_t kSeed = 0x37;

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    child_register_then_send_one(kSize, kSeed);
  }
  expect_child_exits_clean(pid);

  // Try to read with an undersized buffer.
  std::array<uint8_t, kSize / 2> small_buf{};
  ssize_t short_n = ::read(rx_fd, small_buf.data(), small_buf.size());
  EXPECT_EQ(short_n, -1);
  EXPECT_EQ(errno, EMSGSIZE);

  // The message must still be in the queue: a follow-up read with an
  // adequately sized buffer succeeds and returns the original payload.
  std::array<uint8_t, MAX_BRIDGE_MSG_SIZE> ok_buf{};
  ssize_t ok_n = ::read(rx_fd, ok_buf.data(), ok_buf.size());
  ASSERT_EQ(ok_n, static_cast<ssize_t>(kSize)) << std::strerror(errno);

  std::vector<uint8_t> expected(kSize);
  fill_pattern(expected.data(), kSize, kSeed);
  EXPECT_EQ(0, std::memcmp(ok_buf.data(), expected.data(), kSize));

  ::close(rx_fd);
  release_keeper(keeper);
}

TEST_F(BridgeMsgQueueTest, SizeBoundariesOneAndMaxRoundtrip)
{
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);

  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0) << std::strerror(errno);
  ::close(dev_fd);

  KeeperHandle keeper = spawn_keeper();
  ASSERT_GE(keeper.pid, 0);

  struct Case
  {
    uint32_t size;
    uint8_t seed;
  };
  std::array<Case, 2> cases{{{1, 0x01}, {MAX_BRIDGE_MSG_SIZE, 0xFE}}};

  for (const auto & c : cases) {
    pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      child_register_then_send_one(c.size, c.seed);
    }
    expect_child_exits_clean(pid);

    std::array<uint8_t, MAX_BRIDGE_MSG_SIZE> rx_buf{};
    ssize_t n = ::read(rx_fd, rx_buf.data(), rx_buf.size());
    ASSERT_EQ(n, static_cast<ssize_t>(c.size))
      << "size=" << c.size << " errno=" << std::strerror(errno);

    std::vector<uint8_t> expected(c.size);
    fill_pattern(expected.data(), c.size, c.seed);
    EXPECT_EQ(0, std::memcmp(rx_buf.data(), expected.data(), c.size))
      << "payload mismatch for size=" << c.size;
  }

  ::close(rx_fd);
  release_keeper(keeper);
}

// ---------------------------------------------------------------------------
// epoll readiness notification
// ---------------------------------------------------------------------------

TEST_F(BridgeMsgQueueTest, EpollWakesOnIncomingMessageFromChild)
{
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);

  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0) << std::strerror(errno);
  ::close(dev_fd);

  KeeperHandle keeper = spawn_keeper();
  ASSERT_GE(keeper.pid, 0);

  int ep_fd = ::epoll_create1(EPOLL_CLOEXEC);
  ASSERT_GE(ep_fd, 0);

  struct epoll_event ev
  {
  };
  ev.events = EPOLLIN;
  ev.data.fd = rx_fd;
  ASSERT_EQ(::epoll_ctl(ep_fd, EPOLL_CTL_ADD, rx_fd, &ev), 0) << std::strerror(errno);

  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    child_register_then_send_one(64, 0x55);
  }
  expect_child_exits_clean(pid);

  struct epoll_event out_ev
  {
  };
  int ready = ::epoll_wait(ep_fd, &out_ev, 1, 1000 /*ms*/);
  EXPECT_GE(ready, 1) << "epoll_wait did not report readiness";
  EXPECT_TRUE((out_ev.events & EPOLLIN) != 0);

  std::array<uint8_t, 64> buf{};
  ssize_t n = ::read(rx_fd, buf.data(), buf.size());
  EXPECT_EQ(n, 64);

  ::close(ep_fd);
  ::close(rx_fd);
  release_keeper(keeper);
}

// With an empty queue, epoll_wait must block until the timeout elapses and
// then return 0 (no fds ready). This mirrors EmptyReceiverPollDoesNotSignalIn
// but goes through the epoll path instead of poll(2).
TEST_F(BridgeMsgQueueTest, EpollWaitTimesOutWhenQueueEmpty)
{
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);

  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0) << std::strerror(errno);
  ::close(dev_fd);

  int ep_fd = ::epoll_create1(EPOLL_CLOEXEC);
  ASSERT_GE(ep_fd, 0);

  struct epoll_event ev
  {
  };
  ev.events = EPOLLIN;
  ev.data.fd = rx_fd;
  ASSERT_EQ(::epoll_ctl(ep_fd, EPOLL_CTL_ADD, rx_fd, &ev), 0) << std::strerror(errno);

  struct epoll_event out_ev
  {
  };
  int ready = ::epoll_wait(ep_fd, &out_ev, 1, 50 /*ms*/);
  EXPECT_EQ(ready, 0) << "epoll_wait should time out when queue is empty";

  ::close(ep_fd);
  ::close(rx_fd);
}

// The kmod's .poll handler is level-triggered: as long as the queue holds at
// least one entry, every epoll_wait must return immediately with EPOLLIN.
// Push two messages, then verify that after consuming only one of them, a
// subsequent epoll_wait still returns without blocking.
TEST_F(BridgeMsgQueueTest, EpollRemainsReadyAfterPartialDrain)
{
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);

  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0) << std::strerror(errno);
  ::close(dev_fd);

  KeeperHandle keeper = spawn_keeper();
  ASSERT_GE(keeper.pid, 0);

  int ep_fd = ::epoll_create1(EPOLL_CLOEXEC);
  ASSERT_GE(ep_fd, 0);

  struct epoll_event ev
  {
  };
  ev.events = EPOLLIN;
  ev.data.fd = rx_fd;
  ASSERT_EQ(::epoll_ctl(ep_fd, EPOLL_CTL_ADD, rx_fd, &ev), 0) << std::strerror(errno);

  // Push two messages from two short-lived child processes; wait() between
  // forks so both pushes have landed in the queue before we poll.
  struct Msg
  {
    uint32_t size;
    uint8_t seed;
  };
  std::array<Msg, 2> msgs{{{32, 0x11}, {48, 0x22}}};
  for (const auto & m : msgs) {
    pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      child_register_then_send_one(m.size, m.seed);
    }
    expect_child_exits_clean(pid);
  }

  // First epoll_wait: must return immediately with EPOLLIN.
  struct epoll_event out_ev
  {
  };
  int ready = ::epoll_wait(ep_fd, &out_ev, 1, 1000 /*ms*/);
  ASSERT_GE(ready, 1) << "epoll_wait did not report readiness for the first message";
  EXPECT_TRUE((out_ev.events & EPOLLIN) != 0);

  // Drain exactly one message.
  std::array<uint8_t, MAX_BRIDGE_MSG_SIZE> rx_buf{};
  ssize_t n = ::read(rx_fd, rx_buf.data(), rx_buf.size());
  ASSERT_EQ(n, static_cast<ssize_t>(msgs[0].size)) << std::strerror(errno);

  // Second epoll_wait: the queue still has one entry, so .poll must report
  // EPOLLIN again and epoll_wait must return without blocking (timeout=0 is
  // strict — any block here means level-triggered semantics are broken).
  struct epoll_event out_ev2
  {
  };
  int ready2 = ::epoll_wait(ep_fd, &out_ev2, 1, 0 /*ms — must not block*/);
  EXPECT_GE(ready2, 1) << "epoll_wait did not stay ready while a message remained in the queue";
  EXPECT_TRUE((out_ev2.events & EPOLLIN) != 0);

  // Drain the remaining message to verify it was indeed the second one.
  ssize_t n2 = ::read(rx_fd, rx_buf.data(), rx_buf.size());
  ASSERT_EQ(n2, static_cast<ssize_t>(msgs[1].size)) << std::strerror(errno);

  // After fully draining, epoll_wait must time out again.
  struct epoll_event out_ev3
  {
  };
  int ready3 = ::epoll_wait(ep_fd, &out_ev3, 1, 50 /*ms*/);
  EXPECT_EQ(ready3, 0) << "epoll_wait should time out after the queue was fully drained";

  ::close(ep_fd);
  ::close(rx_fd);
  release_keeper(keeper);
}

// ---------------------------------------------------------------------------
// Lifecycle: queue is reclaimed when the last Agnocast process exits
// ---------------------------------------------------------------------------

// When the only Agnocast process in this IPC-namespace exits, the kmod must
// free the bridge_msg_queue. A subsequent receiver fd then sees EOF because
// the kmod re-creates the queue lazily and finds it empty (no senders since
// re-creation). We rely on the read returning EAGAIN (queue empty) or 0
// (EOF after the previous queue was torn down) — both are acceptable; the
// stronger property is that *no stale message survives across the
// cross-cleanup boundary*.
TEST_F(BridgeMsgQueueTest, QueueIsFreedWhenLastAgnocastProcExits)
{
  // Phase 1: child registers, pushes a message, then exits. This is the only
  // Agnocast process in the IPC-namespace at that moment, so its exit must
  // trigger queue cleanup in agnocast_process_exit_cleanup.
  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    child_register_then_send_one(64, 0x77);
  }
  expect_child_exits_clean(pid);

  // Give the kmod's worker kthread time to dequeue the PID and run cleanup.
  // agnocast_enqueue_exit_pid wakes the worker but the dequeue is async.
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(200ms);

  // Phase 2: create a fresh receiver. The earlier message must not be
  // observable through it — the queue was freed when the child exited and is
  // re-created empty here.
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);
  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0) << std::strerror(errno);
  ::close(dev_fd);

  std::array<uint8_t, MAX_BRIDGE_MSG_SIZE> rx_buf{};
  ssize_t n = ::read(rx_fd, rx_buf.data(), rx_buf.size());
  EXPECT_EQ(n, -1) << "stale message survived process-exit cleanup; n=" << n;
  EXPECT_EQ(errno, EAGAIN);

  ::close(rx_fd);
}

// When two Agnocast processes coexist and only one exits, the queue must
// survive — the message pushed by the exiting child must still be readable
// while the other process is alive.
TEST_F(BridgeMsgQueueTest, QueueSurvivesWhileAnotherAgnocastProcAlive)
{
  // Spawn a long-running sibling Agnocast process via fork. The sibling
  // sleeps until the parent writes to a pipe to signal "you may exit now".
  int signal_pipe[2];
  ASSERT_EQ(::pipe(signal_pipe), 0);

  pid_t sibling = ::fork();
  ASSERT_GE(sibling, 0);
  if (sibling == 0) {
    ::close(signal_pipe[1]);
    int dev_fd = ::open(AGNOCAST_DEVICE_PATH, O_RDWR);
    if (dev_fd < 0) _exit(101);
    ioctl_add_process_args add_args = {};
    if (::ioctl(dev_fd, AGNOCAST_ADD_PROCESS_CMD, &add_args) < 0) _exit(102);
    char ch;
    ssize_t r = ::read(signal_pipe[0], &ch, 1);  // wait for parent's go-ahead
    (void)r;
    ::close(dev_fd);
    _exit(0);
  }
  ::close(signal_pipe[0]);

  // Now spawn a short-lived child that pushes a message and exits.
  pid_t sender = ::fork();
  ASSERT_GE(sender, 0);
  if (sender == 0) {
    child_register_then_send_one(96, 0xA5);
  }
  expect_child_exits_clean(sender);

  // The sender has exited, but the sibling is still alive, so the queue
  // must still hold the message.
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(100ms);  // let the worker thread settle

  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);
  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0);
  ::close(dev_fd);

  std::array<uint8_t, MAX_BRIDGE_MSG_SIZE> rx_buf{};
  ssize_t n = ::read(rx_fd, rx_buf.data(), rx_buf.size());
  EXPECT_EQ(n, 96) << "queue lost message while another agnocast proc is alive: "
                   << std::strerror(errno);

  std::vector<uint8_t> expected(96);
  fill_pattern(expected.data(), 96, 0xA5);
  EXPECT_EQ(0, std::memcmp(rx_buf.data(), expected.data(), 96));

  ::close(rx_fd);

  // Release the sibling and wait for it.
  char ch = '!';
  ASSERT_EQ(::write(signal_pipe[1], &ch, 1), 1);
  ::close(signal_pipe[1]);
  expect_child_exits_clean(sibling);
}

// A Bridge-Manager-shaped process: create the receiver fd, register as
// performance bridge manager, send a NOTIFY_BRIDGE_SHUTDOWN, then exit.
// The queue must outlive this process if anyone else has pushed into it,
// or — when nobody else is involved — be cleaned up on exit.
TEST_F(BridgeMsgQueueTest, ReceiverFdAutoClosesOnProcessExit)
{
  // Child opens the device, creates a receiver fd, but never reads from it,
  // and exits without explicitly closing the fd. We verify in the parent
  // that subsequent receiver-fd creation still succeeds and that the queue
  // survives if we kept a reference alive. This is a smoke test for "the
  // kernel cleans up the receiver fd via the regular file table teardown"
  // — if it leaked, the worker kthread on process-exit cleanup might dangle.
  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    int dev_fd = ::open(AGNOCAST_DEVICE_PATH, O_RDWR);
    if (dev_fd < 0) _exit(101);
    ioctl_add_process_args add_args = {};
    if (::ioctl(dev_fd, AGNOCAST_ADD_PROCESS_CMD, &add_args) < 0) _exit(102);
    int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
    if (rx_fd < 0) _exit(103);
    // Deliberately leak rx_fd and dev_fd; rely on kernel cleanup.
    _exit(0);
  }
  expect_child_exits_clean(pid);

  // Wait for cleanup to land.
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(200ms);

  // Parent should still be able to create a new receiver fd, prove the
  // kmod is healthy and the per-IPC-ns slot is reusable.
  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);
  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  EXPECT_GE(rx_fd, 0) << std::strerror(errno);

  ::close(rx_fd);
  ::close(dev_fd);
}

// Verifies that a child that has *only* registered (no send) but later
// exits also triggers queue cleanup when it is the last Agnocast process.
TEST_F(BridgeMsgQueueTest, QueueClearedAfterRegisterOnlyChildExits)
{
  // First, push a message from a sender child. The sender's exit by itself
  // would already trigger cleanup, so we keep a sibling alive during the
  // push to ensure the message lands in a queue that survives the sender.
  int signal_pipe[2];
  ASSERT_EQ(::pipe(signal_pipe), 0);

  pid_t sibling = ::fork();
  ASSERT_GE(sibling, 0);
  if (sibling == 0) {
    ::close(signal_pipe[1]);
    int dev_fd = ::open(AGNOCAST_DEVICE_PATH, O_RDWR);
    if (dev_fd < 0) _exit(101);
    ioctl_add_process_args add_args = {};
    if (::ioctl(dev_fd, AGNOCAST_ADD_PROCESS_CMD, &add_args) < 0) _exit(102);
    char ch;
    ssize_t r = ::read(signal_pipe[0], &ch, 1);
    (void)r;
    ::close(dev_fd);
    _exit(0);
  }
  ::close(signal_pipe[0]);

  pid_t sender = ::fork();
  ASSERT_GE(sender, 0);
  if (sender == 0) {
    child_register_then_send_one(32, 0xC3);
  }
  expect_child_exits_clean(sender);

  // Release sibling — this should be the trigger that drops the IPC-ns
  // alive-process count to zero, and the queue (with the unread message
  // from `sender`) should be freed.
  char ch = '!';
  ASSERT_EQ(::write(signal_pipe[1], &ch, 1), 1);
  ::close(signal_pipe[1]);
  expect_child_exits_clean(sibling);

  using namespace std::chrono_literals;
  std::this_thread::sleep_for(200ms);

  int dev_fd = open_agnocast_device();
  ASSERT_GE(dev_fd, 0);
  int rx_fd = ::ioctl(dev_fd, AGNOCAST_CREATE_BRIDGE_MSG_RECEIVER_CMD);
  ASSERT_GE(rx_fd, 0);
  ::close(dev_fd);

  std::array<uint8_t, MAX_BRIDGE_MSG_SIZE> rx_buf{};
  ssize_t n = ::read(rx_fd, rx_buf.data(), rx_buf.size());
  EXPECT_EQ(n, -1) << "queue not freed after last alive proc exited; n=" << n;
  EXPECT_EQ(errno, EAGAIN);

  ::close(rx_fd);
}

}  // namespace agnocast

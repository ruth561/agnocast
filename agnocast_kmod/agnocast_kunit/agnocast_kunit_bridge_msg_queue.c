// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
#include "agnocast_kunit_bridge_msg_queue.h"

#include "../agnocast.h"

#include <kunit/test.h>
#include <linux/string.h>

static const pid_t PID_A = 5000;
static const pid_t PID_B = 5001;

static void register_agnocast_proc(struct kunit * test, pid_t pid)
{
  union ioctl_add_process_args args;
  memset(&args, 0, sizeof(args));
  int ret = agnocast_ioctl_add_process(pid, current->nsproxy->ipc_ns, false, &args);
  KUNIT_ASSERT_EQ(test, ret, 0);
}

void test_case_bridge_msg_queue_send_and_peek_one(struct kunit * test)
{
  // Arrange
  const struct ipc_namespace * ipc_ns = current->nsproxy->ipc_ns;
  register_agnocast_proc(test, PID_A);

  uint8_t payload[16];
  for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 3 + 7);

  // Act
  int ret = agnocast_ioctl_send_msg_to_bridge(ipc_ns, payload, sizeof(payload));

  // Assert
  KUNIT_EXPECT_EQ(test, ret, 0);
  KUNIT_EXPECT_TRUE(test, agnocast_has_bridge_msg_queue(ipc_ns));
  KUNIT_EXPECT_EQ(test, (int)agnocast_get_bridge_msg_queue_len(ipc_ns), 1);

  uint8_t buf[64] = {0};
  uint32_t out_size = 0;
  ret = agnocast_peek_bridge_msg(ipc_ns, 0, buf, sizeof(buf), &out_size);
  KUNIT_EXPECT_EQ(test, ret, 0);
  KUNIT_EXPECT_EQ(test, (int)out_size, (int)sizeof(payload));
  KUNIT_EXPECT_EQ(test, memcmp(buf, payload, sizeof(payload)), 0);
}

void test_case_bridge_msg_queue_send_fifo_order(struct kunit * test)
{
  // Arrange
  const struct ipc_namespace * ipc_ns = current->nsproxy->ipc_ns;
  register_agnocast_proc(test, PID_A);

  uint8_t a[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  uint8_t b[8] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};
  uint8_t c[8] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

  // Act
  KUNIT_ASSERT_EQ(test, agnocast_ioctl_send_msg_to_bridge(ipc_ns, a, sizeof(a)), 0);
  KUNIT_ASSERT_EQ(test, agnocast_ioctl_send_msg_to_bridge(ipc_ns, b, sizeof(b)), 0);
  KUNIT_ASSERT_EQ(test, agnocast_ioctl_send_msg_to_bridge(ipc_ns, c, sizeof(c)), 0);

  // Assert: FIFO order preserved at peek indexes 0,1,2.
  KUNIT_EXPECT_EQ(test, (int)agnocast_get_bridge_msg_queue_len(ipc_ns), 3);

  uint8_t buf[8];
  uint32_t out_size = 0;
  KUNIT_ASSERT_EQ(test, agnocast_peek_bridge_msg(ipc_ns, 0, buf, sizeof(buf), &out_size), 0);
  KUNIT_EXPECT_EQ(test, memcmp(buf, a, sizeof(a)), 0);
  KUNIT_ASSERT_EQ(test, agnocast_peek_bridge_msg(ipc_ns, 1, buf, sizeof(buf), &out_size), 0);
  KUNIT_EXPECT_EQ(test, memcmp(buf, b, sizeof(b)), 0);
  KUNIT_ASSERT_EQ(test, agnocast_peek_bridge_msg(ipc_ns, 2, buf, sizeof(buf), &out_size), 0);
  KUNIT_EXPECT_EQ(test, memcmp(buf, c, sizeof(c)), 0);
}

void test_case_bridge_msg_queue_send_invalid_size(struct kunit * test)
{
  // Arrange
  const struct ipc_namespace * ipc_ns = current->nsproxy->ipc_ns;
  register_agnocast_proc(test, PID_A);

  uint8_t dummy[4] = {1, 2, 3, 4};

  // Act + Assert: size == 0 is invalid.
  KUNIT_EXPECT_EQ(test, agnocast_ioctl_send_msg_to_bridge(ipc_ns, dummy, 0), -EINVAL);

  // size > MAX_BRIDGE_MSG_SIZE is invalid.
  KUNIT_EXPECT_EQ(
    test, agnocast_ioctl_send_msg_to_bridge(ipc_ns, dummy, MAX_BRIDGE_MSG_SIZE + 1), -EINVAL);

  // Queue should not have been created by failed sends.
  KUNIT_EXPECT_FALSE(test, agnocast_has_bridge_msg_queue(ipc_ns));
}

void test_case_bridge_msg_queue_send_full_returns_enospc(struct kunit * test)
{
  // Arrange
  const struct ipc_namespace * ipc_ns = current->nsproxy->ipc_ns;
  register_agnocast_proc(test, PID_A);

  uint8_t payload[8] = {0};

  // Fill the queue to capacity.
  for (uint32_t i = 0; i < MAX_BRIDGE_MSG_QUEUE_LEN; ++i) {
    int ret = agnocast_ioctl_send_msg_to_bridge(ipc_ns, payload, sizeof(payload));
    KUNIT_ASSERT_EQ(test, ret, 0);
  }
  KUNIT_ASSERT_EQ(
    test, (int)agnocast_get_bridge_msg_queue_len(ipc_ns), (int)MAX_BRIDGE_MSG_QUEUE_LEN);

  // Act: one more push should fail with -ENOSPC.
  int ret = agnocast_ioctl_send_msg_to_bridge(ipc_ns, payload, sizeof(payload));

  // Assert
  KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
  KUNIT_EXPECT_EQ(
    test, (int)agnocast_get_bridge_msg_queue_len(ipc_ns), (int)MAX_BRIDGE_MSG_QUEUE_LEN);
}

void test_case_bridge_msg_queue_freed_when_last_proc_exits(struct kunit * test)
{
  // Arrange: single process pushes a message, then exits.
  const struct ipc_namespace * ipc_ns = current->nsproxy->ipc_ns;
  register_agnocast_proc(test, PID_A);

  uint8_t payload[16] = {0};
  KUNIT_ASSERT_EQ(test, agnocast_ioctl_send_msg_to_bridge(ipc_ns, payload, sizeof(payload)), 0);
  KUNIT_ASSERT_TRUE(test, agnocast_has_bridge_msg_queue(ipc_ns));

  // Act
  agnocast_process_exit_cleanup(PID_A);

  // Assert: queue is reclaimed because no other Agnocast process is alive.
  KUNIT_EXPECT_FALSE(test, agnocast_has_bridge_msg_queue(ipc_ns));
}

void test_case_bridge_msg_queue_survives_while_other_proc_alive(struct kunit * test)
{
  // Arrange: two processes; one of them sends, then exits.
  const struct ipc_namespace * ipc_ns = current->nsproxy->ipc_ns;
  register_agnocast_proc(test, PID_A);
  register_agnocast_proc(test, PID_B);

  uint8_t payload[16] = {0xDE, 0xAD, 0xBE, 0xEF};
  KUNIT_ASSERT_EQ(test, agnocast_ioctl_send_msg_to_bridge(ipc_ns, payload, sizeof(payload)), 0);
  KUNIT_ASSERT_TRUE(test, agnocast_has_bridge_msg_queue(ipc_ns));
  KUNIT_ASSERT_EQ(test, (int)agnocast_get_bridge_msg_queue_len(ipc_ns), 1);

  // Act: only PID_A exits; PID_B remains.
  agnocast_process_exit_cleanup(PID_A);

  // Assert: queue still exists with its entry.
  KUNIT_EXPECT_TRUE(test, agnocast_has_bridge_msg_queue(ipc_ns));
  KUNIT_EXPECT_EQ(test, (int)agnocast_get_bridge_msg_queue_len(ipc_ns), 1);

  // Now PID_B exits and the queue should be reclaimed.
  agnocast_process_exit_cleanup(PID_B);
  KUNIT_EXPECT_FALSE(test, agnocast_has_bridge_msg_queue(ipc_ns));
}

void test_case_bridge_msg_queue_variable_size_messages(struct kunit * test)
{
  // Arrange
  const struct ipc_namespace * ipc_ns = current->nsproxy->ipc_ns;
  register_agnocast_proc(test, PID_A);

  uint8_t small[1] = {0x42};
  uint8_t medium[256];
  for (size_t i = 0; i < sizeof(medium); ++i) medium[i] = (uint8_t)(i & 0xFF);
  uint8_t big[MAX_BRIDGE_MSG_SIZE];
  for (size_t i = 0; i < sizeof(big); ++i) big[i] = (uint8_t)((i * 31) & 0xFF);

  // Act
  KUNIT_ASSERT_EQ(test, agnocast_ioctl_send_msg_to_bridge(ipc_ns, small, sizeof(small)), 0);
  KUNIT_ASSERT_EQ(test, agnocast_ioctl_send_msg_to_bridge(ipc_ns, medium, sizeof(medium)), 0);
  KUNIT_ASSERT_EQ(test, agnocast_ioctl_send_msg_to_bridge(ipc_ns, big, sizeof(big)), 0);

  // Assert: each peek returns the exact size and bytes that were pushed.
  uint8_t buf[MAX_BRIDGE_MSG_SIZE];
  uint32_t out_size = 0;

  KUNIT_ASSERT_EQ(test, agnocast_peek_bridge_msg(ipc_ns, 0, buf, sizeof(buf), &out_size), 0);
  KUNIT_EXPECT_EQ(test, (int)out_size, (int)sizeof(small));
  KUNIT_EXPECT_EQ(test, memcmp(buf, small, sizeof(small)), 0);

  KUNIT_ASSERT_EQ(test, agnocast_peek_bridge_msg(ipc_ns, 1, buf, sizeof(buf), &out_size), 0);
  KUNIT_EXPECT_EQ(test, (int)out_size, (int)sizeof(medium));
  KUNIT_EXPECT_EQ(test, memcmp(buf, medium, sizeof(medium)), 0);

  KUNIT_ASSERT_EQ(test, agnocast_peek_bridge_msg(ipc_ns, 2, buf, sizeof(buf), &out_size), 0);
  KUNIT_EXPECT_EQ(test, (int)out_size, (int)sizeof(big));
  KUNIT_EXPECT_EQ(test, memcmp(buf, big, sizeof(big)), 0);
}

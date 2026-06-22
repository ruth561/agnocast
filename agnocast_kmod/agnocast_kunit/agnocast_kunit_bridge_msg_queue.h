/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#pragma once
#include <kunit/test.h>

#define TEST_CASES_BRIDGE_MSG_QUEUE                                         \
  KUNIT_CASE(test_case_bridge_msg_queue_send_and_peek_one),                 \
    KUNIT_CASE(test_case_bridge_msg_queue_send_fifo_order),                 \
    KUNIT_CASE(test_case_bridge_msg_queue_send_invalid_size),               \
    KUNIT_CASE(test_case_bridge_msg_queue_send_full_returns_enospc),        \
    KUNIT_CASE(test_case_bridge_msg_queue_freed_when_last_proc_exits),      \
    KUNIT_CASE(test_case_bridge_msg_queue_survives_while_other_proc_alive), \
    KUNIT_CASE(test_case_bridge_msg_queue_variable_size_messages)

void test_case_bridge_msg_queue_send_and_peek_one(struct kunit * test);
void test_case_bridge_msg_queue_send_fifo_order(struct kunit * test);
void test_case_bridge_msg_queue_send_invalid_size(struct kunit * test);
void test_case_bridge_msg_queue_send_full_returns_enospc(struct kunit * test);
void test_case_bridge_msg_queue_freed_when_last_proc_exits(struct kunit * test);
void test_case_bridge_msg_queue_survives_while_other_proc_alive(struct kunit * test);
void test_case_bridge_msg_queue_variable_size_messages(struct kunit * test);

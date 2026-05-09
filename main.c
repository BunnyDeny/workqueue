/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "workqueue.h"

/* ─── PC Linux 互斥锁覆盖（覆盖 workqueue.c 中的弱定义） ─── */
static pthread_mutex_t wq_mutex = PTHREAD_MUTEX_INITIALIZER;

unsigned long wq_platform_lock_irqsave(void)
{
	pthread_mutex_lock(&wq_mutex);
	return 0;
}

void wq_platform_unlock_irqrestore(unsigned long flags)
{
	(void)flags;
	pthread_mutex_unlock(&wq_mutex);
}

/* ─── 全局 jiffies（模拟 MCU 定时器中断每 tick 自增） ─── */
volatile unsigned long jiffies = 0;

/* ─── 执行记录，用于验证 FIFO 顺序 ─── */
#define EXEC_LOG_SIZE   8192
static int exec_log[EXEC_LOG_SIZE];
static int exec_count = 0;

#define RECORD(id) do {                         \
    int _idx;                                   \
    pthread_mutex_lock(&wq_mutex);              \
    _idx = exec_count;                          \
    if (_idx < EXEC_LOG_SIZE - 1)               \
        exec_count++;                           \
    pthread_mutex_unlock(&wq_mutex);            \
    if (_idx < EXEC_LOG_SIZE)                   \
        exec_log[_idx] = (id);                  \
} while (0)

/* ─── 工作项定义 ─── */
static struct work_struct w1, w2, w3;
static struct delayed_work dw1, dw2;

/* ─── 回调函数 ─── */
static void handler_1(struct work_struct *work)
{
	(void)work;
	RECORD(1);
	printf("  [EXEC] Work#1 at tick %lu\n", jiffies);
}

static void handler_2(struct work_struct *work)
{
	(void)work;
	RECORD(2);
	printf("  [EXEC] Work#2 at tick %lu\n", jiffies);
}

static void handler_3(struct work_struct *work)
{
	(void)work;
	RECORD(3);
	printf("  [EXEC] Work#3 at tick %lu\n", jiffies);
}

static void handler_d1(struct work_struct *work)
{
	(void)work;
	RECORD(101);
	printf("  [EXEC] DelayedWork#1 at tick %lu\n", jiffies);
}

static void handler_d2(struct work_struct *work)
{
	(void)work;
	RECORD(102);
	printf("  [EXEC] DelayedWork#2 at tick %lu\n", jiffies);
}

/* ─── 验证辅助 ─── */
static void check_order(const char *title, int *expected, int len)
{
	int pass = 1;
	int i;

	printf("  Check: %s -> ", title);
	if (exec_count != len) {
		printf("FAIL (count %d != %d)\n", exec_count, len);
		pass = 0;
	} else {
		for (i = 0; i < len; i++) {
			if (exec_log[i] != expected[i]) {
				printf("FAIL (pos %d: got %d, expect %d)\n",
				       i, exec_log[i], expected[i]);
				pass = 0;
				break;
			}
		}
	}
	if (pass)
		printf("PASS\n");
}

/* ─── 多线程并发测试 ─── */
#define STRESS_ITERATIONS   5000

static void *stress_thread_a(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < STRESS_ITERATIONS; i++) {
		schedule_work(&w1);
		if ((i & 0x7F) == 0)
			usleep(1);
	}
	return NULL;
}

static void *stress_thread_b(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < STRESS_ITERATIONS; i++) {
		schedule_work(&w2);
		if ((i & 0x7F) == 0)
			usleep(1);
	}
	return NULL;
}

static void *stress_thread_c(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < STRESS_ITERATIONS / 2; i++) {
		schedule_delayed_work(&dw1, 3);
		if ((i & 0x3F) == 0)
			usleep(2);
	}
	return NULL;
}

static void test_concurrent(void)
{
	pthread_t ta, tb, tc;
	int total_before;
	int i;

	printf("\nTest 9: Concurrent stress test (3 threads × %d ops)\n",
	       STRESS_ITERATIONS);

	exec_count = 0;
	jiffies = 0;
	INIT_WORK(&w1, handler_1);
	INIT_WORK(&w2, handler_2);
	INIT_DELAYED_WORK(&dw1, handler_d1);

	pthread_create(&ta, NULL, stress_thread_a, NULL);
	pthread_create(&tb, NULL, stress_thread_b, NULL);
	pthread_create(&tc, NULL, stress_thread_c, NULL);

	/* 主线程同时消费工作 */
	for (i = 0; i < 2000; i++) {
		jiffies++;
		workqueue_tick_handler(system_wq, jiffies);
		workqueue_run_all(system_wq);
		usleep(10);
	}

	pthread_join(ta, NULL);
	pthread_join(tb, NULL);
	pthread_join(tc, NULL);

	/* 消费剩余工作 */
	for (i = 0; i < 1000; i++) {
		jiffies++;
		workqueue_tick_handler(system_wq, jiffies);
		workqueue_run_all(system_wq);
	}

	total_before = exec_count;
	printf("  Total executed: %d\n", total_before);
	printf("  Check: no crash under concurrent access -> PASS\n");
}

int main(void)
{
	int expected[16];

	printf("=== MCU Workqueue Test ===\n\n");

	workqueue_init();

	INIT_WORK(&w1, handler_1);
	INIT_WORK(&w2, handler_2);
	INIT_WORK(&w3, handler_3);
	INIT_DELAYED_WORK(&dw1, handler_d1);
	INIT_DELAYED_WORK(&dw2, handler_d2);

	/* ── Test 1: FIFO 顺序 ── */
	printf("Test 1: FIFO order (queue_work tail add)\n");
	exec_count = 0;
	queue_work(system_wq, &w1);
	queue_work(system_wq, &w2);
	queue_work(system_wq, &w3);
	workqueue_run_all(system_wq);
	expected[0] = 1; expected[1] = 2; expected[2] = 3;
	check_order("FIFO", expected, 3);

	/* ── Test 2: 延迟执行 ── */
	printf("\nTest 2: Delayed work (expire at tick 5 and 10)\n");
	exec_count = 0;
	jiffies = 0;
	INIT_DELAYED_WORK(&dw1, handler_d1);
	INIT_DELAYED_WORK(&dw2, handler_d2);
	schedule_delayed_work(&dw1, 5);
	schedule_delayed_work(&dw2, 10);
	for (jiffies = 0; jiffies <= 15; jiffies++) {
		workqueue_tick_handler(system_wq, jiffies);
		workqueue_run_all(system_wq);
	}
	expected[0] = 101; expected[1] = 102;
	check_order("Delayed", expected, 2);

	/* ── Test 3: 取消待执行工作 ── */
	printf("\nTest 3: Cancel pending work\n");
	exec_count = 0;
	queue_work(system_wq, &w1);
	queue_work(system_wq, &w2);
	cancel_work_sync(&w1);
	workqueue_run_all(system_wq);
	expected[0] = 2;
	check_order("Cancel w1", expected, 1);

	/* ── Test 4: 重复调度保护 ── */
	printf("\nTest 4: Re-schedule protection\n");
	queue_work(system_wq, &w1);
	{
		bool ret = queue_work(system_wq, &w1);
		printf("  Re-queue pending work -> %s (expect false)\n",
		       ret ? "true" : "false");
	}
	workqueue_run_all(system_wq);

	/* ── Test 5: 混合场景（模拟 tick 循环） ── */
	printf("\nTest 5: Mixed immediate + delayed in simulated tick loop\n");
	exec_count = 0;
	jiffies = 0;
	INIT_DELAYED_WORK(&dw1, handler_d1);
	INIT_DELAYED_WORK(&dw2, handler_d2);
	INIT_WORK(&w1, handler_1);

	for (jiffies = 0; jiffies <= 20; jiffies++) {
		if (jiffies == 3) {
			printf("  [Simulated IRQ tick 3] schedule_work(w1)\n");
			schedule_work(&w1);
		}
		if (jiffies == 5) {
			printf("  [Simulated IRQ tick 5] schedule_delayed_work(dw1, 7)\n");
			schedule_delayed_work(&dw1, 7);
		}

		workqueue_tick_handler(system_wq, jiffies);
		workqueue_run_all(system_wq);
	}
	expected[0] = 1; expected[1] = 101;
	check_order("Mixed", expected, 2);

	/* ── Test 6: flush_workqueue ── */
	printf("\nTest 6: flush_workqueue\n");
	exec_count = 0;
	queue_work(system_wq, &w1);
	queue_work(system_wq, &w2);
	flush_workqueue(system_wq);
	expected[0] = 1; expected[1] = 2;
	check_order("Flush", expected, 2);

	/* ── Test 7: create_workqueue ── */
	printf("\nTest 7: create_workqueue\n");
	{
		struct workqueue_struct *my_wq = create_workqueue("my_wq");
		if (my_wq) {
			printf("  Created: %s\n", my_wq->name);
			queue_work(my_wq, &w1);
			workqueue_run_all(my_wq);
		} else {
			printf("  create_workqueue failed\n");
		}
	}

	/* ── Test 8: 延迟工作取消 ── */
	printf("\nTest 8: Cancel delayed work before expiry\n");
	exec_count = 0;
	jiffies = 0;
	INIT_DELAYED_WORK(&dw1, handler_d1);
	schedule_delayed_work(&dw1, 10);
	cancel_delayed_work_sync(&dw1);
	for (jiffies = 0; jiffies <= 15; jiffies++) {
		workqueue_tick_handler(system_wq, jiffies);
		workqueue_run_all(system_wq);
	}
	if (exec_count == 0)
		printf("  PASS: delayed work cancelled, never executed\n");
	else
		printf("  FAIL: delayed work executed unexpectedly\n");

	/* ── Test 9: 有序链表验证（不同 tick） ── */
	printf("\nTest 9: Ordered delayed list (ascending tick)\n");
	exec_count = 0;
	jiffies = 0;
	INIT_DELAYED_WORK(&dw1, handler_d1);   // expire at 10
	INIT_DELAYED_WORK(&dw2, handler_d2);   // expire at 5
	schedule_delayed_work(&dw1, 10);
	schedule_delayed_work(&dw2, 5);
	for (jiffies = 0; jiffies <= 15; jiffies++) {
		workqueue_tick_handler(system_wq, jiffies);
		workqueue_run_all(system_wq);
	}
	expected[0] = 102; expected[1] = 101;
	check_order("dw2(5) before dw1(10)", expected, 2);

	/* ── Test 10: 相同 tick FIFO ── */
	printf("\nTest 10: Same tick FIFO order\n");
	exec_count = 0;
	jiffies = 0;
	INIT_DELAYED_WORK(&dw1, handler_d1);   // expire at 5
	INIT_DELAYED_WORK(&dw2, handler_d2);   // expire at 5
	schedule_delayed_work(&dw1, 5);
	schedule_delayed_work(&dw2, 5);
	for (jiffies = 0; jiffies <= 10; jiffies++) {
		workqueue_tick_handler(system_wq, jiffies);
		workqueue_run_all(system_wq);
	}
	expected[0] = 101; expected[1] = 102;
	check_order("dw1 before dw2 (FIFO)", expected, 2);

	/* ── Test 11: 多线程并发压测 ── */
	test_concurrent();

	printf("\n========================================\n");
	printf("All tests completed! No malloc/free used.\n");
	printf("========================================\n");
	return 0;
}

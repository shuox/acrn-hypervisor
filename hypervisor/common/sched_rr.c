/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <rtl.h>
#include <list.h>
#include <bits.h>
#include <cpu.h>
#include <per_cpu.h>
#include <lapic.h>
#include <schedule.h>
#include <sprintf.h>
#include <errno.h>
#include <trace.h>

#define CONFIG_TASK_SLICE_MS 5UL

struct sched_rr_data {
	uint64_t slice_cycles;
	uint64_t last_cycles;
	int64_t  left_cycles;
};

void sched_rr_init_data(struct sched_object *obj)
{
	struct sched_rr_data *data;

	ASSERT(sizeof(struct sched_rr_data) < sizeof(obj->data), "sched_rr data size too large!");
	data = (struct sched_rr_data *)obj->data;
	data->left_cycles = data->slice_cycles = CONFIG_TASK_SLICE_MS * CYCLES_PER_MS;
}

static void sched_tick_handler(void *param)
{
	struct sched_context *ctx = (struct sched_context *)param;
	struct sched_rr_data *data;
	struct sched_object *current, *obj;
	struct list_head *pos, *n;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t now = rdtsc();

	get_schedule_lock(pcpu_id);
	current = ctx->current;

	/* replenish retired sched_objects with slice_cycles, then move them to runqueue */
	spinlock_obtain(&ctx->queue_lock);
	list_for_each_safe(pos, n, &ctx->retired_queue) {
		obj = list_entry(pos, struct sched_object, list);
		data = (struct sched_rr_data *)obj->data;
		data->left_cycles = data->slice_cycles;
		list_del_init(&obj->list);
		list_add_tail(&obj->list, &ctx->runqueue);
		sched_set_status(obj, SCHED_STS_RUNNABLE);
	}
	spinlock_release(&ctx->queue_lock);

	/* If no vCPU start scheduling, ignore this tick */
	if (current == NULL || (sched_is_idle(current) && list_empty(&ctx->runqueue))) {
		release_schedule_lock(pcpu_id);
		return;
	}
	data = (struct sched_rr_data *)current->data;
	/* consume the left_cycles of current sched_object if it is not idle */
	if (!sched_is_idle(current)) {
		data->left_cycles -= now - data->last_cycles;
		data->last_cycles = now;
	}
	/* make reschedule request if current ran out of its cycles */
	if (sched_is_idle(current) || data->left_cycles <= 0) {
		make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	}
	release_schedule_lock(pcpu_id);
}

int sched_rr_init(struct sched_context *ctx)
{
	uint64_t tick_period = CONFIG_TASK_SLICE_MS * CYCLES_PER_MS / 2;
	int ret = 0;

	/* The tick_timer is per-scheduler, periodically */
	initialize_timer(&ctx->tick_timer, sched_tick_handler, ctx,
			rdtsc() + tick_period, TICK_MODE_PERIODIC, tick_period);
	if (add_timer(&ctx->tick_timer) < 0) {
		pr_err("Failed to add schedule tick timer!");
		ret = -1;
	}

	return ret;
}

static struct sched_object *sched_rr_pick_next(struct sched_context *ctx)
{
	struct sched_object *next = NULL;
	struct sched_object *current = NULL;
	struct sched_rr_data *data;
	uint64_t now = rdtsc();

	current = ctx->current;
	data = (struct sched_rr_data *)current->data;
	/* Ignore the idle object, inactive objects */
	if (!sched_is_idle(current) && sched_is_active(current)) {
		data->left_cycles -= now - data->last_cycles;
		sched_queue_remove(current);
		if (data->left_cycles > 0) {
			sched_runqueue_add_tail(current);
		} else {
			sched_retired_queue_add(current);
		}
	}

	/* Pick the next runnable sched object
	 * 1) get the first item in runqueue firstly
	 * 2) if no runnable object picked, replenish first object in retired queue if we have and pick this one
	 * 3) At least take one idle sched object if we have no runnable ones after step 1) and 2)
	 */
	spinlock_obtain(&ctx->queue_lock);
	if (!list_empty(&ctx->runqueue)) {
		next = get_first_item(&ctx->runqueue, struct sched_object, list);
		data = (struct sched_rr_data *)next->data;
		data->last_cycles = now;
	} else if (!list_empty(&ctx->retired_queue)) {
		next = get_first_item(&ctx->retired_queue, struct sched_object, list);
		data = (struct sched_rr_data *)next->data;
		data->left_cycles = data->slice_cycles;
		data->last_cycles = now;
		list_del_init(&next->list);
		list_add_tail(&next->list, &ctx->runqueue);
		sched_set_status(next, SCHED_STS_RUNNABLE);
	} else {
		next = &get_cpu_var(idle);
	}
	spinlock_release(&ctx->queue_lock);

	return next;
}

struct acrn_scheduler sched_rr = {
	.name		= "sched_rr",
	.init		= sched_rr_init,
	.init_data	= sched_rr_init_data,
	.pick_next	= sched_rr_pick_next,
};

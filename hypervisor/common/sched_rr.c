/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <list.h>
#include <per_cpu.h>
#include <schedule.h>

#define CONFIG_SLICE_MS 10UL
struct sched_rr_data {
	/* keep list as the first item */
	struct list_head list;

	uint64_t slice_cycles;
	uint64_t last_cycles;
	int64_t  left_cycles;
};

bool is_active(struct sched_object *obj)
{
	struct sched_rr_data *data = (struct sched_rr_data *)obj->data;
	return !list_empty(&data->list);
}

void runqueue_add_head(struct sched_object *obj)
{
	struct sched_rr_context *rr_ctx = (struct sched_rr_context *)obj->ctx->priv;
	struct sched_rr_data *data = (struct sched_rr_data *)obj->data;

	if (!is_active(obj)) {
		list_add(&data->list, &rr_ctx->runqueue);
	}
}

void runqueue_add_tail(struct sched_object *obj)
{
	struct sched_rr_context *rr_ctx = (struct sched_rr_context *)obj->ctx->priv;
	struct sched_rr_data *data = (struct sched_rr_data *)obj->data;
	struct list_head *pos;
	struct sched_object *iter;
	struct sched_rr_data *iter_data;

	if (!is_active(obj)) {
		if (data->left_cycles <= 0) {
			list_add_tail(&data->list, &rr_ctx->runqueue);
		} else {
			list_for_each(pos, &rr_ctx->runqueue) {
				iter = list_entry(pos, struct sched_object, data);
				iter_data = (struct sched_rr_data *)iter->data;
				if (iter_data->left_cycles <= 0) {
					break;
				}
			}
			list_add_node(&data->list, pos->prev, pos);
		}
	}
}

void queue_remove(struct sched_object *obj)
{
	struct sched_rr_data *data = (struct sched_rr_data *)obj->data;
	/* treat no queued object as paused, should wake it up when events arrive */
	list_del_init(&data->list);
}

static void sched_tick_handler(void *param)
{
	struct sched_context *ctx = (struct sched_context *)param;
	struct sched_rr_context *rr_ctx = (struct sched_rr_context *)ctx->priv;
	struct sched_rr_data *data;
	struct sched_object *current, *obj;
	struct list_head *pos;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t now = rdtsc();

	get_schedule_lock(pcpu_id);
	current = ctx->current;

	/* replenish sched_objects with slice_cycles, then reorder the runqueue according left_cycles */
	list_for_each(pos, &rr_ctx->runqueue) {
		obj = list_entry(pos, struct sched_object, data);
		data = (struct sched_rr_data *)obj->data;
		if (data->left_cycles > 0) {
			continue;
		}
		data->left_cycles += data->slice_cycles;
		queue_remove(obj);
		runqueue_add_tail(obj);
	}

	/* If no vCPU start scheduling, ignore this tick */
	if (current == NULL || (sched_is_idle(current) && list_empty(&rr_ctx->runqueue))) {
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
	struct sched_rr_context *rr_ctx = &per_cpu(sched_rr_ctx, ctx->pcpu_id);
	uint64_t tick_period = CONFIG_SLICE_MS * CYCLES_PER_MS / 2;
	int ret = 0;

	ctx->priv = rr_ctx;
	INIT_LIST_HEAD(&rr_ctx->runqueue);

	/* The tick_timer is periodically */
	initialize_timer(&rr_ctx->tick_timer, sched_tick_handler, ctx,
			rdtsc() + tick_period, TICK_MODE_PERIODIC, tick_period);
	if (add_timer(&rr_ctx->tick_timer) < 0) {
		pr_err("Failed to add schedule tick timer!");
		ret = -1;
	}

	return ret;
}

void sched_rr_deinit(struct sched_context *ctx)
{
	struct sched_rr_context *rr_ctx = (struct sched_rr_context *)ctx->priv;
	del_timer(&rr_ctx->tick_timer);
}

void sched_rr_init_data(struct sched_object *obj)
{
	struct sched_rr_data *data;

	ASSERT(sizeof(struct sched_rr_data) < sizeof(obj->data), "sched_rr data size too large!");
	data = (struct sched_rr_data *)obj->data;
	INIT_LIST_HEAD(&data->list);
	data->left_cycles = data->slice_cycles = CONFIG_SLICE_MS * CYCLES_PER_MS;
}

void sched_rr_insert(struct sched_object *obj)
{
	runqueue_add_tail(obj);
}

void sched_rr_remove(struct sched_object *obj)
{
	queue_remove(obj);
}

static struct sched_object *sched_rr_pick_next(struct sched_context *ctx)
{
	struct sched_rr_context *rr_ctx = (struct sched_rr_context *)ctx->priv;
	struct sched_object *next = NULL;
	struct sched_object *current = NULL;
	struct sched_rr_data *data;
	uint64_t now = rdtsc();

	current = ctx->current;
	data = (struct sched_rr_data *)current->data;
	/* Ignore the idle object, inactive objects */
	if (!sched_is_idle(current) && is_active(current)) {
		data->left_cycles -= now - data->last_cycles;
		queue_remove(current);
		runqueue_add_tail(current);
	}


	/* Pick the next runnable sched object
	 * 1) get the first item in runqueue firstly
	 * 2) if object picked has no time_cycles, replenish it pick this one
	 * 3) At least take one idle sched object if we have no runnable one after step 1) and 2)
	 */
	if (!list_empty(&rr_ctx->runqueue)) {
		next = get_first_item(&rr_ctx->runqueue, struct sched_object, data);
		data = (struct sched_rr_data *)next->data;
		data->last_cycles = now;
		while (data->left_cycles <= 0) {
			data->left_cycles += data->slice_cycles;
		}
	} else {
		next = &get_cpu_var(idle);
	}

	return next;
}

static void sched_rr_yield(__unused struct sched_context *ctx)
{
	/* Do nothing in sched_rr, just let current switch out */
}

static void sched_rr_sleep(struct sched_object *obj)
{
	queue_remove(obj);
}

static void sched_rr_wake(struct sched_object *obj)
{
	runqueue_add_head(obj);
}

static void sched_rr_poke(struct sched_object *obj)
{
	struct sched_rr_data *data = (struct sched_rr_data *)obj->data;
	if (data->left_cycles > 0) {
		queue_remove(obj);
		runqueue_add_head(obj);
	}
}

struct acrn_scheduler sched_rr = {
	.name		= "sched_rr",
	.init		= sched_rr_init,
	.init_data	= sched_rr_init_data,
	.insert		= sched_rr_insert,
	.pick_next	= sched_rr_pick_next,
	.yield		= sched_rr_yield,
	.sleep		= sched_rr_sleep,
	.wake		= sched_rr_wake,
	.poke		= sched_rr_poke,
	.remove		= sched_rr_remove,
	.deinit		= sched_rr_deinit,
};

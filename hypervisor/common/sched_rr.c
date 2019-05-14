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

	if (!is_active(obj)) {
		list_add_tail(&data->list, &rr_ctx->runqueue);
	}
}

void retired_queue_add(struct sched_object *obj)
{
	struct sched_rr_context *rr_ctx = (struct sched_rr_context *)obj->ctx->priv;
	struct sched_rr_data *data = (struct sched_rr_data *)obj->data;

	if (!is_active(obj)) {
		list_add(&data->list, &rr_ctx->retired_queue);
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
	struct list_head *pos, *n;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t now = rdtsc();

	get_schedule_lock(pcpu_id);
	rr_ctx->stats.tick_count++;
	current = ctx->current;

	/* replenish retired sched_objects with slice_cycles, then move them to runqueue */
	list_for_each_safe(pos, n, &rr_ctx->retired_queue) {
		obj = list_entry(pos, struct sched_object, data);
		data = (struct sched_rr_data *)obj->data;
		data->left_cycles = data->slice_cycles;
		list_del_init(&data->list);
		list_add_tail(&data->list, &rr_ctx->runqueue);
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
	INIT_LIST_HEAD(&rr_ctx->retired_queue);
	rr_ctx->stats.start_time = rdtsc();

	/* The tick_timer is periodically */
	initialize_timer(&rr_ctx->tick_timer, sched_tick_handler, ctx,
			rdtsc() + tick_period, TICK_MODE_PERIODIC, tick_period);
	if (add_timer(&rr_ctx->tick_timer) < 0) {
		pr_err("Failed to add schedule tick timer!");
		ret = -1;
	}

	return ret;
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
		if (data->left_cycles > 0) {
			runqueue_add_tail(current);
		} else {
			retired_queue_add(current);
		}
	}


	/* Pick the next runnable sched object
	 * 1) get the first item in runqueue firstly
	 * 2) if no runnable object picked, replenish first object in retired queue if we have and pick this one
	 * 3) At least take one idle sched object if we have no runnable ones after step 1) and 2)
	 */
	if (!list_empty(&rr_ctx->runqueue)) {
		next = get_first_item(&rr_ctx->runqueue, struct sched_object, data);
		data = (struct sched_rr_data *)next->data;
		data->last_cycles = now;
	} else if (!list_empty(&rr_ctx->retired_queue)) {
		next = get_first_item(&rr_ctx->retired_queue, struct sched_object, data);
		data = (struct sched_rr_data *)next->data;
		data->left_cycles = data->slice_cycles;
		data->last_cycles = now;
		list_del_init(&data->list);
		list_add_tail(&data->list, &rr_ctx->runqueue);
	} else {
		next = &get_cpu_var(idle);
	}

	if (current->stats.last == 0UL) {
		current->stats.last = rr_ctx->stats.start_time;
	}
	current->stats.total_runtime += now - current->stats.last;
	next->stats.last = now;
	next->stats.sched_count++;

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

static void dump_sched_obj(struct sched_object *obj)
{
	struct sched_rr_data *data = (struct sched_rr_data *)obj->data;
	pr_acrnlog("%12s%5d%20lld%15lld%15lld", obj->name, obj->status,
			ticks_to_us(obj->stats.total_runtime),
			ticks_to_us(data->left_cycles),
			obj->stats.sched_count);
}

static void sched_rr_dump(struct sched_context *ctx)
{
	struct sched_rr_context *rr_ctx = (struct sched_rr_context *)ctx->priv;
	struct sched_object *obj;
	struct list_head *pos;

	pr_acrnlog("scheduler: sched_rr runtime: %lld(us)  current: %s  tick: %lld",
			ticks_to_us(rdtsc() - rr_ctx->stats.start_time),
			ctx->current->name, rr_ctx->stats.tick_count);
	pr_acrnlog("%12s%10s%15s(us)%10s(us)%15s", "object", "status", "total_runtime", "slice", "sched_count");
	get_schedule_lock(ctx->pcpu_id);
	list_for_each(pos, &rr_ctx->runqueue) {
		obj = list_entry(pos, struct sched_object, data);
		dump_sched_obj(obj);
	}
	list_for_each(pos, &rr_ctx->retired_queue) {
		obj = list_entry(pos, struct sched_object, data);
		dump_sched_obj(obj);
	}
	release_schedule_lock(ctx->pcpu_id);
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
	.dump		= sched_rr_dump,
};

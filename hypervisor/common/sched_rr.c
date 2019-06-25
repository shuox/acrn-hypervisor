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

static void sched_tick_handler(__unused void *param)
{
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

void sched_rr_insert(__unused struct sched_object *obj)
{
}

void sched_rr_remove(__unused struct sched_object *obj)
{
}

static struct sched_object *sched_rr_pick_next(__unused struct sched_context *ctx)
{
	return NULL;
}

static void sched_rr_yield(__unused struct sched_context *ctx)
{
	/* Do nothing in sched_rr, just let current switch out */
}

static void sched_rr_sleep(__unused struct sched_object *obj)
{
}

static void sched_rr_wake(__unused struct sched_object *obj)
{
}


static void sched_rr_poke(__unused struct sched_object *obj)
{
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

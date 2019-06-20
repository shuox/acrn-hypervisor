/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <list.h>
#include <per_cpu.h>
#include <schedule.h>

struct sched_rr_data {
	/* keep list as the first item */
	struct list_head list;

	uint64_t slice_cycles;
	uint64_t last_cycles;
	int64_t  left_cycles;
};

int sched_rr_init(__unused struct sched_context *ctx)
{
	return 0;
}

void sched_rr_deinit(__unused struct sched_context *ctx)
{
}

void sched_rr_init_data(__unused struct sched_object *obj)
{
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

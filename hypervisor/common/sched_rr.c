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

int sched_rr_init(__unused struct sched_control *ctl)
{
	return 0;
}

void sched_rr_deinit(__unused struct sched_control *ctl)
{
}

void sched_rr_init_data(__unused struct thread_object *obj)
{
}

static struct thread_object *sched_rr_pick_next(__unused struct sched_control *ctl)
{
	return NULL;
}

static void sched_rr_sleep(__unused struct thread_object *obj)
{
}

static void sched_rr_wake(__unused struct thread_object *obj)
{
}

struct acrn_scheduler sched_rr = {
	.name		= "sched_rr",
	.init		= sched_rr_init,
	.init_data	= sched_rr_init_data,
	.pick_next	= sched_rr_pick_next,
	.sleep		= sched_rr_sleep,
	.wake		= sched_rr_wake,
	.deinit		= sched_rr_deinit,
};

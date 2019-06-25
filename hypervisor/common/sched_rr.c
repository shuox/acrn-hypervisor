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

int sched_rr_init(struct sched_control *ctl)
{
	struct sched_rr_control *rr_ctl = &per_cpu(sched_rr_ctl, ctl->pcpu_id);
	uint64_t tick_period = CYCLES_PER_MS;
	int ret = 0;

	ctl->priv = rr_ctl;
	INIT_LIST_HEAD(&rr_ctl->runqueue);

	/* The tick_timer is periodically */
	initialize_timer(&rr_ctl->tick_timer, sched_tick_handler, ctl,
			rdtsc() + tick_period, TICK_MODE_PERIODIC, tick_period);
	if (add_timer(&rr_ctl->tick_timer) < 0) {
		pr_err("Failed to add schedule tick timer!");
		ret = -1;
	}

	return ret;
}

void sched_rr_deinit(struct sched_control *ctl)
{
	struct sched_rr_control *rr_ctl = (struct sched_rr_control *)ctl->priv;
	del_timer(&rr_ctl->tick_timer);
}

void sched_rr_init_data(struct thread_object *obj)
{
	struct sched_rr_data *data;

	data = (struct sched_rr_data *)obj->data;
	INIT_LIST_HEAD(&data->list);
	data->left_cycles = data->slice_cycles = CONFIG_SLICE_MS * CYCLES_PER_MS;
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

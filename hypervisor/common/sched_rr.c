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

bool is_inqueue(struct thread_object *obj)
{
	struct sched_rr_data *data = (struct sched_rr_data *)obj->data;
	return !list_empty(&data->list);
}

void runqueue_add_head(struct thread_object *obj)
{
	struct sched_rr_control *rr_ctl = (struct sched_rr_control *)obj->sched_ctl->priv;
	struct sched_rr_data *data = (struct sched_rr_data *)obj->data;

	if (!is_inqueue(obj)) {
		list_add(&data->list, &rr_ctl->runqueue);
	}
}

void runqueue_add_tail(struct thread_object *obj)
{
	struct sched_rr_control *rr_ctl = (struct sched_rr_control *)obj->sched_ctl->priv;
	struct sched_rr_data *data = (struct sched_rr_data *)obj->data;

	if (!is_inqueue(obj)) {
		list_add_tail(&data->list, &rr_ctl->runqueue);
	}
}

void queue_remove(struct thread_object *obj)
{
	struct sched_rr_data *data = (struct sched_rr_data *)obj->data;
	/* treat no queued object as paused, should wake it up when events arrive */
	list_del_init(&data->list);
}

static void sched_tick_handler(void *param)
{
	struct sched_control  *ctl = (struct sched_control *)param;
	struct sched_rr_control *rr_ctl = (struct sched_rr_control *)ctl->priv;
	struct sched_rr_data *data;
	struct thread_object *current;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t now = rdtsc();
	uint64_t rflags;

	obtain_schedule_lock(pcpu_id, &rflags);
	current = ctl->curr_obj;

	/* If no vCPU start scheduling, ignore this tick */
	if (current == NULL || (is_idle_thread(current) && list_empty(&rr_ctl->runqueue))) {
		release_schedule_lock(pcpu_id, rflags);
		return;
	}
	data = (struct sched_rr_data *)current->data;
	/* consume the left_cycles of current thread_object if it is not idle */
	if (!is_idle_thread(current)) {
		data->left_cycles -= now - data->last_cycles;
		data->last_cycles = now;
	}
	/* make reschedule request if current ran out of its cycles */
	if (is_idle_thread(current) || data->left_cycles <= 0) {
		make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	}
	release_schedule_lock(pcpu_id, rflags);
}

int sched_rr_init(struct sched_control *ctl)
{
	struct sched_rr_control *rr_ctl = &per_cpu(sched_rr_ctl, ctl->pcpu_id);
	uint64_t tick_period = CYCLES_PER_MS;

	ctl->priv = rr_ctl;
	INIT_LIST_HEAD(&rr_ctl->runqueue);

	/* The tick_timer is periodically */
	initialize_timer(&rr_ctl->tick_timer, sched_tick_handler, ctl,
			rdtsc() + tick_period, TICK_MODE_PERIODIC, tick_period);

	return 0;
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

static struct thread_object *sched_rr_pick_next(struct sched_control *ctl)
{
	struct sched_rr_control *rr_ctl = (struct sched_rr_control *)ctl->priv;
	struct thread_object *next = NULL;
	struct thread_object *current = NULL;
	struct sched_rr_data *data;
	uint64_t now = rdtsc();

	current = ctl->curr_obj;
	data = (struct sched_rr_data *)current->data;
	/* Ignore the idle object, inactive objects */
	if (!is_idle_thread(current) && is_inqueue(current)) {
		data->left_cycles -= now - data->last_cycles;
		if (data->left_cycles <= 0) {
			/*  replenish thread_object with slice_cycles */
			data->left_cycles += data->slice_cycles;
		}
		/* move the thread_object to tail */
		queue_remove(current);
		runqueue_add_tail(current);
	}

	/* Pick the next runnable sched object
	 * 1) get the first item in runqueue firstly
	 * 2) if object picked has no time_cycles, replenish it pick this one
	 * 3) At least take one idle sched object if we have no runnable one after step 1) and 2)
	 */
	if (!list_empty(&rr_ctl->runqueue)) {
		next = get_first_item(&rr_ctl->runqueue, struct thread_object, data);
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

static void sched_rr_sleep(struct thread_object *obj)
{
	queue_remove(obj);
}

static void sched_rr_wake(struct thread_object *obj)
{
	struct sched_rr_control *rr_ctl = &per_cpu(sched_rr_ctl, obj->pcpu_id);

	if (rr_ctl->active_obj_num < 2U) {
		rr_ctl->active_obj_num++;
		if (rr_ctl->active_obj_num == 2U) {
			if (add_timer(&rr_ctl->tick_timer) < 0) {
				pr_err("Failed to add schedule tick timer!");
			}
		}
	}
	runqueue_add_head(obj);
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

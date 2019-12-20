/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <list.h>
#include <per_cpu.h>
#include <bits.h>
#include <schedule.h>
#include <trace.h>

/* The minimal time slice that a scheduled object can run */
#define CONFIG_DEFAULT_SCHED_PERIOD 	(5000UL)
#define CONFIG_DEFAULT_YIELD_RUNTIME 	(500UL)
#define CONFIG_WEIGHT_BASE 		(1024UL)
struct sched_cfs_data {
#define CFS_YIELD	(0UL)
	/* keep list as the first item */
	struct list_head list;

	uint64_t flags;

	uint64_t vruntime;	/* in us */
	uint32_t weight;
	uint32_t rq_weight;	/* cache the rq_weight in cfs_contorl */

	uint64_t period;	/* period */
	uint64_t vruntime_in_period;	/* vruntime used in current period */
	uint64_t last_cycles;
};

static uint64_t cycles2vruntime(struct thread_object *obj, uint64_t cycles)
{
	struct sched_cfs_data *data = (struct sched_cfs_data *)obj->data;
	uint64_t cycles_weighted = cycles * data->rq_weight / data->weight;

	return ticks_to_us(cycles_weighted);
}

static inline struct list_head *get_rq(struct thread_object *obj)
{
	struct sched_cfs_control *cfs_ctl = (struct sched_cfs_control *)obj->sched_ctl->priv;

	return &cfs_ctl->runqueue;
}

static bool is_inqueue(struct thread_object *obj)
{
	struct sched_cfs_data *data = (struct sched_cfs_data *)obj->data;
	return !list_empty(&data->list);
}

static uint64_t get_runtime_in_period(struct thread_object *obj, uint64_t now)
{
	struct sched_cfs_control *cfs_ctl = (struct sched_cfs_control *)obj->sched_ctl->priv;
	struct sched_cfs_data *data = (struct sched_cfs_data *)obj->data;
	uint64_t now_us = ticks_to_us(now);
	uint64_t curr_period = now_us / CONFIG_DEFAULT_SCHED_PERIOD;
	uint64_t period_rest = CONFIG_DEFAULT_SCHED_PERIOD - now_us % CONFIG_DEFAULT_SCHED_PERIOD;
	uint64_t vruntime_used = 0UL;

	TRACE_4I(TRACE_VMEXIT_CFS_OBJ_VRUNTIME, (uint32_t)data->vruntime_in_period, (uint32_t)data->period, (uint32_t)curr_period, (uint32_t)period_rest);
	if (!is_idle_thread(obj) && data->period == curr_period) {
		vruntime_used = data->vruntime_in_period;
	}
	/* get the right runtime in this period of the obj */
	return is_idle_thread(obj) ? period_rest :
		min(((CONFIG_DEFAULT_SCHED_PERIOD - vruntime_used) * data->weight / cfs_ctl->rq_weight), period_rest);
}

static void update_ctl_vruntimes(struct sched_cfs_control *cfs_ctl)
{
	struct thread_object *first, *last;
	struct sched_cfs_data *first_data, *last_data;
	struct list_head *rq = &cfs_ctl->runqueue;

	/* update min_vruntime and max_vruntime in this control block */
	first = list_entry(rq->next, struct thread_object, data);
	first_data = (struct sched_cfs_data *)first->data;
	last = list_entry(rq->prev, struct thread_object, data);
	last_data = (struct sched_cfs_data *)last->data;
	cfs_ctl->min_vruntime = first_data->vruntime;
	cfs_ctl->max_vruntime = last_data->vruntime;
}

static void runqueue_add(struct thread_object *obj)
{
	struct sched_cfs_data *data = (struct sched_cfs_data *)obj->data;
	struct sched_cfs_control *cfs_ctl = (struct sched_cfs_control *)obj->sched_ctl->priv;
	struct list_head *pos;
	struct thread_object *iter;
	struct sched_cfs_data *iter_data;

	if (!is_inqueue(obj)) {
		/*
		 * vruntime less more than min_vruntime, which could be a new one or
		 * sleep for long time, reassign it
		 */
		if ((data->vruntime + CONFIG_DEFAULT_SCHED_PERIOD) < cfs_ctl->min_vruntime) {
			data->vruntime = cfs_ctl->min_vruntime - CONFIG_DEFAULT_SCHED_PERIOD;
		}

		list_for_each(pos, get_rq(obj)) {
			iter = list_entry(pos, struct thread_object, data);
			iter_data = (struct sched_cfs_data *)iter->data;
			if (data->vruntime < iter_data->vruntime) {
				list_add(&data->list, get_rq(obj));
				break;
			}
		}
		if (!is_inqueue(obj)) {
			list_add_tail(&data->list, get_rq(obj));
		}

		update_ctl_vruntimes(cfs_ctl);
	}
}

static void runqueue_remove(struct thread_object *obj)
{
	struct sched_cfs_data *data = (struct sched_cfs_data *)obj->data;
	struct sched_cfs_control *cfs_ctl = (struct sched_cfs_control *)obj->sched_ctl->priv;

	/* treat no queued object as paused, should wake it up when events arrive */
	list_del_init(&data->list);

	update_ctl_vruntimes(cfs_ctl);
}

static void increase_thread_vruntime(struct thread_object *obj, uint64_t now)
{
	struct sched_cfs_data *data = (struct sched_cfs_data *)obj->data;
	uint64_t now_us = ticks_to_us(now);
	uint64_t curr_period = now_us / CONFIG_DEFAULT_SCHED_PERIOD;
	uint64_t vruntime;

	vruntime = cycles2vruntime(obj, now - data->last_cycles);
	data->last_cycles = now;
	if (data->period == curr_period) {
		/* if we increase vruntime in same period, record it */
		data->vruntime_in_period += vruntime;
		if (data->vruntime_in_period > CONFIG_DEFAULT_SCHED_PERIOD) {
			data->vruntime_in_period = CONFIG_DEFAULT_SCHED_PERIOD;
		}
	} else {
		/* if we increase vruntime in next period, reset it */
		data->period = curr_period;
		data->vruntime_in_period = 0UL;
	}
	data->vruntime += vruntime;
	if (is_inqueue(obj)) {
		list_del_init(&data->list);
		runqueue_add(obj);
	}
}

static void sched_timer_handler(__unused void *param)
{
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t rflags;

	obtain_schedule_lock(pcpu_id, &rflags);
	/* make reschedule request */
	make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	TRACE_2L(TRACE_VMEXIT_CFS_TIMER, rdtsc(), 0UL);
	release_schedule_lock(pcpu_id, rflags);
}

int sched_cfs_init(struct sched_control *ctl)
{
	struct sched_cfs_control *cfs_ctl = &per_cpu(sched_cfs_ctl, ctl->pcpu_id);

	ctl->priv = cfs_ctl;
	INIT_LIST_HEAD(&cfs_ctl->runqueue);

	initialize_timer(&cfs_ctl->sched_timer, sched_timer_handler, ctl, 0UL, TICK_MODE_ONESHOT, 0UL);
	cfs_ctl->max_vruntime = 0UL;
	cfs_ctl->min_vruntime = 0UL;
	cfs_ctl->nr_active = 0UL;
	cfs_ctl->rq_weight = 0UL;

	return 0;
}

void sched_cfs_deinit(struct sched_control *ctl)
{
	struct sched_cfs_control *cfs_ctl = (struct sched_cfs_control *)ctl->priv;
	del_timer(&cfs_ctl->sched_timer);
}

void sched_cfs_init_data(struct thread_object *obj)
{
	struct sched_cfs_data *data;

	data = (struct sched_cfs_data *)obj->data;
	INIT_LIST_HEAD(&data->list);
	data->vruntime = 0UL;
	data->weight = CONFIG_WEIGHT_BASE;
}

static struct thread_object *sched_cfs_pick_next(struct sched_control *ctl)
{
	struct sched_cfs_control *cfs_ctl = (struct sched_cfs_control *)ctl->priv;
	struct thread_object *current = NULL;
	struct thread_object *next = NULL;
	struct thread_object *t = NULL;
	struct sched_cfs_data *t_data, *next_data;
	struct list_head *pos;
	uint64_t now = rdtsc();
	uint64_t runtime = 0UL;
	int loop_times = 2;

	current = ctl->curr_obj;

	/* Ignore the idle object, inactive objects */
	if (!is_idle_thread(current)) {
		increase_thread_vruntime(current, now);
	}

	/* Pick the next runnable sched object */
	if (!list_empty(&cfs_ctl->runqueue)) {
		for (; next == NULL && loop_times > 0; loop_times--) {
			list_for_each(pos, &cfs_ctl->runqueue) {
				t = list_entry(pos, struct thread_object, data);
				t_data = (struct sched_cfs_data *)t->data;

				if (bitmap_test_and_clear_lock(CFS_YIELD, &t_data->flags)) {
					continue;
				}

				runtime = get_runtime_in_period(t, now);
				TRACE_2L(TRACE_VMEXIT_CFS_RUNTIME, runtime, *(uint64_t *)t->name);
				if (runtime == 0UL) {
					continue;
				} else {
					next = t;
					break;
				}
			}
		}
	}

	if (next == NULL) {
		/* no runnable thread, pick the idle */
		next = &get_cpu_var(idle);
		runtime = get_runtime_in_period(next, now);
	} else {
		/* update the cached cfs_control rq_weight */
		next_data = (struct sched_cfs_data *)next->data;
		next_data->rq_weight = cfs_ctl->rq_weight;
	}

	del_timer(&cfs_ctl->sched_timer);
	/* launch the sched_timer if there are multiple waiting runnable threads */
	if (((cfs_ctl->nr_active > 1UL) || (cfs_ctl->nr_active > 0UL && is_idle_thread(next))) && (runtime != 0UL)) {
		cfs_ctl->sched_timer.fire_tsc = now + us_to_ticks(runtime);
		if (add_timer(&cfs_ctl->sched_timer) < 0) {
			pr_err("Failed to add schedule tick timer!");
		}
		TRACE_2L(TRACE_VMEXIT_CFS_NEW_TIMER1, cfs_ctl->sched_timer.fire_tsc, now);
	}

	return next;
}

static void sched_cfs_sleep(struct thread_object *obj)
{
	struct sched_cfs_control *cfs_ctl = (struct sched_cfs_control *)obj->sched_ctl->priv;
	struct sched_cfs_data *data = (struct sched_cfs_data *)obj->data;

	runqueue_remove(obj);
	/* update active thread_object on this runqueue */
	cfs_ctl->nr_active--;
	cfs_ctl->rq_weight -= data->weight;
}

static void sched_cfs_wake(struct thread_object *obj)
{
	struct sched_cfs_control *cfs_ctl = (struct sched_cfs_control *)obj->sched_ctl->priv;
	struct sched_cfs_data *data = (struct sched_cfs_data *)obj->data;

	runqueue_add(obj);
	/* update active thread_object on this runqueue */
	cfs_ctl->nr_active++;
	cfs_ctl->rq_weight += data->weight;
}

/* can be change to return a bool to indicate if we need reschedule in framework */
static void sched_cfs_yield(struct sched_control *ctl)
{
	struct thread_object *current = ctl->curr_obj;
	struct sched_cfs_data *data = (struct sched_cfs_data *)current->data;

	/* if nr_active == 1, can return false */
	bitmap_set_lock(CFS_YIELD, &data->flags);
	increase_thread_vruntime(current, rdtsc());
}

static void dump_thread_obj(struct thread_object *obj)
{
	struct sched_cfs_data *data = (struct sched_cfs_data *)obj->data;
	pr_acrnlog("%12s%5d%20lld%20llx%10lld%10lld",
			obj->name, obj->status, data->vruntime, data->flags, data->period, data->vruntime_in_period);
}

static void sched_cfs_dump(struct sched_control *ctl)
{
	struct sched_cfs_control *cfs_ctl = (struct sched_cfs_control *)ctl->priv;
	struct thread_object *obj;
	struct list_head *pos;
	uint64_t rflags;

	pr_acrnlog("scheduler: sched_cfs max_vruntime: %lld(us)  min_vruntime %lld(us) current: %s now: %lld, next sched_timer %lld",
			cfs_ctl->max_vruntime, cfs_ctl->min_vruntime, ctl->curr_obj->name, rdtsc(), cfs_ctl->sched_timer.fire_tsc);
	pr_acrnlog("%12s%10s%15s(us)%15s%10s%15s", "object", "status", "vruntime", "flags", "period", "vruntime_in_period");
	obtain_schedule_lock(ctl->pcpu_id, &rflags);
	list_for_each(pos, &cfs_ctl->runqueue) {
		obj = list_entry(pos, struct thread_object, data);
		dump_thread_obj(obj);
	}
	release_schedule_lock(ctl->pcpu_id, rflags);
}

struct acrn_scheduler sched_cfs = {
	.name		= "sched_cfs",
	.init		= sched_cfs_init,
	.init_data	= sched_cfs_init_data,
	.pick_next	= sched_cfs_pick_next,
	.sleep		= sched_cfs_sleep,
	.wake		= sched_cfs_wake,
	.yield		= sched_cfs_yield,
	.deinit		= sched_cfs_deinit,
	.dump		= sched_cfs_dump,
};

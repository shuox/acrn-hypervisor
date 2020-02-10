/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <list.h>
#include <per_cpu.h>
#include <schedule.h>

#define CONFIG_MCU_MS	1U
#define CONFIG_CSA_MCU_NUM 5U
struct sched_bvt_data {
	/* keep list as the first item */
	struct list_head list;

	uint64_t mcu;
	uint64_t mcu_ratio;
	uint16_t weight;
	/* context switch allowance */
	uint64_t cs_allow_mcu;
	int64_t run_mcu;
	/* scheduler virtual time */
	int64_t svt_mcu;
	/* actual virtual time */
	int64_t avt_mcu;
	/* effective virtual time */
	int64_t evt_mcu;
	uint64_t residual;

	uint64_t start;
};

/*
 * @pre obj != NULL
 * @pre obj->data != NULL
 */
bool is_inqueue(struct thread_object *obj)
{
	struct sched_bvt_data *data = (struct sched_bvt_data *)obj->data;
	return !list_empty(&data->list);
}

/*
 * @pre obj != NULL
 * @pre obj->data != NULL
 * @pre obj->sched_ctl != NULL
 * @pre obj->sched_ctl->priv != NULL
 */
static void runqueue_add(struct thread_object *obj)
{
	struct sched_bvt_control *bvt_ctl =
		(struct sched_bvt_control *)obj->sched_ctl->priv;
	struct sched_bvt_data *data = (struct sched_bvt_data *)obj->data;
	struct list_head *pos;
	struct thread_object *iter_obj;
	struct sched_bvt_data *iter_data;

	/*
	 * the earliest evt_mcu has highest priority,
	 * the runqueue is ordered by priority.
	 */

	if (list_empty(&bvt_ctl->runqueue)) {
		list_add(&data->list, &bvt_ctl->runqueue);
	} else {
		list_for_each(pos, &bvt_ctl->runqueue) {
			iter_obj = list_entry(pos, struct thread_object, data);
			iter_data = (struct sched_bvt_data *)iter_obj->data;
			if (iter_data->evt_mcu > data->evt_mcu) {
				list_add_node(&data->list, pos->prev, pos);
				break;
			}
		}
		if (!is_inqueue(obj)) {
			list_add_tail(&data->list, &bvt_ctl->runqueue);
		}
	}
}

/*
 * @pre obj != NULL
 * @pre obj->data != NULL
 */
void runqueue_remove(struct thread_object *obj)
{
	struct sched_bvt_data *data = (struct sched_bvt_data *)obj->data;

	list_del_init(&data->list);
}

/*
 * @pre obj != NULL
 * @pre obj->data != NULL
 * @pre obj->sched_ctl != NULL
 * @pre obj->sched_ctl->priv != NULL
 */

int64_t get_svt(struct thread_object *obj)
{
	struct sched_bvt_control *bvt_ctl = (struct sched_bvt_control *)obj->sched_ctl->priv;
	struct sched_bvt_data *obj_data;
	struct thread_object *tmp_obj;
	int64_t svt_mcu = 0;

	if (!list_empty(&bvt_ctl->runqueue)) {
		tmp_obj = get_first_item(&bvt_ctl->runqueue, struct thread_object, data);
		obj_data = (struct sched_bvt_data *)tmp_obj->data;
		svt_mcu = obj_data->avt_mcu;
	}
	return svt_mcu;
}

static bool is_only_one_inqueue(struct list_head *node)
{
	return !list_empty(node) && (get_prev_node(node) == get_next_node(node));
}

static bool can_be_preempted(struct thread_object *obj, struct sched_bvt_control *bvt_ctl)
{
	struct sched_bvt_data *data;
	bool need = false;

	data = (struct sched_bvt_data *)obj->data;

	if (is_idle_thread(obj)) {
		if (!list_empty(&bvt_ctl->runqueue)) {
			need = true;
		}
	} else {
		if (data->run_mcu < 0) {
			if (!is_only_one_inqueue(&data->list)) {
				need = true;
			}
		}
	}
	return need;
}

static void sched_tick_handler(void *param)
{
	struct sched_control  *ctl = (struct sched_control *)param;
	struct sched_bvt_control *bvt_ctl = (struct sched_bvt_control *)ctl->priv;
	struct sched_bvt_data *data;
	struct thread_object *current;
	uint16_t pcpu_id = get_pcpu_id();
	uint64_t rflags;

	obtain_schedule_lock(pcpu_id, &rflags);
	current = ctl->curr_obj;

	if (current != NULL ) {
		data = (struct sched_bvt_data *)current->data;
		/* only non-idle thread need to consume run_mcu */
		if (!is_idle_thread(current) && !is_only_one_inqueue(&data->list)) {
			data->run_mcu -= 1;
		}
		if (can_be_preempted(current, bvt_ctl)) {
			make_reschedule_request(pcpu_id, DEL_MODE_IPI);
		}
	}
	release_schedule_lock(pcpu_id, rflags);
}

int sched_bvt_init(struct sched_control *ctl)
{
	struct sched_bvt_control *bvt_ctl = &per_cpu(sched_bvt_ctl, ctl->pcpu_id);
	uint64_t tick_period = CYCLES_PER_MS;
	int ret = 0;

	ASSERT(get_pcpu_id() == ctl->pcpu_id, "Init scheduler on wrong CPU!");

	ctl->priv = bvt_ctl;
	INIT_LIST_HEAD(&bvt_ctl->runqueue);

	/* The tick_timer is periodically */
	initialize_timer(&bvt_ctl->tick_timer, sched_tick_handler, ctl,
			rdtsc() + tick_period, TICK_MODE_PERIODIC, tick_period);

	if (add_timer(&bvt_ctl->tick_timer) < 0) {
		pr_err("Failed to add schedule tick timer!");
		ret = -1;
	}

	return ret;
}

void sched_bvt_deinit(__unused struct sched_control *ctl)
{
}

void sched_bvt_init_data(struct thread_object *obj)
{
	struct sched_bvt_data *data;

	data = (struct sched_bvt_data *)obj->data;
	INIT_LIST_HEAD(&data->list);
	data->mcu = CONFIG_MCU_MS * CYCLES_PER_MS;
	/* TODO: mcu advance value should be proportional to weight. */
	data->mcu_ratio = 1;
	data->cs_allow_mcu = CONFIG_CSA_MCU_NUM;
	data->run_mcu = data->cs_allow_mcu;
}

static uint64_t v2p(uint64_t virt_time, uint64_t ratio)
{
	return (uint64_t)(virt_time / ratio);
}

static uint64_t p2v(uint64_t phy_time, uint64_t ratio)
{
	return (uint64_t)(phy_time * ratio);
}

static void update_vt(struct thread_object *obj)
{
	struct sched_bvt_data *data;
	uint64_t now = rdtsc();
	uint64_t delta, delta_mcu = 0U;

	data = (struct sched_bvt_data *)obj->data;

	/* update current thread's avt_mcu and evt_mcu */
	if (now > data->start) {
		delta = now - data->start + data->residual;
		delta_mcu = (uint64_t)(delta / data->mcu);
		data->residual = delta % data->mcu;
	}
	data->avt_mcu += p2v(delta_mcu, data->mcu_ratio);
	/* TODO: evt_mcu = avt_mcu - (warp ? warpback : 0U) */
	data->evt_mcu = data->avt_mcu;

	/* Ignore the idle object, inactive objects */
	if (is_inqueue(obj)) {
		runqueue_remove(obj);
		runqueue_add(obj);
	}
	data->svt_mcu = get_svt(obj);

}

static struct thread_object *sched_bvt_pick_next(struct sched_control *ctl)
{
	struct sched_bvt_control *bvt_ctl = (struct sched_bvt_control *)ctl->priv;
	struct thread_object *first_obj = NULL, *second_obj = NULL;
	struct sched_bvt_data *first_data = NULL, *second_data = NULL;
	struct list_head *pos;
	struct thread_object *next = NULL;
	struct thread_object *current = ctl->curr_obj;
	uint64_t now = rdtsc();
	uint64_t delta_mcu = 0U;

	if (!is_idle_thread(current)) {
		update_vt(current);
	}

	if (!list_empty(&bvt_ctl->runqueue)) {
		list_for_each(pos, &bvt_ctl->runqueue) {
			if (first_obj == NULL) {
				first_obj = list_entry(pos, struct thread_object, data);
				first_data = (struct sched_bvt_data *)first_obj->data;
			} else {
				second_obj = list_entry(pos, struct thread_object, data);
				if (first_obj != second_obj) {
					second_data = (struct sched_bvt_data *)second_obj->data;
				}
				break;
			}
		}
		if (second_data != NULL) {
			if (second_data->evt_mcu >= first_data->evt_mcu) {
				delta_mcu = second_data->evt_mcu - first_data->evt_mcu;
			} else {
				pr_err("runqueue is not in order!!");
			}
			/* run_mcu is the real time the thread can run */
			first_data->run_mcu = v2p(delta_mcu, first_data->mcu_ratio)
				+ first_data->cs_allow_mcu;
		} else {
			/* there is only one object in runqueue, run_mcu will be ignored */
			first_data->run_mcu = 0U;
		}
		first_data->start = now;
		next = first_obj;
	} else {
		next = &get_cpu_var(idle);
	}

	return next;

}

static void sched_bvt_sleep(__unused struct thread_object *obj)
{
	runqueue_remove(obj);
}

static void sched_bvt_wake(__unused struct thread_object *obj)
{
	struct sched_bvt_data *data;

	/* update target not current thread's avt_mcu and evt_mcu */
	data = (struct sched_bvt_data *)obj->data;
	/* prevents a thread from claiming an excessive share
	 * of the CPU after sleeping for a long time as might happen
	 * if there was no adjustment */
	data->svt_mcu = get_svt(obj);
	data->avt_mcu = (data->avt_mcu > data->svt_mcu) ? data->avt_mcu : data->svt_mcu;
	/* TODO: evt_mcu = avt_mcu - (warp ? warpback : 0U) */
	data->evt_mcu = data->avt_mcu;
	/* add to runqueue in order */
	runqueue_add(obj);

}

struct acrn_scheduler sched_bvt = {
	.name		= "sched_bvt",
	.init		= sched_bvt_init,
	.init_data	= sched_bvt_init_data,
	.pick_next	= sched_bvt_pick_next,
	.sleep		= sched_bvt_sleep,
	.wake		= sched_bvt_wake,
	.deinit		= sched_bvt_deinit,
};

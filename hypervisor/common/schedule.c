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

static inline bool is_idle(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	return (obj == &per_cpu(idle, pcpu_id));
}

void get_schedule_lock(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	spinlock_obtain(&ctx->scheduler_lock);
}

void release_schedule_lock(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	spinlock_release(&ctx->scheduler_lock);
}

static void sched_tick_handler(void *data)
{
	struct sched_context *ctx = (struct sched_context *)data;
	struct sched_object *current, *obj;
	struct list_head *pos, *n;
	uint16_t pcpu_id = get_cpu_id();
	uint64_t now = rdtsc();

	get_schedule_lock(pcpu_id);
	current = ctx->curr_obj;

	/* replenish blocked sched_objects with slice_cycles, then move them to runqueue */
	list_for_each_safe(pos, n, &ctx->blocked_queue) {
		obj = list_entry(pos, struct sched_object, list);
		obj->data.left_cycles = obj->data.slice_cycles;
		remove_from_queue(obj);
		add_to_cpu_runqueue_tail(obj);
	}

	/* If no vCPU start scheduling, ignore this tick */
	if (current == NULL || (is_idle(current) && list_empty(&ctx->runqueue))) {
		release_schedule_lock(pcpu_id);
		return;
	}
	/* consume the left_cycles of current sched_object if it is not idle */
	if (!is_idle(current)) {
		current->data.left_cycles -= now - current->data.last_cycles;
		current->data.last_cycles = now;
	}
	/* make reschedule request if current ran out of its cycles */
	if (current->data.left_cycles <= 0 ) {
		make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	}
	release_schedule_lock(pcpu_id);
}

void init_scheduler(uint16_t pcpu_id)
{
	struct sched_context *ctx;
	uint64_t tick_period = CONFIG_TASK_SLICE_MS * CYCLES_PER_MS / 2;

	ctx = &per_cpu(sched_ctx, pcpu_id);
	spinlock_init(&ctx->queue_lock);
	spinlock_init(&ctx->scheduler_lock);
	INIT_LIST_HEAD(&ctx->runqueue);
	INIT_LIST_HEAD(&ctx->blocked_queue);
	ctx->flags = 0UL;
	ctx->curr_obj = NULL;

	/* The tick_timer is per-scheduler, periodically */
	initialize_timer(&ctx->tick_timer, sched_tick_handler, ctx,
			rdtsc() + tick_period, TICK_MODE_PERIODIC, tick_period);
	if (add_timer(&ctx->tick_timer) < 0) {
		pr_err("Failed to add schedule tick timer for pcpu%hu!", pcpu_id);
	} else {
		pr_info("add schedule tick timer for pcpu[%d] Done", pcpu_id);
	}
}

void sched_init_sched_data(struct sched_data *data)
{
	data->left_cycles = data->slice_cycles = CONFIG_TASK_SLICE_MS * CYCLES_PER_MS;
}

int16_t sched_pick_pcpu(uint64_t cpus_bitmap, uint64_t vcpu_sched_affinity)
{
	uint16_t pcpu;

	pcpu = ffs64(cpus_bitmap & vcpu_sched_affinity);
	if (pcpu == INVALID_BIT_INDEX) {
		return -1;
	}

	return pcpu;
}

void add_to_cpu_runqueue(struct sched_object *obj)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, obj->pcpu_id);

	spinlock_obtain(&ctx->queue_lock);
	if (list_empty(&obj->list)) {
		list_add(&obj->list, &ctx->runqueue);
	}
	spinlock_release(&ctx->queue_lock);
}

void add_to_cpu_runqueue_tail(struct sched_object *obj)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, obj->pcpu_id);

	spinlock_obtain(&ctx->queue_lock);
	if (list_empty(&obj->list)) {
		list_add_tail(&obj->list, &ctx->runqueue);
	}
	spinlock_release(&ctx->queue_lock);
}

void add_to_cpu_blocked_queue(struct sched_object *obj)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, obj->pcpu_id);

	spinlock_obtain(&ctx->queue_lock);
	if (list_empty(&obj->list)) {
		list_add(&obj->list, &ctx->blocked_queue);
	}
	spinlock_release(&ctx->queue_lock);
}

void remove_from_queue(struct sched_object *obj)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, obj->pcpu_id);

	spinlock_obtain(&ctx->queue_lock);
	list_del_init(&obj->list);
	spinlock_release(&ctx->queue_lock);
}

static bool is_active(struct sched_object *obj)
{
	return !list_empty(&obj->list);
}

static struct sched_object *get_next_sched_obj(struct sched_context *ctx)
{
	struct sched_object *obj = NULL;
	struct sched_object *curr = NULL;

	curr = ctx->curr_obj;
	/* Ignore the idle object, inactive objects */
	if (!is_idle(curr) && is_active(curr)) {
		remove_from_queue(curr);
		if (curr->data.left_cycles > 0) {
			add_to_cpu_runqueue_tail(curr);
		} else {
			add_to_cpu_blocked_queue(curr);
		}
	}

	/* Pick the next runnable sched object
	 * 1) get the first item in runqueue firstly
	 * 2) if no runnable object picked, replenish first object in blocked queue if we have and pick this one
	 * 3) At least take one idle sched object if we have no runnable ones after step 1) and 2)
	 */
	spinlock_obtain(&ctx->queue_lock);
	if (!list_empty(&ctx->runqueue)) {
		obj = get_first_item(&ctx->runqueue, struct sched_object, list);
	} else if (!list_empty(&ctx->blocked_queue)) {
		obj = get_first_item(&ctx->blocked_queue, struct sched_object, list);
		obj->data.left_cycles = obj->data.slice_cycles;
		list_del_init(&obj->list);
		list_add_tail(&obj->list, &ctx->runqueue);
	} else {
		obj = &get_cpu_var(idle);
	}
	spinlock_release(&ctx->queue_lock);

	return obj;
}

/**
 * @pre delmode == DEL_MODE_IPI || delmode == DEL_MODE_INIT
 */
void make_reschedule_request(uint16_t pcpu_id, uint16_t delmode)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	bitmap_set_lock(NEED_RESCHEDULE, &ctx->flags);
	if (get_cpu_id() != pcpu_id) {
		switch (delmode) {
		case DEL_MODE_IPI:
			send_single_ipi(pcpu_id, VECTOR_NOTIFY_VCPU);
			break;
		case DEL_MODE_INIT:
			send_single_init(pcpu_id);
			break;
		default:
			ASSERT(false, "Unknown delivery mode %u for pCPU%u", delmode, pcpu_id);
			break;
		}
	}
}

bool need_reschedule(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	return bitmap_test(NEED_RESCHEDULE, &ctx->flags);
}

void make_pcpu_offline(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	bitmap_set_lock(NEED_OFFLINE, &ctx->flags);
	if (get_cpu_id() != pcpu_id) {
		send_single_ipi(pcpu_id, VECTOR_NOTIFY_VCPU);
	}
}

int32_t need_offline(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	return bitmap_test_and_clear_lock(NEED_OFFLINE, &ctx->flags);
}

struct sched_object *get_cur_sched_obj(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	struct sched_object *obj = NULL;

	get_schedule_lock(pcpu_id);
	if (!is_idle(ctx->curr_obj)) {
		obj = ctx->curr_obj;
	}
	release_schedule_lock(pcpu_id);

	return obj;
}

/**
 * @pre obj != NULL
 */
uint16_t pcpuid_from_sched_obj(const struct sched_object *obj)
{
	return obj->pcpu_id;
}

static void prepare_switch(struct sched_object *prev, struct sched_object *next)
{
	uint64_t now = rdtsc();

	if ((prev != NULL) && (prev->switch_out != NULL)) {
		prev->switch_out(prev);
		if (!is_idle(prev)) {
			prev->data.left_cycles -= now - prev->data.last_cycles;
		}
	}

	/* update current object */
	get_cpu_var(sched_ctx).curr_obj = next;

	if ((next != NULL) && (next->switch_in != NULL)) {
		next->switch_in(next);
		if (!is_idle(next)) {
			next->data.last_cycles = now;
		}
	}
}

void schedule(void)
{
	uint16_t pcpu_id = get_cpu_id();
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	struct sched_object *next = NULL;
	struct sched_object *prev = ctx->curr_obj;

	get_schedule_lock(pcpu_id);
	next = get_next_sched_obj(ctx);
	bitmap_clear_lock(NEED_RESCHEDULE, &ctx->flags);

	/*
	 * If we picked different sched object, switch them; else, leave as it is
	 */
	if (prev != next) {
		pr_dbg("%s: prev[%s] next[%s]", __func__, prev->name, next->name);
		prepare_switch(prev, next);
		release_schedule_lock(pcpu_id);
		arch_switch_to(&prev->host_sp, &next->host_sp);
	} else {
		release_schedule_lock(pcpu_id);
	}
}

void yield(void)
{
	uint16_t pcpu_id = get_cpu_id();
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	get_schedule_lock(pcpu_id);
	if (!is_idle(ctx->curr_obj)) {
		TRACE_6C(TRACE_SCHED_YIELD, (uint8_t)ctx->curr_obj->name[0], (uint8_t)ctx->curr_obj->name[1],
					(uint8_t)ctx->curr_obj->name[2], (uint8_t)ctx->curr_obj->name[6],
					(uint8_t)ctx->curr_obj->name[7],(uint8_t)ctx->curr_obj->name[8]);
		remove_from_queue(ctx->curr_obj);
		make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	}
	release_schedule_lock(pcpu_id);
}

void wake(struct sched_object *obj)
{
	uint16_t pcpu_id = get_cpu_id();
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	get_schedule_lock(pcpu_id);
	if (is_idle(ctx->curr_obj)) {
		add_to_cpu_runqueue_tail(obj);
		make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	}
	release_schedule_lock(pcpu_id);
}

void run_sched_thread(struct sched_object *obj)
{
	if (obj->thread != NULL) {
		obj->thread(obj);
	}

	ASSERT(false, "Shouldn't go here, invalid thread!");
}

void switch_to_idle(sched_thread idle_thread)
{
	uint16_t pcpu_id = get_cpu_id();
	struct sched_object *idle = &per_cpu(idle, pcpu_id);
	char idle_name[16];

	snprintf(idle_name, 16U, "idle%hu", pcpu_id);
	(void)strncpy_s(idle->name, 16U, idle_name, 16U);
	idle->pcpu_id = pcpu_id;
	idle->data.left_cycles = 0;
	idle->data.slice_cycles = 0;
	idle->thread = idle_thread;
	idle->switch_out = NULL;
	idle->switch_in = NULL;
	get_cpu_var(sched_ctx).curr_obj = idle;

	run_sched_thread(idle);
}

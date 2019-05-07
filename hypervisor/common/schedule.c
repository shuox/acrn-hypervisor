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

#define SCHEDULER_MAX_NUMBER	4
static struct acrn_scheduler *schedulers[SCHEDULER_MAX_NUMBER] = {
	&sched_pin,
	&sched_rr,
};

bool sched_is_idle(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	return (obj == &per_cpu(idle, pcpu_id));
}

bool sched_is_active(struct sched_object *obj)
{
	return !list_empty(&obj->list);
}

static inline bool is_paused(struct sched_object *obj)
{
	return obj->status == SCHED_STS_PAUSED;
}

static inline bool is_runnable(struct sched_object *obj)
{
	return obj->status == SCHED_STS_RUNNABLE;
}

static inline bool is_running(struct sched_object *obj)
{
	return obj->status == SCHED_STS_RUNNING;
}

void sched_set_status(struct sched_object *obj, uint16_t status)
{
	obj->status = status;
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

void set_scheduler(uint16_t pcpu_id, struct acrn_scheduler *scheduler)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	ctx->scheduler = scheduler;
}

struct acrn_scheduler *get_scheduler(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	return ctx->scheduler;
}

struct acrn_scheduler *find_scheduler_by_name(const char *name)
{
	int i;
	struct acrn_scheduler *scheduler = NULL;

	for (i = 0; i < SCHEDULER_MAX_NUMBER && schedulers[i] != 0; i++) {
		if (strncmp(name, schedulers[i]->name, sizeof(schedulers[i]->name)) == 0) {
			scheduler = schedulers[i];
			break;
		}
	}
	return scheduler;
}

void init_sched(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);

	spinlock_init(&ctx->queue_lock);
	spinlock_init(&ctx->scheduler_lock);
	INIT_LIST_HEAD(&ctx->runqueue);
	INIT_LIST_HEAD(&ctx->retired_queue);
	ctx->flags = 0UL;
	ctx->current = NULL;
	ctx->stats.start_time = rdtsc();

	scheduler->init(ctx);
}

void sched_init_data(struct sched_object *obj)
{
	struct acrn_scheduler *scheduler = get_scheduler(obj->pcpu_id);
	scheduler->init_data(obj);
}

/* need refine to scheduler's callback with scheduler config */
uint16_t sched_pick_pcpu(uint64_t cpus_bitmap, uint64_t vcpu_sched_affinity)
{
	uint16_t pcpu = 0;

	pcpu = ffs64(cpus_bitmap & vcpu_sched_affinity);
	if (pcpu == INVALID_BIT_INDEX) {
		return INVALID_CPU_ID;
	}

	return pcpu;
}

void sched_runqueue_add_head(struct sched_object *obj)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, obj->pcpu_id);

	spinlock_obtain(&ctx->queue_lock);
	if (!sched_is_active(obj)) {
		list_add(&obj->list, &ctx->runqueue);
		sched_set_status(obj, SCHED_STS_RUNNABLE);
	}
	spinlock_release(&ctx->queue_lock);
}

void sched_runqueue_add_tail(struct sched_object *obj)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, obj->pcpu_id);

	spinlock_obtain(&ctx->queue_lock);
	if (!sched_is_active(obj)) {
		list_add_tail(&obj->list, &ctx->runqueue);
		sched_set_status(obj, SCHED_STS_RUNNABLE);
	}
	spinlock_release(&ctx->queue_lock);
}

void sched_retired_queue_add(struct sched_object *obj)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, obj->pcpu_id);

	spinlock_obtain(&ctx->queue_lock);
	if (!sched_is_active(obj)) {
		list_add(&obj->list, &ctx->retired_queue);
		sched_set_status(obj, SCHED_STS_RETIRED);
	}
	spinlock_release(&ctx->queue_lock);
}

void sched_queue_remove(struct sched_object *obj)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, obj->pcpu_id);

	spinlock_obtain(&ctx->queue_lock);
	/* treat no queued object as paused, should wake it up when events arrive */
	list_del_init(&obj->list);
	sched_set_status(obj, SCHED_STS_PAUSED);
	spinlock_release(&ctx->queue_lock);
}

/**
 * @pre delmode == DEL_MODE_IPI || delmode == DEL_MODE_INIT
 */
void make_reschedule_request(uint16_t pcpu_id, uint16_t delmode)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	bitmap_set_lock(NEED_RESCHEDULE, &ctx->flags);
	if (get_pcpu_id() != pcpu_id) {
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

struct sched_object *sched_get_current(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	struct sched_object *curr = NULL;

	get_schedule_lock(pcpu_id);
	curr = ctx->current;
	release_schedule_lock(pcpu_id);

	return curr;
}

/**
 * @pre obj != NULL
 */
uint16_t sched_get_pcpuid(const struct sched_object *obj)
{
	return obj->pcpu_id;
}

/*
 * with schedule lock hold
 */
static void sched_prepare_switch(struct sched_object *prev, struct sched_object *next)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	uint64_t now = rdtsc();

	if (prev->stats.last == 0UL) {
		prev->stats.last = ctx->stats.start_time;
	}
	prev->stats.total_runtime += now - prev->stats.last;
	next->stats.last = now;
	next->stats.sched_count++;

	sched_set_status(next, SCHED_STS_RUNNING);
	if (prev != next) {
		if ((prev != NULL) && (prev->switch_out != NULL)) {
			prev->switch_out(prev);
		}

		/* update current object */
		get_cpu_var(sched_ctx).current = next;

		if ((next != NULL) && (next->switch_in != NULL)) {
			next->switch_in(next);
		}
	}
}

void schedule(void)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);
	struct sched_object *next = NULL;
	struct sched_object *prev = ctx->current;

	get_schedule_lock(pcpu_id);
	bitmap_clear_lock(NEED_RESCHEDULE, &ctx->flags);
	next = scheduler->pick_next(ctx);
	sched_prepare_switch(prev, next);
	release_schedule_lock(pcpu_id);
	/*
	 * If we picked different sched object, switch them; else leave as it is
	 */
	if (prev != next) {
		pr_info("%s: prev[%s] next[%s][%x]", __func__, prev->name, next->name, next->host_sp);
		arch_switch_to(&prev->host_sp, &next->host_sp);
	}
}

void yield(void)
{
	uint16_t pcpu_id = get_pcpu_id();

	make_reschedule_request(pcpu_id, DEL_MODE_IPI);
}

void sleep(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;

	get_schedule_lock(pcpu_id);
	sched_queue_remove(obj);
	if (obj->notify_mode == SCHED_NOTIFY_INIT) {
		make_reschedule_request(pcpu_id, DEL_MODE_INIT);
	} else {
		if (is_running(obj)) {
			make_reschedule_request(pcpu_id, DEL_MODE_IPI);
		}
	}
	release_schedule_lock(pcpu_id);
}

void wake(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;

	get_schedule_lock(pcpu_id);
	if (is_paused(obj)) {
		sched_runqueue_add_head(obj);
		make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	}
	release_schedule_lock(pcpu_id);
}

void poke(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;

	get_schedule_lock(pcpu_id);
	if (is_running(obj)) {
		send_single_ipi(pcpu_id, VECTOR_NOTIFY_VCPU);
	} else if (is_runnable(obj)) {
		sched_queue_remove(obj);
		sched_runqueue_add_head(obj);
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
	uint16_t pcpu_id = get_pcpu_id();
	struct sched_object *idle = &per_cpu(idle, pcpu_id);
	char idle_name[16];

	snprintf(idle_name, 16U, "idle%hu", pcpu_id);
	(void)strncpy_s(idle->name, 16U, idle_name, 16U);
	sched_init_data(idle);
	idle->pcpu_id = pcpu_id;
	idle->thread = idle_thread;
	idle->switch_out = NULL;
	idle->switch_in = NULL;
	get_cpu_var(sched_ctx).current = idle;
	sched_set_status(idle, SCHED_STS_RUNNING);

	run_sched_thread(idle);
}

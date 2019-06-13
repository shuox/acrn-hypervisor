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

bool sched_is_idle(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	return (obj == &per_cpu(idle, pcpu_id));
}

static inline bool is_blocked(struct sched_object *obj)
{
	return obj->status == SCHED_STS_BLOCKED;
}

static inline bool is_runnable(struct sched_object *obj)
{
	return obj->status == SCHED_STS_RUNNABLE;
}

static inline bool is_running(struct sched_object *obj)
{
	return obj->status == SCHED_STS_RUNNING;
}

static inline void sched_set_status(struct sched_object *obj, uint16_t status)
{
	obj->status = status;
}

/**
 * @pre obj != NULL
 */
uint16_t sched_get_pcpuid(const struct sched_object *obj)
{
	return obj->pcpu_id;
}

void init_sched(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	spinlock_init(&ctx->scheduler_lock);
	ctx->flags = 0UL;
	ctx->current = NULL;
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

uint16_t sched_pick_pcpu(uint64_t cpus_bitmap, uint64_t vcpu_sched_affinity)
{
	uint16_t pcpu = 0;

	pcpu = ffs64(cpus_bitmap & vcpu_sched_affinity);
	if (pcpu == INVALID_BIT_INDEX) {
		pcpu = INVALID_CPU_ID;
	}

	return pcpu;
}

void sched_insert(struct sched_object *obj, uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	ctx->sched_obj = obj;
}

void sched_remove(__unused struct sched_object *obj, uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	ctx->sched_obj = NULL;
}

static struct sched_object *get_next_sched_obj(struct sched_context *ctx)
{
	return ctx->sched_obj == NULL ? &get_cpu_var(idle) : ctx->sched_obj;
}

struct sched_object *sched_get_current(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	return ctx->current;
}

/**
 * @pre delmode == DEL_MODE_IPI || delmode == DEL_MODE_INIT
 */
void make_reschedule_request(uint16_t pcpu_id, uint16_t delmode)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	bitmap_set_nolock(NEED_RESCHEDULE, &ctx->flags);
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

static void prepare_switch(struct sched_object *prev, struct sched_object *next)
{
	if ((prev != NULL) && (prev->switch_out != NULL)) {
		prev->switch_out(prev);
	}

	/* update current object */
	get_cpu_var(sched_ctx).current = next;

	if ((next != NULL) && (next->switch_in != NULL)) {
		next->switch_in(next);
	}
}

void schedule(void)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	struct sched_object *next = NULL;
	struct sched_object *prev = ctx->current;

	get_schedule_lock(pcpu_id);
	next = get_next_sched_obj(ctx);
	bitmap_clear_nolock(NEED_RESCHEDULE, &ctx->flags);
	/* Don't change prev object's status if it's not running */
	if (is_running(prev)) {
		sched_set_status(prev, SCHED_STS_RUNNABLE);
	}
	sched_set_status(next, SCHED_STS_RUNNING);

	if (prev == next) {
		release_schedule_lock(pcpu_id);
	} else {
		prepare_switch(prev, next);
		release_schedule_lock(pcpu_id);

		arch_switch_to(&prev->host_sp, &next->host_sp);
	}
}

void run_sched_thread(struct sched_object *obj)
{
	if (obj->thread != NULL) {
		obj->thread(obj);
	}

	ASSERT(false, "Shouldn't go here, invalid thread!");
}

void switch_to_idle(sched_thread_t idle_thread)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct sched_object *idle = &per_cpu(idle, pcpu_id);
	char idle_name[16];

	snprintf(idle_name, 16U, "idle%hu", pcpu_id);
	(void)strncpy_s(idle->name, 16U, idle_name, 16U);
	idle->pcpu_id = pcpu_id;
	idle->thread = idle_thread;
	idle->switch_out = NULL;
	idle->switch_in = NULL;
	get_cpu_var(sched_ctx).current = idle;
	sched_set_status(idle, SCHED_STS_RUNNING);

	run_sched_thread(idle);
}

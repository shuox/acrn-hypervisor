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

#define SCHED_OP(scheduler, op, ...)	\
	((scheduler->op == NULL) ? (typeof(scheduler->op(__VA_ARGS__)))0 : scheduler->op(__VA_ARGS__))

#define SCHEDULER_MAX_NUMBER	4
static struct acrn_scheduler *schedulers[SCHEDULER_MAX_NUMBER] = {
	&sched_mono,
	&sched_rr,
};

bool sched_is_idle(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	return (obj == &per_cpu(idle, pcpu_id));
}

static inline bool is_sleeping(struct sched_object *obj)
{
	return obj->status == SCHED_STS_SLEEPING;
}

static inline bool is_waiting(struct sched_object *obj)
{
	return obj->status == SCHED_STS_WAITING;
}

static inline bool is_running(struct sched_object *obj)
{
	return obj->status == SCHED_STS_RUNNING;
}

static void sched_set_status(struct sched_object *obj, uint16_t status)
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

int init_sched(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);

	spinlock_init(&ctx->scheduler_lock);
	ctx->flags = 0UL;
	ctx->current = NULL;
	ctx->pcpu_id = pcpu_id;
	return SCHED_OP(scheduler, init, ctx);
}

void sched_init_data(struct sched_object *obj)
{
	struct acrn_scheduler *scheduler = get_scheduler(obj->pcpu_id);
	SCHED_OP(scheduler, init_data, obj);
}

int suspend_sched(void)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, get_pcpu_id());

	return SCHED_OP(ctx->scheduler, suspend, ctx);
}

int resume_sched(void)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, get_pcpu_id());

	return SCHED_OP(ctx->scheduler, resume, ctx);
}

uint16_t sched_pick_pcpu(uint64_t cpus_bitmap, uint64_t vcpu_sched_affinity)
{
	uint16_t pcpu = 0;

	pcpu = ffs64(cpus_bitmap & vcpu_sched_affinity);
	if (pcpu == INVALID_BIT_INDEX) {
		return INVALID_CPU_ID;
	}

	return pcpu;
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

void schedule_on_pcpu(uint16_t pcpu_id, struct sched_object *obj)
{
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);
	get_schedule_lock(pcpu_id);
	SCHED_OP(scheduler, insert, obj);
	make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	release_schedule_lock(pcpu_id);
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

extern uint64_t sched_switch_start[4];
extern uint64_t sched_switch_total[4];
extern uint64_t sched_switch_count[4];
void schedule(void)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);
	struct sched_object *next = NULL;
	struct sched_object *prev = ctx->current;

	sched_switch_start[pcpu_id] = rdtsc();
	get_schedule_lock(pcpu_id);
	bitmap_clear_lock(NEED_RESCHEDULE, &ctx->flags);
	next = SCHED_OP(scheduler, pick_next, ctx);
	/* update current object and status */
	if (is_running(prev)) {
		sched_set_status(prev, SCHED_STS_WAITING);
	}
	sched_set_status(next, SCHED_STS_RUNNING);
	ctx->current = next;
	release_schedule_lock(pcpu_id);
	/*
	 * If we picked different sched object, switch them; else leave as it is
	 */
	if (prev != next) {
		if ((prev != NULL) && (prev->switch_out != NULL)) {
			prev->switch_out(prev);
		}

		if ((next != NULL) && (next->switch_in != NULL)) {
			next->switch_in(next);
		}
		TRACE_2L(TRACE_SCHED_SWITCH,
				(uint64_t)(((prev->name[2]-'0') << 16) | (prev->name[8]-'0')),
				(uint64_t)(((next->name[2]-'0') << 16) | (next->name[8]-'0')));
		arch_switch_to(&prev->host_sp, &next->host_sp);
		sched_switch_total[pcpu_id] += rdtsc() - sched_switch_start[pcpu_id];
		sched_switch_count[pcpu_id] ++;
	}
}

void yield(void)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	SCHED_OP(scheduler, yield, ctx);
	make_reschedule_request(pcpu_id, DEL_MODE_IPI);
}

void sleep(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);

	get_schedule_lock(pcpu_id);
	SCHED_OP(scheduler, sleep, obj);
	if (obj->notify_mode == SCHED_NOTIFY_INIT) {
		make_reschedule_request(pcpu_id, DEL_MODE_INIT);
	} else {
		if (is_running(obj)) {
			make_reschedule_request(pcpu_id, DEL_MODE_IPI);
		}
	}
	sched_set_status(obj, SCHED_STS_SLEEPING);
	release_schedule_lock(pcpu_id);
}

void wake(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);

	get_schedule_lock(pcpu_id);
	if (is_sleeping(obj)) {
		SCHED_OP(scheduler, wake, obj);
		sched_set_status(obj, SCHED_STS_WAITING);
		make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	}
	release_schedule_lock(pcpu_id);
}

void poke(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);

	get_schedule_lock(pcpu_id);
	/* TODO: need check if it's current pcpu */
	if (is_running(obj)) {
		send_single_ipi(pcpu_id, VECTOR_NOTIFY_VCPU);
	} else if (is_waiting(obj)) {
		SCHED_OP(scheduler, poke, obj);
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
	idle->pcpu_id = pcpu_id;
	idle->thread = idle_thread;
	idle->switch_out = NULL;
	idle->switch_in = NULL;
	get_cpu_var(sched_ctx).current = idle;
	sched_init_data(idle);
	sched_set_status(idle, SCHED_STS_RUNNING);

	run_sched_thread(idle);
}

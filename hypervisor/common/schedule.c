/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <rtl.h>
#include <list.h>
#include <bits.h>
#include <errno.h>
#include <cpu.h>
#include <per_cpu.h>
#include <lapic.h>
#include <schedule.h>
#include <sprintf.h>

#define SCHED_OP(scheduler, op, ...)   \
	(((scheduler)->op == NULL) ? (typeof((scheduler)->op(__VA_ARGS__)))0 : (scheduler)->op(__VA_ARGS__))

static struct acrn_scheduler *schedulers[SCHEDULER_MAX_NUMBER] = {
	&sched_noop,
};

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

static void set_scheduler(uint16_t pcpu_id, struct acrn_scheduler *scheduler)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	ctx->scheduler = scheduler;
}

static struct acrn_scheduler *get_scheduler(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	return ctx->scheduler;
}

static struct acrn_scheduler *find_scheduler_by_name(const char *name)
{
	unsigned int i;
	struct acrn_scheduler *scheduler = NULL;

	for (i = 0U; i < SCHEDULER_MAX_NUMBER && schedulers[i] != NULL; i++) {
		if (strncmp(name, schedulers[i]->name, sizeof(schedulers[i]->name)) == 0) {
			scheduler = schedulers[i];
			break;
		}
	}

	return scheduler;
}

int32_t init_pcpu_schedulers(struct acrn_vm_config *vm_config)
{
	int32_t ret = 0;
	uint16_t pcpu_id;
	struct acrn_scheduler *scheduler;
	uint64_t pcpu_bitmap = vm_config->pcpu_bitmap;

	/* verify & set scheduler for all pcpu of this VM */
	pcpu_id = ffs64(pcpu_bitmap);
	while (pcpu_id != INVALID_BIT_INDEX) {
		scheduler = get_scheduler(pcpu_id);
		if (scheduler && scheduler != find_scheduler_by_name(vm_config->scheduler)) {
			pr_err("%s: detect scheduler conflict on pcpu%d, might be overwrote!\n", __func__, pcpu_id);
		}
		scheduler = find_scheduler_by_name(vm_config->scheduler);
		if (!scheduler) {
			pr_err("%s: No valid scheduler found for pcpu%d\n", __func__, pcpu_id);
			ret = -EINVAL;
			break;
		}
		pr_acrnlog("%s: Set pcpu%d scheduler: %s", __func__, pcpu_id, scheduler->name);
		set_scheduler(pcpu_id, scheduler);
		bitmap_clear_nolock(pcpu_id, &pcpu_bitmap);
		pcpu_id = ffs64(pcpu_bitmap);
	}

	return ret;
}

/**
 * @pre obj != NULL
 */
uint16_t sched_get_pcpuid(const struct sched_object *obj)
{
	return obj->pcpu_id;
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

void init_sched(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);

	spinlock_init(&ctx->scheduler_lock);
	ctx->flags = 0UL;
	ctx->current = NULL;
	ctx->pcpu_id = pcpu_id;
	if (ctx->scheduler == NULL) {
		ctx->scheduler = &sched_noop;
	}
	SCHED_OP(ctx->scheduler, init, ctx);
}

void deinit_sched(uint16_t pcpu_id)
{
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);

	SCHED_OP(scheduler, deinit, ctx);
}

void sched_init_data(struct sched_object *obj)
{
	struct acrn_scheduler *scheduler = get_scheduler(obj->pcpu_id);
	SCHED_OP(scheduler, init_data, obj);
}

void sched_deinit_data(struct sched_object *obj)
{
	struct acrn_scheduler *scheduler = get_scheduler(obj->pcpu_id);
	SCHED_OP(scheduler, deinit_data, obj);
}

void sched_insert(struct sched_object *obj, uint16_t pcpu_id)
{
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);

	get_schedule_lock(pcpu_id);
	SCHED_OP(scheduler, insert, obj);
	make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	release_schedule_lock(pcpu_id);
}

void sched_remove(struct sched_object *obj, uint16_t pcpu_id)
{
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);

	get_schedule_lock(pcpu_id);
	SCHED_OP(scheduler, remove, obj);
	make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	release_schedule_lock(pcpu_id);
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

void schedule(void)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct sched_context *ctx = &per_cpu(sched_ctx, pcpu_id);
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);
	struct sched_object *next = NULL;
	struct sched_object *prev = ctx->current;

	get_schedule_lock(pcpu_id);
	next = SCHED_OP(scheduler, pick_next, ctx);
	bitmap_clear_nolock(NEED_RESCHEDULE, &ctx->flags);
	/* Don't change prev object's status if it's not running */
	if (is_running(prev)) {
		sched_set_status(prev, SCHED_STS_RUNNABLE);
	}
	sched_set_status(next, SCHED_STS_RUNNING);
	ctx->current = next;
	release_schedule_lock(pcpu_id);

	/* If we picked different sched object, switch context */
	if (prev != next) {
		if ((prev != NULL) && (prev->switch_out != NULL)) {
			prev->switch_out(prev);
		}

		if ((next != NULL) && (next->switch_in != NULL)) {
			next->switch_in(next);
		}

		arch_switch_to(&prev->host_sp, &next->host_sp);
	}
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
	sched_set_status(obj, SCHED_STS_BLOCKED);
	release_schedule_lock(pcpu_id);
}

void wake(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);

	get_schedule_lock(pcpu_id);
	if (is_blocked(obj)) {
		SCHED_OP(scheduler, wake, obj);
		sched_set_status(obj, SCHED_STS_RUNNABLE);
		make_reschedule_request(pcpu_id, DEL_MODE_IPI);
	}
	release_schedule_lock(pcpu_id);
}

void poke(struct sched_object *obj)
{
	uint16_t pcpu_id = obj->pcpu_id;
	struct acrn_scheduler *scheduler = get_scheduler(pcpu_id);

	get_schedule_lock(pcpu_id);
	if (is_running(obj) && get_pcpu_id() != pcpu_id) {
		send_single_ipi(pcpu_id, VECTOR_NOTIFY_VCPU);
	} else if (is_runnable(obj)) {
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
	sched_init_data(idle);
	sched_set_status(idle, SCHED_STS_RUNNING);

	run_sched_thread(idle);
}

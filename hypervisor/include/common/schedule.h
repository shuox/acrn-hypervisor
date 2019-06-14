/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SCHEDULE_H
#define SCHEDULE_H
#include <spinlock.h>

#define	NEED_RESCHEDULE		(1U)

#define DEL_MODE_INIT		(1U)
#define DEL_MODE_IPI		(2U)

enum sched_object_state {
	SCHED_STS_UNKNOWN,
	SCHED_STS_RUNNING,
	SCHED_STS_RUNNABLE,
	SCHED_STS_BLOCKED
};

enum sched_notify_mode {
	SCHED_NOTIFY_INIT = DEL_MODE_INIT,
	SCHED_NOTIFY_IPI = DEL_MODE_IPI
};

struct sched_object;
typedef void (*sched_thread_t)(struct sched_object *obj);
typedef void (*switch_t)(struct sched_object *obj);
struct sched_object {
	char name[16];
	uint16_t pcpu_id;
	struct sched_context *ctx;
	sched_thread_t thread;
	volatile enum sched_object_state status;
	enum sched_notify_mode notify_mode;

	uint64_t host_sp;
	switch_t switch_out;
	switch_t switch_in;
};

struct sched_context {
	uint64_t flags;
	struct sched_object *current;
	spinlock_t scheduler_lock;

	struct sched_object *sched_obj;
};

bool sched_is_idle(struct sched_object *obj);
uint16_t sched_get_pcpuid(const struct sched_object *obj);
struct sched_object *sched_get_current(uint16_t pcpu_id);

void init_sched(uint16_t pcpu_id);
void switch_to_idle(sched_thread_t idle_thread);
void get_schedule_lock(uint16_t pcpu_id);
void release_schedule_lock(uint16_t pcpu_id);

uint16_t sched_pick_pcpu(uint64_t pcpu_bitmap, uint64_t vcpu_sched_affinity);

void sched_insert(struct sched_object *obj, uint16_t pcpu_id);
void sched_remove(struct sched_object *obj, uint16_t pcpu_id);

void make_reschedule_request(uint16_t pcpu_id, uint16_t delmode);
bool need_reschedule(uint16_t pcpu_id);

void sleep(struct sched_object *obj);
void wake(struct sched_object *obj);
void schedule(void);
void run_sched_thread(struct sched_object *obj);

void arch_switch_to(void *prev_sp, void *next_sp);
#endif /* SCHEDULE_H */


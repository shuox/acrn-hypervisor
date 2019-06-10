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

struct sched_object;
typedef void (*sched_thread_t)(struct sched_object *obj);
typedef void (*switch_t)(struct sched_object *obj);
struct sched_object {
	char name[16];
	uint16_t pcpu_id;
	struct sched_context *ctx;
	sched_thread_t thread;

	uint64_t host_sp;
	switch_t switch_out;
	switch_t switch_in;
};

struct sched_context {
	uint64_t flags;
	struct sched_object *curr_obj;
	spinlock_t scheduler_lock;

	struct sched_object *sched_obj;
};

uint16_t sched_get_pcpuid(const struct sched_object *obj);

void init_scheduler(void);
void switch_to_idle(sched_thread_t idle_thread);
void get_schedule_lock(uint16_t pcpu_id);
void release_schedule_lock(uint16_t pcpu_id);

void set_pcpu_used(uint16_t pcpu_id);
uint16_t allocate_pcpu(void);
void free_pcpu(uint16_t pcpu_id);

void sched_insert(struct sched_object *obj, uint16_t pcpu_id);
void sched_remove(struct sched_object *obj, uint16_t pcpu_id);

void make_reschedule_request(uint16_t pcpu_id, uint16_t delmode);
bool need_reschedule(uint16_t pcpu_id);

void schedule(void);
void run_sched_thread(struct sched_object *obj);

void arch_switch_to(void *prev_sp, void *next_sp);
#endif /* SCHEDULE_H */


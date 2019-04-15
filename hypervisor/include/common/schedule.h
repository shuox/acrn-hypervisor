/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SCHEDULE_H
#define SCHEDULE_H
#include <spinlock.h>

#define	NEED_RESCHEDULE		(1U)
#define	NEED_OFFLINE		(2U)

#define DEL_MODE_INIT		(1U)
#define DEL_MODE_IPI		(2U)

#define INVALID_TASK_ID		0xFFFFU
#define TASK_ID_MONOPOLY	0xFFFEU

struct sched_object;
typedef void (*run_thread_t)(struct sched_object *obj);
typedef void (*switch_t)(struct sched_object *obj);

struct sched_data {
	uint16_t pcpu_id;
	uint64_t slice_cycles;

	uint64_t last_cycles;
	int64_t left_cycles;

	uint64_t host_sp;
};

struct sched_object {
	char name[16];
	struct list_head list;
	struct sched_data data;
	run_thread_t thread;
	switch_t switch_out;
	switch_t switch_in;
};

struct sched_context {
	spinlock_t queue_lock;
	struct list_head runqueue;
	struct list_head blocked_queue;
	uint64_t flags;
	struct sched_object *curr_obj;
	spinlock_t scheduler_lock;
	struct hv_timer tick_timer;
};

void init_scheduler(uint16_t pcpu_id);
void switch_to_idle(run_thread_t idle_thread);
void get_schedule_lock(uint16_t pcpu_id);
void release_schedule_lock(uint16_t pcpu_id);

int32_t sched_pick_pcpu(struct sched_data *data, uint64_t cpus_bitmap, uint64_t vcpu_sched_affinity);

void add_to_cpu_runqueue(struct sched_object *obj);
void add_to_cpu_runqueue_tail(struct sched_object *obj);
void remove_from_queue(struct sched_object *obj);

void make_reschedule_request(uint16_t pcpu_id, uint16_t delmode);
bool need_reschedule(uint16_t pcpu_id);
void make_pcpu_offline(uint16_t pcpu_id);
int32_t need_offline(uint16_t pcpu_id);
struct sched_object *get_cur_sched_obj(uint16_t pcpu_id);

uint16_t pcpuid_from_sched_obj(const struct sched_object *obj);

void yield(void);
void wake(struct sched_object *obj);
void schedule(void);
void run_sched_thread(struct sched_object *obj);

void arch_switch_to(void *prev_sp, void *next_sp);
#endif /* SCHEDULE_H */


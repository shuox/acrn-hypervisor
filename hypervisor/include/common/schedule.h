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
typedef void (*sched_thread)(struct sched_object *obj);
typedef void (*switch_t)(struct sched_object *obj);

struct sched_data {
	uint64_t slice_cycles;

	uint64_t last_cycles;
	int64_t left_cycles;
};

struct sched_object {
	char name[16];
	uint16_t pcpu_id;
	struct list_head list;
	sched_thread thread;
	struct sched_data data;

	uint64_t host_sp;
	switch_t switch_in;
	switch_t switch_out;
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
void switch_to_idle(sched_thread thread);
void get_schedule_lock(uint16_t pcpu_id);
void release_schedule_lock(uint16_t pcpu_id);

int16_t sched_pick_pcpu(uint64_t pcpu_bitmap, uint64_t vcpu_sched_affinity);
void sched_init_sched_data(struct sched_data *data);

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


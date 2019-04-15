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

#define SCHED_DATA_SIZE		64U

#define SCHED_RUNNABLE		0U
#define SCHED_RUNNING		1U
#define SCHED_BLOCKED		2U
#define SCHED_PAUSED		3U

struct sched_object;
typedef void (*sched_thread)(struct sched_object *obj);
typedef void (*switch_t)(struct sched_object *obj);

struct sched_object {
	char name[16];
	uint16_t pcpu_id;
	struct list_head list;
	sched_thread thread;
	uint8_t status;

	uint64_t host_sp;
	switch_t switch_in;
	switch_t switch_out;

	uint32_t data[SCHED_DATA_SIZE];
};

struct sched_context {
	spinlock_t scheduler_lock;
	spinlock_t queue_lock;
	struct list_head runqueue;
	struct list_head retired_queue;
	uint64_t flags;
	bool inited;
	struct sched_object *current;
	struct hv_timer tick_timer;

	struct acrn_scheduler *scheduler;
};

void init_sched(uint16_t pcpu_id);
void switch_to_idle(sched_thread thread);
void get_schedule_lock(uint16_t pcpu_id);
void release_schedule_lock(uint16_t pcpu_id);

void set_scheduler(uint16_t pcpu_id, struct acrn_scheduler *scheduler);
struct acrn_scheduler *find_scheduler_by_name(const char *name);
uint16_t sched_assign_pcpu(uint64_t pcpu_bitmap, uint64_t vcpu_sched_affinity);
void sched_init_data(struct sched_object *obj);
void sched_runqueue_add_head(struct sched_object *obj);
void sched_runqueue_add_tail(struct sched_object *obj);
void sched_retired_queue_add(struct sched_object *obj);
void sched_queue_remove(struct sched_object *obj);

bool sched_is_idle(struct sched_object *obj);
bool sched_is_active(struct sched_object *obj);
struct sched_object *sched_get_current(uint16_t pcpu_id);
uint16_t sched_get_pcpuid(const struct sched_object *obj);

void make_reschedule_request(uint16_t pcpu_id, uint16_t delmode);
bool need_reschedule(uint16_t pcpu_id);
void make_pcpu_offline(uint16_t pcpu_id);
int32_t need_offline(uint16_t pcpu_id);

void sched_set_status(struct sched_object *obj, uint16_t status);
void yield(void);
void wake(struct sched_object *obj);
void poke(struct sched_object *obj);
void schedule(void);
void run_sched_thread(struct sched_object *obj);

void arch_switch_to(void *prev_sp, void *next_sp);

struct acrn_scheduler {
	char name[16];

	int	 (*init)(struct sched_context *ctx);
	void 	 (*init_data)(struct sched_object *obj);
	uint16_t (*assign_pcpu)(uint64_t cpus_bitmap, uint64_t vcpu_sched_affinity);
	void	 (*prepare_switch)(struct sched_object *prev, struct sched_object *next);
	struct sched_object* (*pick_next)(struct sched_context *ctx);
};
extern struct acrn_scheduler sched_rr;

#endif /* SCHEDULE_H */


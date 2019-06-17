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

#define SCHED_DATA_SIZE		(256U)

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

	uint8_t data[SCHED_DATA_SIZE];
};

struct sched_context {
	uint16_t pcpu_id;
	uint64_t flags;
	struct sched_object *current;
	spinlock_t scheduler_lock;
	struct acrn_scheduler *scheduler;
	void *priv;
};

#define SCHEDULER_MAX_NUMBER 4U
struct acrn_scheduler {
	char name[16];

	/* init scheduler */
	int	(*init)(struct sched_context *ctx);
	/* init private data of scheduler */
	void	(*init_data)(struct sched_object *obj);
	/* insert sched_object into its schedule context */
	void	(*insert)(struct sched_object *obj);
	/* pick the next schedule object */
	struct sched_object* (*pick_next)(struct sched_context *ctx);
	/* put schedule object into sleep */
	void	(*sleep)(struct sched_object *obj);
	/* wake up schedule object from sleep status */
	void	(*wake)(struct sched_object *obj);
	/* yield current schedule object */
	void	(*yield)(struct sched_context *ctx);
	/* poke the schedule object */
	void	(*poke)(struct sched_object *obj);
	/* remove sched_object from its schedule context */
	void	(*remove)(struct sched_object *obj);
	/* deinit private data of scheduler */
	void	(*deinit_data)(struct sched_object *obj);
	/* deinit scheduler */
	void	(*deinit)(struct sched_context *ctx);
};
extern struct acrn_scheduler sched_noop;

struct sched_noop_context {
	struct sched_object *noop_sched_obj;
};

bool sched_is_idle(struct sched_object *obj);
uint16_t sched_get_pcpuid(const struct sched_object *obj);
struct sched_object *sched_get_current(uint16_t pcpu_id);

void init_sched(uint16_t pcpu_id);
void deinit_sched(uint16_t pcpu_id);
void switch_to_idle(sched_thread_t idle_thread);
void get_schedule_lock(uint16_t pcpu_id);
void release_schedule_lock(uint16_t pcpu_id);

uint16_t sched_pick_pcpu(uint64_t pcpu_bitmap, uint64_t vcpu_sched_affinity);

void sched_init_data(struct sched_object *obj);
void sched_deinit_data(struct sched_object *obj);
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


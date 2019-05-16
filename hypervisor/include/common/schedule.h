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

#define INVALID_TASK_ID		0xFFFFU
#define TASK_ID_MONOPOLY	0xFFFEU

#define SCHED_DATA_SIZE		64U

enum sched_object_state {
	SCHED_STS_UNKNOWN,
	SCHED_STS_RUNNING,
	SCHED_STS_WAITING,
	SCHED_STS_SLEEPING
};

enum sched_notify_mode {
	SCHED_NOTIFY_INIT = DEL_MODE_INIT,
	SCHED_NOTIFY_IPI = DEL_MODE_IPI
};

struct sched_object;
typedef void (*sched_thread)(struct sched_object *obj);
typedef void (*switch_t)(struct sched_object *obj);

struct sched_object {
	char name[16];
	uint16_t pcpu_id;
	struct sched_context *ctx;
	sched_thread thread;
	volatile enum sched_object_state status;
	enum sched_notify_mode notify_mode;

	uint64_t host_sp;
	switch_t switch_in;
	switch_t switch_out;

	struct {
		uint64_t last;
		uint64_t total_runtime;
		uint64_t sched_count;
	} stats;

	uint32_t data[SCHED_DATA_SIZE];
};

struct sched_context {
	uint16_t pcpu_id;
	spinlock_t scheduler_lock;
	uint64_t flags;
	struct sched_object *current;
	struct acrn_scheduler *scheduler;
	void *priv;
};

struct acrn_scheduler {
	char name[16];

	/* init scheduler */
	int	(*init)(struct sched_context *ctx);
	/* init private data of scheduler */
	void 	(*init_data)(struct sched_object *obj);
	void	(*insert)(struct sched_object *obj);
	/* pick the next schedule object */
	struct sched_object* (*pick_next)(struct sched_context *ctx);
	/* put schedule object into sleep */
	void	(*sleep)(struct sched_object *obj);
	/* wake up schedule object from sleep status */
	void	(*wake)(struct sched_object *obj);
	/* yield current schedule object */
	void 	(*yield)(struct sched_context *ctx);
	/* poke the schedule object */
	void	(*poke)(struct sched_object *obj);
	/* migrate schedule object from one context to another */
	void	(*migrate)(struct sched_context *to, struct sched_context *from,
			struct sched_object *obj);
	/* suspend schedule */
	int 	(*suspend)(struct sched_context *ctx);
	/* resume schedule */
	int 	(*resume)(struct sched_context *ctx);
	/* deinit private data of scheduler */
	void 	(*deinit_data)(struct sched_object *obj);
	/* deinit scheduler */
	int	(*deinit)(struct sched_context *ctx);

	void 	(*dump)(struct sched_context *ctx);
};
extern struct acrn_scheduler sched_rr;
extern struct acrn_scheduler sched_mono;

struct sched_rr_context {
	struct list_head runqueue;
	struct list_head retired_queue;
	struct hv_timer tick_timer;

	struct {
		uint64_t start_time;
		uint64_t tick_count;
	} stats;
};

struct sched_mono_context {
	struct sched_object *mono_sched_obj;

	struct {
		uint64_t start_time;
	} stats;
};

int init_sched(uint16_t pcpu_id);
void switch_to_idle(sched_thread thread);
void get_schedule_lock(uint16_t pcpu_id);
void release_schedule_lock(uint16_t pcpu_id);

void set_scheduler(uint16_t pcpu_id, struct acrn_scheduler *scheduler);
struct acrn_scheduler *get_scheduler(uint16_t pcpu_id);
struct acrn_scheduler *find_scheduler_by_name(const char *name);
uint16_t sched_pick_pcpu(uint64_t pcpu_bitmap, uint64_t vcpu_sched_affinity);
void sched_init_data(struct sched_object *obj);

bool sched_is_idle(struct sched_object *obj);
struct sched_object *sched_get_current(uint16_t pcpu_id);
uint16_t sched_get_pcpuid(const struct sched_object *obj);

void schedule_on_pcpu(uint16_t pcpu_id, struct sched_object *obj);
void make_reschedule_request(uint16_t pcpu_id, uint16_t delmode);
bool need_reschedule(uint16_t pcpu_id);

int suspend_sched(void);
int resume_sched(void);

void sleep(struct sched_object *obj);
void wake(struct sched_object *obj);
void poke(struct sched_object *obj);
void yield(void);
void schedule(void);
void run_sched_thread(struct sched_object *obj);

void arch_switch_to(void *prev_sp, void *next_sp);

#endif /* SCHEDULE_H */


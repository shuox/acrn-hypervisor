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

void sched_pin_init_data(__unused struct sched_object *obj)
{
}

int sched_pin_init(__unused struct sched_context *ctx)
{
	return 0;
}

static struct sched_object *sched_pin_pick_next(struct sched_context *ctx)
{
	struct sched_object *next = NULL;

	/* pinned sched_object, if runqueue is null, then return idle */
	spinlock_obtain(&ctx->queue_lock);
	if (!list_empty(&ctx->runqueue)) {
		next = get_first_item(&ctx->runqueue, struct sched_object, list);
	} else {
		next = &get_cpu_var(idle);
	}
	spinlock_release(&ctx->queue_lock);
	return next ;
}

struct acrn_scheduler sched_pin = {
	.name		= "sched_pin",
	.init		= sched_pin_init,
	.init_data	= sched_pin_init_data,
	.pick_next	= sched_pin_pick_next,
};

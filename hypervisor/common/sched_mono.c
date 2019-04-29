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

int sched_mono_init(struct sched_context *ctx)
{
	struct sched_mono_context *mono_ctx = &per_cpu(sched_mono_ctx, ctx->pcpu_id);
	ctx->priv = mono_ctx;
	return 0;
}

void sched_mono_insert(struct sched_object *obj)
{
	struct sched_mono_context *mono_ctx = (struct sched_mono_context *)obj->ctx->priv;

	mono_ctx->mono_sched_obj = obj;
}

static struct sched_object *sched_mono_pick_next(struct sched_context *ctx)
{
	struct sched_mono_context *mono_ctx = (struct sched_mono_context *)ctx->priv;
	struct sched_object *next = NULL;

	/* monopolist, if runqueue is null, then return idle */
	if (mono_ctx->mono_sched_obj != NULL) {
		next = mono_ctx->mono_sched_obj;
	} else {
		next = &get_cpu_var(idle);
	}
	return next;
}

static void sched_mono_sleep(struct sched_object *obj)
{
	struct sched_mono_context *mono_ctx = (struct sched_mono_context *)obj->ctx->priv;
	/* remove the sched_object as it will go to sleep */
	if (mono_ctx->mono_sched_obj == obj) {
		mono_ctx->mono_sched_obj = NULL;
	}
}

static void sched_mono_wake(struct sched_object *obj)
{
	struct sched_mono_context *mono_ctx = (struct sched_mono_context *)obj->ctx->priv;

	if (mono_ctx->mono_sched_obj == NULL) {
		mono_ctx->mono_sched_obj = obj;
	}
}

static void sched_mono_dump(__unused struct sched_context *ctx)
{
}

struct acrn_scheduler sched_mono = {
	.name		= "sched_mono",
	.init		= sched_mono_init,
	.insert		= sched_mono_insert,
	.pick_next	= sched_mono_pick_next,
	.sleep		= sched_mono_sleep,
	.wake		= sched_mono_wake,
	.dump		= sched_mono_dump,
};

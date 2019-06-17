/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <per_cpu.h>
#include <schedule.h>

static int sched_noop_init(struct sched_context *ctx)
{
	struct sched_noop_context *noop_ctx = &per_cpu(sched_noop_ctx, ctx->pcpu_id);
	ctx->priv = noop_ctx;

	return 0;
}

static void sched_noop_insert(struct sched_object *obj)
{
	struct sched_noop_context *noop_ctx = (struct sched_noop_context *)obj->ctx->priv;
	noop_ctx->noop_sched_obj = obj;
}

static void sched_noop_remove(struct sched_object *obj)
{
	struct sched_noop_context *noop_ctx = (struct sched_noop_context *)obj->ctx->priv;
	noop_ctx->noop_sched_obj = NULL;
}

static struct sched_object *sched_noop_pick_next(struct sched_context *ctx)
{
	struct sched_noop_context *noop_ctx = (struct sched_noop_context *)ctx->priv;
	struct sched_object *next = NULL;

	if (noop_ctx->noop_sched_obj != NULL) {
		next = noop_ctx->noop_sched_obj;
	} else {
		next = &get_cpu_var(idle);
	}
	return next;
}

static void sched_noop_sleep(struct sched_object *obj)
{
	struct sched_noop_context *noop_ctx = (struct sched_noop_context *)obj->ctx->priv;

	if (noop_ctx->noop_sched_obj == obj) {
		noop_ctx->noop_sched_obj = NULL;
	}
}

static void sched_noop_wake(struct sched_object *obj)
{
	struct sched_noop_context *noop_ctx = (struct sched_noop_context *)obj->ctx->priv;

	if (noop_ctx->noop_sched_obj == NULL) {
		noop_ctx->noop_sched_obj = obj;
	}
}

struct acrn_scheduler sched_noop = {
	.name		= "sched_noop",
	.init		= sched_noop_init,
	.insert		= sched_noop_insert,
	.remove		= sched_noop_remove,
	.pick_next	= sched_noop_pick_next,
	.sleep		= sched_noop_sleep,
	.wake		= sched_noop_wake,
};

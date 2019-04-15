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

int sched_pin_init(void)
{
	pr_fatal("%s\n", __func__);
	return 0;
}

int sched_pin_insert(struct sched_object *obj)
{
	pr_fatal("%s: obj->name = %s\n", __func__, obj->name);
	return 0;
}

int sched_pin_remove(struct sched_object *obj)
{
	pr_fatal("%s: obj->name = %s\n", __func__, obj->name);
	return 0;
}

uint32_t sched_pin_pick(struct sched_object *obj)
{
	pr_fatal("%s: obj->name = %s\n", __func__, obj->name);
	return 0;
}

int sched_pin_schedule(void)
{
	pr_fatal("%s\n", __func__);
	return 0;
}

int sched_pin_sleep(struct sched_object *obj)
{
	pr_fatal("%s: obj->name = %s\n", __func__, obj->name);
	return 0;
}

int sched_pin_wakeup(struct sched_object *obj)
{
	pr_fatal("%s: obj->name = %s\n", __func__, obj->name);
	return 0;
}

int sched_pin_yield(void)
{
	pr_fatal("%s\n", __func__);
	return 0;
}

struct acrn_scheduler sched_pin = {
	.name					=	"PIN scheduler",
	.need_timer				=	false,
	.init					=	sched_pin_init,
	.insert_sched_obj		=	sched_pin_insert,
	.remove_sched_obj		=	sched_pin_remove,
	.pick_next_sched_obj	=	sched_pin_pick,
	.schedule				=	sched_pin_schedule,
	.sleep					=	sched_pin_sleep,
	.wakeup					=	sched_pin_wakeup,
	.yield					=	sched_pin_yield,
};


/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <cpu.h>
#include <schedule.h>
#include <event.h>
#include <logmsg.h>
#include <trace.h>

void init_event(struct sched_event *event)
{
	spinlock_init(&event->lock);
	event->set = false;
	event->waiting_thread = NULL;
}

void reset_event(struct sched_event *event)
{
	uint64_t rflag;

	spinlock_irqsave_obtain(&event->lock, &rflag);
	event->set = false;
	event->waiting_thread = NULL;
	spinlock_irqrestore_release(&event->lock, rflag);
}

/* support exclusive waiting only */
void wait_event(struct sched_event *event)
{
	uint64_t rflag;

	spinlock_irqsave_obtain(&event->lock, &rflag);
	TRACE_4I(TRACE_WAIT_EVENT, event->vm_id, event->vcpu_id, event->type, event->set);
	ASSERT((event->waiting_thread == NULL), "only support exclusive waiting");
	event->waiting_thread = sched_get_current(get_pcpu_id());
	while (!event->set && (event->waiting_thread != NULL)) {
		sleep_thread(event->waiting_thread);
		spinlock_irqrestore_release(&event->lock, rflag);
		schedule();
		spinlock_irqsave_obtain(&event->lock, &rflag);
	}
	event->set = false;
	event->waiting_thread = NULL;
	spinlock_irqrestore_release(&event->lock, rflag);
}

void signal_event(struct sched_event *event)
{
	uint64_t rflag;

	spinlock_irqsave_obtain(&event->lock, &rflag);
	TRACE_4I(TRACE_SIGNAL_EVENT, event->vm_id, event->vcpu_id, event->type, 0);
	event->set = true;
	if (event->waiting_thread != NULL) {
		wake_thread(event->waiting_thread);
	}
	spinlock_irqrestore_release(&event->lock, rflag);
}

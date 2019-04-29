/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vm_config.h>
#include <bits.h>
#include <errno.h>
#include <schedule.h>
#include <logmsg.h>
#include <cat.h>

/*
 * @pre vm_id < CONFIG_MAX_VM_NUM
 * @post return != NULL 
 */
struct acrn_vm_config *get_vm_config(uint16_t vm_id)
{
	return &vm_configs[vm_id];
}

static inline bool uuid_is_equal(const uint8_t *uuid1, const uint8_t *uuid2)
{
	uint64_t uuid1_h = *(uint64_t *)uuid1;
	uint64_t uuid1_l = *(uint64_t *)(uuid1 + 8);
	uint64_t uuid2_h = *(uint64_t *)uuid2;
	uint64_t uuid2_l = *(uint64_t *)(uuid2 + 8);

	return ((uuid1_h == uuid2_h) && (uuid1_l == uuid2_l));
}

/**
 * return true if the input uuid is configured in VM
 *
 * @pre vmid < CONFIG_MAX_VM_NUM
 */
bool vm_has_matched_uuid(uint16_t vmid, const uint8_t *uuid)
{
	struct acrn_vm_config *vm_config = get_vm_config(vmid);

	return (uuid_is_equal(&vm_config->uuid[0], uuid));
}

/**
 * return true if no UUID collision is found in vm configs array start from vm_configs[vm_id]
 *
 * @pre vm_id < CONFIG_MAX_VM_NUM
 */
static bool check_vm_uuid_collision(uint16_t vm_id)
{
	uint16_t i;
	bool ret = true;
	struct acrn_vm_config *start_config = get_vm_config(vm_id);
	struct acrn_vm_config *following_config;

	for (i = vm_id + 1U; i < CONFIG_MAX_VM_NUM; i++) {
		following_config = get_vm_config(i);
		if (uuid_is_equal(&start_config->uuid[0], &following_config->uuid[0])) {
			ret = false;
			break;
		}
	}
	return ret;
}

static int32_t init_pcpu_schedulers(struct acrn_vm_config *vm_config)
{
	int32_t ret = 0;
	uint16_t pcpu_id;
	struct acrn_scheduler *scheduler;
	uint64_t pcpu_bitmap = vm_config->pcpu_bitmap;

	/* verify & set scheduler for all pcpu of this VM */
	pcpu_id = ffs64(pcpu_bitmap);
	while (pcpu_id != INVALID_BIT_INDEX) {
		scheduler = get_scheduler(pcpu_id);
		if (scheduler && scheduler != find_scheduler_by_name(vm_config->scheduler)) {
			pr_err("%s: detect scheduler conflict on pcpu%d\n", __func__, pcpu_id);
			ret = -EINVAL;
			break;
		}
		scheduler = find_scheduler_by_name(vm_config->scheduler);
		if (!scheduler) {
			pr_err("%s: No valid scheduler found for pcpu%d\n", __func__, pcpu_id);
			ret = -EINVAL;
			break;
		}
		set_scheduler(pcpu_id, scheduler);
		bitmap_clear_nolock(pcpu_id, &pcpu_bitmap);
		pcpu_id = ffs64(pcpu_bitmap);
	}

	return ret;
}

/**
 * @pre vm_config != NULL
 */
bool sanitize_vm_config(void)
{
	bool ret = true;
	uint16_t vm_id, vcpu_id, pcpu_num;
	uint64_t sos_pcpu_bitmap, pre_launch_pcpu_bitmap, affinity = 0UL;
	struct acrn_vm_config *vm_config;

	sos_pcpu_bitmap = (uint64_t)((((uint64_t)1U) << get_pcpu_nums()) - 1U);
	pre_launch_pcpu_bitmap = 0U;
	/* All physical CPUs except ocuppied by Pre-launched VMs are all
	 * belong to SOS_VM. i.e. The pcpu_bitmap of a SOS_VM is decided
	 * by pcpu_bitmap status in PRE_LAUNCHED_VMs.
	 * We need to setup a rule, that the vm_configs[] array should follow
	 * the order of PRE_LAUNCHED_VM first, and then SOS_VM.
	 */
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		vm_config = get_vm_config(vm_id);
		switch (vm_config->load_order) {
		case PRE_LAUNCHED_VM:
			if (vm_config->pcpu_bitmap == 0U) {
				ret = false;
			/* GUEST_FLAG_RT must be set if we have GUEST_FLAG_LAPIC_PASSTHROUGH set in guest_flags */
			} else if (((vm_config->guest_flags & GUEST_FLAG_LAPIC_PASSTHROUGH) != 0U)
					&& ((vm_config->guest_flags & GUEST_FLAG_RT) == 0U)) {
				ret = false;
			} else if (vm_config->mptable == NULL) {
				ret = false;
			} else {
				pre_launch_pcpu_bitmap |= vm_config->pcpu_bitmap;
			}
			break;
		case SOS_VM:
			/* Deduct pcpus of PRE_LAUNCHED_VMs */
			sos_pcpu_bitmap ^= pre_launch_pcpu_bitmap;
			if ((sos_pcpu_bitmap == 0U) || ((vm_config->guest_flags & GUEST_FLAG_LAPIC_PASSTHROUGH) != 0U)) {
				ret = false;
			} else {
				vm_config->pcpu_bitmap = sos_pcpu_bitmap;
			}
			break;
		case POST_LAUNCHED_VM:
			if (vm_config->pcpu_bitmap == 0U || (vm_config->pcpu_bitmap & pre_launch_pcpu_bitmap) != 0U) {
				pr_err("%s: Post-launch VM has no pcpus or share pcpu with Pre-launch VM!", __func__);
				ret = false;
			}
			pcpu_num = bitmap_weight(vm_config->pcpu_bitmap);
			if (pcpu_num < vm_config->cpu_num) {
				pr_err("%s: One VM cannot have multi vcpus share one pcpu!", __func__);
				ret = false;
			}
			for (vcpu_id = 0; vcpu_id < vm_config->cpu_num; vcpu_id++) {
				if (bitmap_weight(vm_config->vcpu_sched_affinity[vcpu_id]) > 1) {
					pr_err("%s: vm%u vcpu%u should have only one prefer affinity pcpu!", __func__, vm_id, vcpu_id);
					ret = false;
				}
				affinity |= vm_config->vcpu_sched_affinity[vcpu_id];
			}
			if (bitmap_weight(affinity) != vm_config->cpu_num) {
				pr_err("%s: One VM cannot have multi vcpus share one pcpu!", __func__);
				ret = false;
			}
			break;
		default:
			/* Nothing to do for a UNDEFINED_VM, break directly. */
			break;
		}

		ret |= init_pcpu_schedulers(vm_config);

		if ((vm_config->guest_flags & GUEST_FLAG_CLOS_REQUIRED) != 0U) {
			if (cat_cap_info.support && (vm_config->clos <= cat_cap_info.clos_max)) {
					cat_cap_info.enabled = true;
			} else {
				pr_err("%s set wrong CLOS or CAT is not supported\n", __func__);
				ret = false;
			}
		}

		if (ret) {
			/* make sure no identical UUID in following VM configurations */
			ret = check_vm_uuid_collision(vm_id);
		}
		if (!ret) {
			break;
		}
	}
	return ret;
}

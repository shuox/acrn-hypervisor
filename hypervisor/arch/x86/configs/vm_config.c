/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vm_config.h>
#include <cpu.h>
#include <errno.h>
#include <acrn_common.h>
#include <page.h>
#include <logmsg.h>
#include <cat.h>
#include <vm_configurations.h>

#define INIT_VM_CONFIG(idx)	\
	{		\
		.type = VM##idx##_CONFIG_TYPE,	\
		.name = VM##idx##_CONFIG_NAME,	\
		.pcpu_bitmap = VM##idx##_CONFIG_PCPU_BITMAP,	\
		.guest_flags = VM##idx##_CONFIG_FLAGS,	\
		.clos = VM##idx##_CONFIG_CLOS,	\
		.memory = {	\
			.start_hpa = VM##idx##_CONFIG_MEM_START_HPA,	\
			.size = VM##idx##_CONFIG_MEM_SIZE,	\
		},	\
		.os_config = {	\
			.name = VM##idx##_CONFIG_OS_NAME,	\
			.bootargs = VM##idx##_CONFIG_OS_BOOTARGS,	\
		},	\
		.vm_vuart = true,	\
		.pci_ptdev_num = VM##idx##_CONFIG_PCI_PTDEV_NUM,	\
		.pci_ptdevs = vm##idx##_pci_ptdevs,	\
	}

static struct acrn_vm_config vm_configs[CONFIG_MAX_VM_NUM] __aligned(PAGE_SIZE) = {
#ifdef VM0_CONFIGURED
	INIT_VM_CONFIG(0),
#endif

	{
		.type = NORMAL_VM,
		.name = "ACRN GUEST VM",
		.pcpu_bitmap = 0xfUL,
		.GUID = {0xd2, 0x79, 0x54, 0x38, 0x25, 0xd6, 0x11, 0xe8, 0x86, 0x4e, 0xcb, 0x7a, 0x18, 0xb3, 0x46, 0x43},
		.vcpu_num = 4,
		.vcpu_sched_affinity = {1<<0, 1<<1, 1<<2, 1<<3,},
	},

#ifdef VM2_CONFIGURED
	INIT_VM_CONFIG(2),
#endif

#ifdef VM3_CONFIGURED
	INIT_VM_CONFIG(3),
#endif
};

/*
 * @pre vm_id < CONFIG_MAX_VM_NUM
 * @post return != NULL 
 */
struct acrn_vm_config *get_vm_config(uint16_t vm_id)
{
	return &vm_configs[vm_id];
}

struct acrn_vm_config *get_vm_config_by_uuid(uint8_t *guid, uint16_t *vm_id)
{
	unsigned int i, j;

	for (i = 0; i < CONFIG_MAX_VM_NUM; i++) {
		for (j = 0; j < 16; j++) {
			if (vm_configs[i].GUID[j] != guid[j])
				break;
		}
		if (j == 16) {
			if (vm_id)
				*vm_id = i;
			return &vm_configs[i];
		}
	}
	return NULL;
}

/**
 * @pre vm_config != NULL
 */
int32_t sanitize_vm_config(void)
{
	int32_t ret = 0;
	uint16_t vm_id;
	uint64_t sos_pcpu_bitmap, pre_launch_pcpu_bitmap;
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
		switch (vm_config->type) {
		case PRE_LAUNCHED_VM:
			if (vm_config->pcpu_bitmap == 0U) {
				ret = -EINVAL;
			/* GUEST_FLAG_RT must be set if we have GUEST_FLAG_LAPIC_PASSTHROUGH set in guest_flags */
			} else if (((vm_config->guest_flags & GUEST_FLAG_LAPIC_PASSTHROUGH) != 0U)
					&& ((vm_config->guest_flags & GUEST_FLAG_RT) == 0U)) {
				ret = -EINVAL;
			} else {
				pre_launch_pcpu_bitmap |= vm_config->pcpu_bitmap;
			}
			break;
		case SOS_VM:
			/* Deduct pcpus of PRE_LAUNCHED_VMs */
			sos_pcpu_bitmap ^= pre_launch_pcpu_bitmap;
			if ((sos_pcpu_bitmap == 0U) || ((vm_config->guest_flags & GUEST_FLAG_LAPIC_PASSTHROUGH) != 0U)) {
				ret = -EINVAL;
			} else {
				vm_config->pcpu_bitmap = sos_pcpu_bitmap;
			}
			break;
		case NORMAL_VM:
			/* NORMAL_VM will be post launched by acrn-dm */
			break;
		default:
			/* Nothing to do for a UNDEFINED_VM, break directly. */
			break;
		}

		if ((vm_config->guest_flags & GUEST_FLAG_CLOS_REQUIRED) != 0U) {
			if (cat_cap_info.support && (vm_config->clos <= cat_cap_info.clos_max)) {
					cat_cap_info.enabled = true;
			} else {
				pr_err("%s set wrong CLOS or CAT is not supported\n", __func__);
				ret = -EINVAL;
			}
		}

		if (ret != 0) {
			break;
		}
	}
	return ret;
}

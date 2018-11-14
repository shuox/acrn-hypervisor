/*-
 * Copyright (c) 2018 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "dm.h"
#include "vmmapi.h"
#include "sw_load.h"


/*                 ovmf binary layout:
 *
 * +--------------------------------------------------+ <--OVMF Top
 * |             |offset: Top - 0x10 (reset vector)   |
 * + SECFV       |------------------------------------+
 * |             |other                               |
 * +--------------------------------------------------+
 * |                                                  |
 * + FVMAIN_COMPACT                                   +
 * |                                                  |
 * +--------------------------------------------------+
 * |                                                  |
 * + NV data storage                                  +
 * |                                                  |
 * +--------------------------------------------------+
 */

/* ovmf real entry is reset vector, which is (OVMF_TOP - 16) */
#define	OVMF_TOP(ctx)		(4*GB)

static char ovmf_path[STR_LEN];
static int ovmf_size;

extern int init_cmos_vrpmb(struct vmctx *ctx);

int
acrn_parse_ovmf(char *arg)
{
	size_t len = strlen(arg);

	if (len < STR_LEN) {
		strncpy(ovmf_path, arg, len + 1);
		assert(check_image(ovmf_path) == 0);

		ovmf_file_name = ovmf_path;
		printf("SW_LOAD: get ovmf path %s\n", ovmf_path);
		return 0;
	} else
		return -1;
}

static int
acrn_prepare_ovmf(struct vmctx *ctx)
{
	FILE *fp;
	int len, read;

	fp = fopen(ovmf_path, "r");
	if (fp == NULL) {
		fprintf(stderr,
			"SW_LOAD ERR: could not open ovmf file: %s\n",
			ovmf_path);
		return -1;
	}

	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	if (len > (2*MB)) {
		fprintf(stderr,
			"SW_LOAD ERR: too large ovmf file\n");
		fclose(fp);
		return -1;
	}

	ovmf_size = len;

	fseek(fp, 0, SEEK_SET);
	read = fread(ctx->baseaddr + OVMF_TOP(ctx) - ovmf_size,
		sizeof(char), len, fp);
	if (read < len) {
		fprintf(stderr,
			"SW_LOAD ERR: could not read whole partition blob\n");
		fclose(fp);
		return -1;
	}
	fclose(fp);
	printf("SW_LOAD: partition blob %s size %d copy to guest 0x%lx\n",
		ovmf_path, ovmf_size, OVMF_TOP(ctx) - ovmf_size);

	return 0;
}

int
acrn_sw_load_ovmf(struct vmctx *ctx)
{
	int ret;

	init_cmos_vrpmb(ctx);

	ret = acrn_prepare_ovmf(ctx);

	if (ret)
		return ret;

	printf("SW_LOAD: ovmf_entry 0x%lx\n", OVMF_TOP(ctx) - 16);

	/* set guest bsp state. Will call hypercall set bsp state
	 * after bsp is created.
	 */
	memset(&ctx->bsp_regs, 0, sizeof(struct acrn_set_vcpu_regs));
	ctx->bsp_regs.vcpu_id = 0;

	/* CR0_ET | CR0_NE */
	ctx->bsp_regs.vcpu_regs.cr0 = 0x30U;
	ctx->bsp_regs.vcpu_regs.cs_ar = 0x009FU;
	ctx->bsp_regs.vcpu_regs.cs_sel = 0xF000U;
	ctx->bsp_regs.vcpu_regs.cs_limit = 0xFFFFU;
	ctx->bsp_regs.vcpu_regs.cs_base = (OVMF_TOP(ctx) - 16) & 0xFFFF0000UL;
	ctx->bsp_regs.vcpu_regs.rip = (OVMF_TOP(ctx) - 16) & 0xFFFFUL;

	return 0;
}

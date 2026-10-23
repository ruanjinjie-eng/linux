// SPDX-License-Identifier: GPL-2.0
/*
 * kexec_file for arm64
 *
 * Copyright (C) 2018 Linaro Limited
 * Author: AKASHI Takahiro <takahiro.akashi@linaro.org>
 *
 * Most code is derived from arm64 port of kexec-tools
 */

#define pr_fmt(fmt) "kexec_file: " fmt

#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/kexec.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

const struct kexec_file_ops * const kexec_file_loaders[] = {
	&kexec_image_ops,
	NULL
};

int arch_kimage_file_post_load_cleanup(struct kimage *image)
{
	kvfree(image->arch.dtb);
	image->arch.dtb = NULL;

	return kexec_image_post_load_cleanup_default(image);
}

#ifdef CONFIG_CRASH_DUMP
unsigned int arch_get_system_nr_ranges(void)
{
	unsigned int nr_ranges = 2 + crashk_cma_cnt; /* for exclusion of crashkernel region */
	phys_addr_t start, end;
	u64 i;

	for_each_mem_range(i, &start, &end)
		nr_ranges++;

	return nr_ranges;
}

int arch_crash_populate_cmem(struct crash_mem *cmem)
{
	phys_addr_t start, end;
	u64 i;

	for_each_mem_range(i, &start, &end) {
		cmem->ranges[cmem->nr_ranges].start = start;
		cmem->ranges[cmem->nr_ranges].end = end - 1;
		cmem->nr_ranges++;
	}

	return 0;
}
#endif

int arch_kexec_setup_other_segments(struct kimage *image,
				    struct kexec_load_params *params,
				    unsigned long initrd_load_addr)
{
	struct kexec_buf kbuf = {
		.image = image,
		.buf_min = params->kernel_load_addr + params->kernel_size,
	};
	unsigned long dtb_len;
	void *dtb;
	int ret;

	if (image->type == KEXEC_TYPE_CRASH) {
		ret = crash_load_dm_crypt_keys(image);
		if (ret)
			return ret;
	}

	/* load dtb */
	dtb = of_kexec_alloc_and_setup_fdt(image, initrd_load_addr,
					   params->initrd_len, params->cmdline, 0);
	if (!dtb) {
		pr_err("Preparing for new dtb failed\n");
		return -EINVAL;
	}

	/* trim it */
	fdt_pack(dtb);
	dtb_len = fdt_totalsize(dtb);
	kbuf.buffer = dtb;
	kbuf.bufsz = dtb_len;
	kbuf.mem = KEXEC_BUF_MEM_UNKNOWN;
	kbuf.memsz = dtb_len;
	/* not across 2MB boundary */
	kbuf.buf_align = SZ_2M;
	kbuf.buf_max = ULONG_MAX;
	kbuf.top_down = true;

	ret = kexec_add_buffer(&kbuf);
	if (ret) {
		kvfree(dtb);
		return ret;
	}
	image->arch.dtb = dtb;
	image->arch.dtb_mem = kbuf.mem;

	kexec_dprintk("Loaded dtb at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
		      kbuf.mem, kbuf.bufsz, kbuf.memsz);

	return 0;
}

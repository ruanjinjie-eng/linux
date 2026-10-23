// SPDX-License-Identifier: GPL-2.0
/*
 * kexec_file for LoongArch
 *
 * Author: Youling Tang <tangyouling@kylinos.cn>
 * Copyright (C) 2025 KylinSoft Corporation.
 *
 * Most code is derived from LoongArch port of kexec-tools
 */

#define pr_fmt(fmt) "kexec_file: " fmt

#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/kexec.h>
#include <linux/memblock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <asm/bootinfo.h>

const struct kexec_file_ops * const kexec_file_loaders[] = {
	&kexec_efi_ops,
	&kexec_elf_ops,
	NULL
};

int arch_kimage_file_post_load_cleanup(struct kimage *image)
{
	return kexec_image_post_load_cleanup_default(image);
}

/* Add the "kexec_file" command line parameter to command line. */
static void cmdline_add_loader(unsigned long *cmdline_tmplen, char *modified_cmdline)
{
	int loader_strlen;

	loader_strlen = sprintf(modified_cmdline + (*cmdline_tmplen), "kexec_file ");
	*cmdline_tmplen += loader_strlen;
}

/* Add the "initrd=start,size" command line parameter to command line. */
static void cmdline_add_initrd(struct kimage *image, unsigned long *cmdline_tmplen,
				char *modified_cmdline, unsigned long initrd)
{
	int initrd_strlen;

	initrd_strlen = sprintf(modified_cmdline + (*cmdline_tmplen), "initrd=0x%lx,0x%lx ",
		initrd, image->initrd_buf_len);
	*cmdline_tmplen += initrd_strlen;
}

#ifdef CONFIG_CRASH_DUMP
unsigned int arch_get_system_nr_ranges(void)
{
	int nr_ranges = 2; /* for exclusion of crashkernel region */
	phys_addr_t start, end;
	uint64_t i;

	for_each_mem_range(i, &start, &end)
		nr_ranges++;

	return nr_ranges;
}

int arch_crash_populate_cmem(struct crash_mem *cmem)
{
	phys_addr_t start, end;
	uint64_t i;

	for_each_mem_range(i, &start, &end) {
		cmem->ranges[cmem->nr_ranges].start = start;
		cmem->ranges[cmem->nr_ranges].end = end - 1;
		cmem->nr_ranges++;
	}

	return 0;
}

/*
 * Add the "mem=size@start" command line parameter to command line, indicating the
 * memory region the new kernel can use to boot into.
 */
static void cmdline_add_mem(unsigned long *cmdline_tmplen, char *modified_cmdline)
{
	int mem_strlen = 0;

	mem_strlen = sprintf(modified_cmdline + (*cmdline_tmplen), "mem=0x%llx@0x%llx ",
		crashk_res.end - crashk_res.start + 1, crashk_res.start);
	*cmdline_tmplen += mem_strlen;

	if (crashk_low_res.end) {
		mem_strlen = sprintf(modified_cmdline + (*cmdline_tmplen), "mem=0x%llx@0x%llx ",
			crashk_low_res.end - crashk_low_res.start + 1, crashk_low_res.start);
		*cmdline_tmplen += mem_strlen;
	}
}

/* Add the "elfcorehdr=size@start" command line parameter to command line. */
static void cmdline_add_elfcorehdr(struct kimage *image, unsigned long *cmdline_tmplen,
				   char *modified_cmdline, unsigned long elfcorehdr_sz)
{
	int elfcorehdr_strlen = 0;

	elfcorehdr_strlen = sprintf(modified_cmdline + (*cmdline_tmplen), "elfcorehdr=0x%lx@0x%lx ",
		elfcorehdr_sz, image->elf_load_addr);
	*cmdline_tmplen += elfcorehdr_strlen;
}

#endif

int arch_kexec_setup_other_segments(struct kimage *image,
				    struct kexec_load_params *params,
				    unsigned long initrd_load_addr)
{
	unsigned long cmdline_tmplen = 0;
	unsigned long cmdline_len = params->cmdline_len;
	const char *cmdline = params->cmdline;
	char *modified_cmdline;

	modified_cmdline = kzalloc(COMMAND_LINE_SIZE, GFP_KERNEL);
	if (!modified_cmdline)
		return -ENOMEM;

	cmdline_add_loader(&cmdline_tmplen, modified_cmdline);
	/* Ensure it's null terminated */
	modified_cmdline[COMMAND_LINE_SIZE - 1] = '\0';

#ifdef CONFIG_CRASH_DUMP
	if (image->type == KEXEC_TYPE_CRASH) {
		/* Add the mem=size@start parameter to the command line */
		cmdline_add_mem(&cmdline_tmplen, modified_cmdline);

		/* Add the elfcorehdr=size@start parameter to the command line */
		cmdline_add_elfcorehdr(image, &cmdline_tmplen, modified_cmdline,
				       image->elf_headers_sz);
	}
#endif

	/* Add the initrd=start,size parameter to the command line */
	if (initrd_load_addr)
		cmdline_add_initrd(image, &cmdline_tmplen, modified_cmdline, initrd_load_addr);

	if (cmdline_len + cmdline_tmplen > COMMAND_LINE_SIZE) {
		pr_err("Appending command line exceeds COMMAND_LINE_SIZE\n");
		kfree(modified_cmdline);
		return -EINVAL;
	}

	memcpy(modified_cmdline + cmdline_tmplen, cmdline, cmdline_len);
	image->arch.cmdline_ptr = (unsigned long)modified_cmdline;

	return 0;
}

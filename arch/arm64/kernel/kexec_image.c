// SPDX-License-Identifier: GPL-2.0
/*
 * Kexec image loader

 * Copyright (C) 2018 Linaro Limited
 * Author: AKASHI Takahiro <takahiro.akashi@linaro.org>
 */

#define pr_fmt(fmt)	"kexec_file(Image): " fmt

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kexec.h>
#include <linux/pe.h>
#include <linux/string.h>
#include <asm/byteorder.h>
#include <asm/cpufeature.h>
#include <asm/image.h>
#include <asm/memory.h>

static int image_probe(const char *kernel_buf, unsigned long kernel_len)
{
	const struct arm64_image_header *h =
		(const struct arm64_image_header *)(kernel_buf);

	if (!h || (kernel_len < sizeof(*h)))
		return -EINVAL;

	if (memcmp(&h->magic, ARM64_IMAGE_MAGIC, sizeof(h->magic)))
		return -EINVAL;

	return 0;
}

static void *image_load(struct kimage *image,
				char *kernel, unsigned long kernel_len,
				char *initrd, unsigned long initrd_len,
				char *cmdline, unsigned long cmdline_len)
{
	struct arm64_image_header *h;
	u64 flags, value;
	bool be_image;
	struct kexec_buf kbuf = {};
	unsigned long text_offset, kernel_segment_number;
	struct kexec_segment *kernel_segment;
	struct kexec_load_params params = {
		.initrd = initrd,
		.initrd_len = initrd_len,
		.cmdline = cmdline,
		.cmdline_len = cmdline_len,
	};
	int ret;

	/*
	 * We require a kernel with an unambiguous Image header. Per
	 * Documentation/arch/arm64/booting.rst, this is the case when image_size
	 * is non-zero (practically speaking, since v3.17).
	 */
	h = (struct arm64_image_header *)kernel;
	if (!h->image_size)
		return ERR_PTR(-EINVAL);

	/* Check cpu features */
	flags = le64_to_cpu(h->flags);
	be_image = arm64_image_flag_field(flags, ARM64_IMAGE_FLAG_BE);
	if (be_image && !system_supports_mixed_endian())
		return ERR_PTR(-EINVAL);

	value = arm64_image_flag_field(flags, ARM64_IMAGE_FLAG_PAGE_SIZE);
	if (((value == ARM64_IMAGE_FLAG_PAGE_SIZE_4K) &&
			!system_supports_4kb_granule()) ||
	    ((value == ARM64_IMAGE_FLAG_PAGE_SIZE_64K) &&
			!system_supports_64kb_granule()) ||
	    ((value == ARM64_IMAGE_FLAG_PAGE_SIZE_16K) &&
			!system_supports_16kb_granule()))
		return ERR_PTR(-EINVAL);

	/* Load the kernel */
	kbuf.image = image;
	kbuf.buf_min = 0;
	kbuf.buf_max = ULONG_MAX;
	kbuf.top_down = false;

	kbuf.buffer = kernel;
	kbuf.bufsz = kernel_len;
	kbuf.mem = KEXEC_BUF_MEM_UNKNOWN;
	kbuf.memsz = le64_to_cpu(h->image_size);
	/*
	 * Pre-v5.8 images carry a non-zero text_offset, which is no longer
	 * honored; reject them instead of loading them at a wrong address.
	 */
	text_offset = le64_to_cpu(h->text_offset);
	if (text_offset) {
		pr_err("Unsupported Image text_offset 0x%lx\n", text_offset);
		return ERR_PTR(-ENOEXEC);
	}
	kbuf.buf_align = MIN_KIMG_ALIGN;

	kernel_segment_number = image->nr_segments;

	/*
	 * The location of the kernel segment may make it impossible to satisfy
	 * the other segment requirements, so we try repeatedly to find a
	 * location that will work.
	 */
	while ((ret = kexec_add_buffer(&kbuf)) == 0) {
		/* Try to load additional data */
		kernel_segment = &image->segment[kernel_segment_number];
		params.kernel_load_addr = kernel_segment->mem;
		params.kernel_size = kernel_segment->memsz;
		ret = kexec_load_other_segments(image, &params, false);
		if (!ret)
			break;

		/*
		 * We couldn't find space for the other segments; erase the
		 * kernel segment and try the next available hole.
		 */
		/*
		 * kexec_alloc_contig() ignores buf_min and always returns
		 * CMA memory, so disable it.  A CMA segment was not found by
		 * the RAM walk: keep buf_min so that the walk can still use
		 * the memory below the CMA region.
		 */
		if (image->segment_cma[kernel_segment_number])
			image->no_cma = true;
		else
			kbuf.buf_min = kernel_segment->mem + kernel_segment->memsz;

		kexec_free_segment_cma(image, kernel_segment_number);
		image->nr_segments -= 1;
		kbuf.mem = KEXEC_BUF_MEM_UNKNOWN;
	}

	if (ret) {
		pr_err("Could not find any suitable kernel location!");
		return ERR_PTR(ret);
	}

	kernel_segment = &image->segment[kernel_segment_number];
	image->start = kernel_segment->mem;

	kexec_dprintk("Loaded kernel at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
		      kernel_segment->mem, kbuf.bufsz,
		      kernel_segment->memsz);

	return NULL;
}

const struct kexec_file_ops kexec_image_ops = {
	.probe = image_probe,
	.load = image_load,
#ifdef CONFIG_KEXEC_IMAGE_VERIFY_SIG
	.verify_sig = kexec_kernel_verify_pe_sig,
#endif
};

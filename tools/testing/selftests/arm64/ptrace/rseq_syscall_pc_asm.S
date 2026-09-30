/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Issue a system call from inside and from outside an rseq critical
 * section, with labels the test uses to check and rewrite the tracee PC.
 *
 * x0:	&rseq->rseq_cs
 * x1:	struct rseq_cs describing [cs_start, cs_end)
 */

#include <asm/unistd.h>

#include "rseq_syscall_pc.h"

	.text

	.globl	call_in_cs
call_in_cs:
	str	x1, [x0]
	.globl	cs_start
cs_start:
	mov	x8, #__NR_getppid
	svc	#0
	.globl	cs_after_svc
cs_after_svc:
	nop
	.globl	cs_end
cs_end:
	mov	x0, #0
	ret

	.inst	RSEQ_SIG_INSN
	.globl	cs_abort
cs_abort:
	mov	x0, #42
	ret

	.globl	call_out_of_cs
call_out_of_cs:
	str	x1, [x0]
	mov	x8, #__NR_getppid
	svc	#0
	.globl	out_after_svc
out_after_svc:
	mov	x0, #0
	ret

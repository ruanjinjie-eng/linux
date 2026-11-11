/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RSEQ_SYSCALL_PC_H
#define RSEQ_SYSCALL_PC_H

/* BRK #0x45E0, the arm64 RSEQ_SIG */
#define RSEQ_SIG_INSN		0xd428bc00

#ifndef __ASSEMBLER__
/*
 * Both functions store @cs into @slot (rseq->rseq_cs) and then issue
 * getppid(). call_in_cs() does that inside the critical section
 * [cs_start, cs_end), call_out_of_cs() outside it. Both return 0, or 42
 * if the critical section was aborted.
 */
long call_in_cs(void *slot, void *cs);
long call_out_of_cs(void *slot, void *cs);

extern char cs_start[], cs_after_svc[], cs_end[], cs_abort[];
extern char out_after_svc[];
#endif

#endif /* RSEQ_SYSCALL_PC_H */

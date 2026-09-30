// SPDX-License-Identifier: GPL-2.0
/*
 * The rseq debug check on syscall exit must look at the PC the system
 * call was issued from, not at a PC written by a tracer at the
 * syscall-exit-stop.
 *
 * With rseq debugging enabled (CONFIG_DEBUG_RSEQ, or the rseq_debug= boot
 * option and /sys/kernel/debug/rseq/debug with generic entry), a system
 * call issued inside an rseq critical section kills the task with
 * SIGSEGV. arm64 used to do this check after reporting the
 * syscall-exit-stop, so a tracer could:
 *  - hide a system call issued inside the critical section by moving the
 *    PC out of it, and
 *  - get a task killed for a system call issued outside the critical
 *    section by moving the PC into it.
 *
 * The test is skipped if rseq debugging is not enabled.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/rseq.h>

#include <linux/compiler.h>

#include "kselftest.h"
#include "ptrace_common.h"

#include "rseq_syscall_pc.h"

/* struct rseq_cs must be 32-byte aligned (see linux/rseq.h). */
static struct rseq_cs cs __aligned(32);

static void *rseq_cs_slot(void)
{
	struct rseq *rs = (struct rseq *)((char *)__builtin_thread_pointer() +
					  __rseq_offset);

	return &rs->rseq_cs;
}

/*
 * Run the system call in the child. If @ret_pc is set, trace it with
 * PTRACE_SYSCALL and write @new_pc at the syscall-exit-stop whose PC is
 * @ret_pc. Returns the wait status of the child's exit.
 */
static int run(bool in_cs, unsigned long ret_pc, unsigned long new_pc)
{
	int status, i, sig = 0;
	pid_t pid;

	pid = fork();
	if (pid == 0) {
		if (ret_pc) {
			if (traceme() < 0)
				_exit(1);
			raise(SIGSTOP);
		}
		_exit(in_cs ? call_in_cs(rseq_cs_slot(), &cs) :
			      call_out_of_cs(rseq_cs_slot(), &cs));
	}
	if (pid < 0)
		return -1;
	if (!ret_pc) {
		waitpid(pid, &status, 0);
		return status;
	}

	if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status))
		return -1;
	ptrace(PTRACE_SETOPTIONS, pid, 0,
	       (void *)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL));
	for (i = 0; i < 10000; i++) {
		if (resume_and_wait(pid, PTRACE_SYSCALL, sig, &status) < 0)
			break;
		sig = 0;
		if (WSTOPSIG(status) != (SIGTRAP | 0x80)) {
			sig = WSTOPSIG(status);	/* pass real signals on */
			continue;
		}
		if (syscall_info(pid, NULL) == PTRACE_SYSCALL_INFO_EXIT &&
		    tracee_pc(pid) == ret_pc) {
			struct user_pt_regs regs;

			if (tracee_get_regs(pid, &regs) == 0) {
				regs.pc = new_pc;
				tracee_set_regs(pid, &regs);
			}
		}
	}
	return status;
}

static bool killed_by_segv(int status)
{
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

int main(void)
{
	int status;

	ksft_print_header();
	ksft_set_plan(2);

	if (__rseq_size == 0)
		ksft_exit_skip("rseq is not registered by libc\n");

	cs.version = 0;
	cs.flags = 0;
	cs.start_ip = (uintptr_t)cs_start;
	cs.post_commit_offset = (uintptr_t)cs_end - (uintptr_t)cs_start;
	cs.abort_ip = (uintptr_t)cs_abort;

	/* An untraced system call inside the critical section must be fatal. */
	status = run(true, 0, 0);
	if (!killed_by_segv(status))
		ksft_exit_skip("rseq debugging is not enabled\n");

	status = run(true, (uintptr_t)cs_after_svc, (uintptr_t)cs_end);
	ksft_test_result(killed_by_segv(status),
			 "syscall in critical section is caught after the tracer moves the PC out\n");

	status = run(false, (uintptr_t)out_after_svc, (uintptr_t)cs_after_svc);
	/* exit 42 means the critical section was aborted, which is also fine */
	ksft_test_result(WIFEXITED(status),
			 "syscall outside critical section survives the tracer moving the PC in\n");

	ksft_finished();
}

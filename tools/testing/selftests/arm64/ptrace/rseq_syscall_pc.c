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
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <linux/compiler.h>
#include <linux/rseq.h>

#include "kselftest.h"
#include "ptrace_common.h"

#include "rseq_syscall_pc.h"

/* struct rseq_cs must be 32-byte aligned (see linux/rseq.h). */
static struct rseq_cs cs __aligned(32);

/*
 * rseq is registered by libc (glibc >= 2.35) in every thread, including
 * the forked children the system calls run in, and libc exports the
 * offset of its rseq area. Declare the two symbols weak, as
 * tools/testing/selftests/rseq/rseq.c does, so the test also builds
 * against libcs without rseq support and skips there.
 */
extern __weak ptrdiff_t __rseq_offset;
extern __weak unsigned int __rseq_size;

static const ptrdiff_t *libc_rseq_offset_p = &__rseq_offset;
static const unsigned int *libc_rseq_size_p = &__rseq_size;

/*
 * Offset from the thread pointer to the rseq area libc registered,
 * found in main() before forking the children.
 */
static ptrdiff_t rseq_offset;

static void *rseq_cs_slot(void)
{
	struct rseq *rs = (struct rseq *)((char *)__builtin_thread_pointer() +
					  rseq_offset);

	return &rs->rseq_cs;
}

/*
 * Run getppid() inside (@in_cs) or outside the critical section. The
 * kernel aborts the critical section, and call_in_cs() returns 42, if
 * the task is preempted before the system call runs; start over so it
 * is actually issued.
 */
static long run_syscall(bool in_cs)
{
	void *slot = rseq_cs_slot();
	long ret;
	int i;

	for (i = 0; i < 100; i++) {
		ret = in_cs ? call_in_cs(slot, &cs) :
			      call_out_of_cs(slot, &cs);
		if (!in_cs || ret != 42)
			break;
	}
	return ret;
}

/*
 * Run the system call in the child. If @ret_pc is set, trace it with
 * PTRACE_SYSCALL and write @new_pc at the syscall-exit-stop whose PC is
 * @ret_pc. Returns the wait status of the child's exit, or -1 if the
 * child could not be traced.
 */
static int run(bool in_cs, unsigned long ret_pc, unsigned long new_pc)
{
	int status = -1, i, sig = 0;
	pid_t pid;

	pid = fork();
	if (pid == 0) {
		if (ret_pc) {
			if (traceme() < 0)
				_exit(1);
			raise(SIGSTOP);
		}
		_exit(run_syscall(in_cs));
	}
	if (pid < 0)
		return -1;
	if (!ret_pc) {
		waitpid(pid, &status, 0);
		return status;
	}

	if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status))
		goto out;
	if (ptrace(PTRACE_SETOPTIONS, pid, 0,
		   (void *)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL)) < 0)
		goto out;
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
out:
	/* The child is usually dead already; reap any tracee left behind. */
	tracee_kill(pid);
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

	/*
	 * The weak symbols bind to libc's definitions when libc registers
	 * rseq. If they did not, e.g. the test was built against a libc
	 * without rseq support, try dlsym() in case the libc it runs with
	 * has the symbols anyway; skip when no libc rseq area is found.
	 */
	if (!libc_rseq_size_p || !*libc_rseq_size_p) {
		libc_rseq_offset_p = dlsym(RTLD_NEXT, "__rseq_offset");
		libc_rseq_size_p = dlsym(RTLD_NEXT, "__rseq_size");
	}
	if (!libc_rseq_offset_p || !libc_rseq_size_p ||
	    *libc_rseq_size_p == 0)
		ksft_exit_skip("rseq is not registered by libc\n");
	rseq_offset = *libc_rseq_offset_p;

	cs.version = 0;
	cs.flags = 0;
	cs.start_ip = (uintptr_t)cs_start;
	cs.post_commit_offset = (uintptr_t)cs_end - (uintptr_t)cs_start;
	cs.abort_ip = (uintptr_t)cs_abort;

	/* An untraced system call inside the critical section must be fatal. */
	status = run(true, 0, 0);
	if (status < 0)
		ksft_exit_fail_msg("could not run the child\n");
	if (!killed_by_segv(status))
		ksft_exit_skip("rseq debugging is not enabled\n");

	ksft_set_plan(2);

	status = run(true, (uintptr_t)cs_after_svc, (uintptr_t)cs_end);
	if (status < 0)
		ksft_test_result_error("in critical section: test setup failed\n");
	else
		ksft_test_result(killed_by_segv(status),
				 "syscall in critical section is caught after the tracer moves the PC out\n");

	status = run(false, (uintptr_t)out_after_svc, (uintptr_t)cs_after_svc);
	if (status < 0)
		ksft_test_result_error("outside critical section: test setup failed\n");
	else
		/* exit 42 means the critical section was aborted, which is also fine */
		ksft_test_result(WIFEXITED(status),
				 "syscall outside critical section survives the tracer moving the PC in\n");

	ksft_finished();
}

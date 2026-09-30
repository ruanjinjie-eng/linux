// SPDX-License-Identifier: GPL-2.0
/*
 * PTRACE_SINGLESTEP across a system call that takes a ptrace stop while it
 * is in progress (here a PTRACE_EVENT_SECCOMP stop) must stop at the
 * return address of the SVC before any further instruction runs, so that
 * the tracer can look at the result of the system call.
 *
 * Before commit ac2081cdc4d9 ("arm64: ptrace: Consistently use
 * pseudo-singlestep exceptions"), the first stop after the seccomp stop
 * came one instruction later when stepping was enabled at the seccomp
 * stop: the hardware step needs one instruction to complete. The fix
 * reports a pseudo-step at the SVC return address instead.
 *
 * The pseudo-step also covers a register write at the seccomp stop:
 * since commit 3a5a4366cecc ("arm64: ptrace: Override SPSR.SS when
 * single-stepping is enabled"), PTRACE_SETREGSET sets SPSR.SS, which
 * discards the step left pending by the SVC.
 *
 * Both ways to get there are tested:
 *  - continue to the seccomp stop, then single-step;
 *  - single-step into the SVC, rewrite the registers at the seccomp stop,
 *    then single-step.
 */
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <linux/filter.h>
#include <linux/seccomp.h>

#include "kselftest.h"
#include "ptrace_common.h"

extern char after_svc[];

static void child(void)
{
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getegid, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = (unsigned short)ARRAY_SIZE(filter),
		.filter = filter,
	};

	if (traceme() < 0)
		_exit(1);
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 ||
	    prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0)
		_exit(2);
	raise(SIGSTOP);
	/* svc, then a label right after it and a few NOPs to step over */
	asm volatile("mov x8, %0\n"
		     "svc #0\n"
		     ".globl after_svc\n"
		     "after_svc:\n"
		     "nop\n nop\n nop\n nop\n"
		     : : "i" (SYS_getegid) : "x0", "x8", "memory");
	_exit(0);
}

/*
 * @step_in: single-step into the SVC (true) or continue to it (false).
 * Returns the PC of the first stop after the seccomp stop, 0 on error.
 */
static unsigned long first_stop_after_seccomp(bool step_in)
{
	unsigned long pc = 0;
	int status, i;
	pid_t pid;

	pid = fork();
	if (pid == 0)
		child();
	if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status))
		return 0;
	if (ptrace(PTRACE_SETOPTIONS, pid, 0,
		   (void *)(PTRACE_O_TRACESECCOMP | PTRACE_O_EXITKILL)) < 0)
		goto out;

	/* Run to the seccomp stop of the SVC at after_svc - 4. */
	for (i = 0; i < 100000; i++) {
		if (resume_and_wait(pid, step_in ? PTRACE_SINGLESTEP : PTRACE_CONT,
				    0, &status) < 0)
			goto out;
		if ((status >> 8) == SECCOMP_STOP_STATUS)
			break;
	}
	if ((status >> 8) != SECCOMP_STOP_STATUS)
		goto out;

	if (step_in) {
		struct user_pt_regs regs;

		/* Any register write goes through valid_user_regs(). */
		if (tracee_get_regs(pid, &regs) < 0 ||
		    tracee_set_regs(pid, &regs) < 0)
			goto out;
	}

	if (resume_and_wait(pid, PTRACE_SINGLESTEP, 0, &status) < 0 ||
	    WSTOPSIG(status) != SIGTRAP)
		goto out;
	pc = tracee_pc(pid);
out:
	tracee_kill(pid);
	return pc;
}

static void check(bool step_in, const char *name)
{
	unsigned long pc = first_stop_after_seccomp(step_in);

	if (!pc) {
		ksft_test_result_fail("%s: test setup failed\n", name);
		return;
	}
	if (pc != (unsigned long)after_svc)
		ksft_print_msg("%s: stopped at after_svc%+ld\n", name,
			       (long)(pc - (unsigned long)after_svc));
	ksft_test_result(pc == (unsigned long)after_svc,
			 "%s: first stop is at the SVC return address\n", name);
}

int main(void)
{
	ksft_print_header();
	ksft_set_plan(2);

	check(false, "continue to seccomp stop, then step");
	check(true, "step into seccomp stop, write regs, then step");

	ksft_finished();
}

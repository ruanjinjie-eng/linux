// SPDX-License-Identifier: GPL-2.0
/*
 * PTRACE_SYSEMU_SINGLESTEP over a system call.
 *
 * ptrace(2) says the emulated system call is reported by the
 * syscall-enter-stop alone ("If the tracee is continued using any other
 * method (including PTRACE_SYSEMU), no syscall-exit-stop occurs"), and the
 * next resume must single-step the following instruction.
 *
 * Commit ac2081cdc4d9 ("arm64: ptrace: Consistently use pseudo-singlestep
 * exceptions") added a pseudo-step SIGTRAP (si_code SI_USER) on every
 * syscall exit with stepping enabled, including the emulated one. That
 * gives the tracer an extra stop at the SVC return address before any
 * instruction runs.
 *
 * Tests:
 *  - no syscall-exit-stop for the emulated system call;
 *  - no pseudo-step stop after the syscall-enter-stop;
 *  - after resuming a PTRACE_EVENT_SECCOMP stop with
 *    PTRACE_SYSEMU_SINGLESTEP, the first stop is not a pseudo-step.
 */
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <linux/filter.h>
#include <linux/seccomp.h>

#include "kselftest.h"
#include "ptrace_common.h"

extern char emu_after_svc[], secc_after_svc[];

#define SI_USER_CODE	0	/* SI_USER, used by the pseudo-step */

static void child_emulated(void)
{
	if (traceme() < 0)
		_exit(1);
	raise(SIGSTOP);
	/* svc, then a label right after it and a few NOPs to step over */
	asm volatile("mov x8, %0\n"
		     "svc #0\n"
		     ".globl emu_after_svc\n"
		     "emu_after_svc:\n"
		     "nop\n nop\n nop\n nop\n"
		     : : "i" (SYS_getpid) : "x0", "x8", "memory");
	_exit(0);
}

/*
 * Single-step with PTRACE_SYSEMU_SINGLESTEP up to the emulated getpid()
 * and two stops past it. Reports whether a syscall-exit-stop or a
 * pseudo-step stop was seen after the syscall-enter-stop.
 */
static int run_emulated(bool *saw_exit, bool *saw_pseudo)
{
	int status, i, after = -1;
	pid_t pid;

	*saw_exit = *saw_pseudo = false;
	pid = fork();
	if (pid == 0)
		child_emulated();
	if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status))
		return -1;
	ptrace(PTRACE_SETOPTIONS, pid, 0,
	       (void *)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL));

	for (i = 0; i < 100000 && after != 0; i++) {
		long nr = -1;
		int op;

		if (resume_and_wait(pid, PTRACE_SYSEMU_SINGLESTEP, 0, &status) < 0)
			break;
		op = syscall_info(pid, &nr);
		if (after < 0) {
			if (op == PTRACE_SYSCALL_INFO_ENTRY && nr == SYS_getpid &&
			    tracee_pc(pid) == (unsigned long)emu_after_svc)
				after = 2;
			continue;
		}
		if (op == PTRACE_SYSCALL_INFO_EXIT)
			*saw_exit = true;
		if (WSTOPSIG(status) == SIGTRAP &&
		    tracee_si_code(pid) == SI_USER_CODE)
			*saw_pseudo = true;
		after--;
	}
	tracee_kill(pid);
	return after == 0 ? 0 : -1;
}

static void child_seccomp(void)
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
		     ".globl secc_after_svc\n"
		     "secc_after_svc:\n"
		     "nop\n nop\n nop\n nop\n"
		     : : "i" (SYS_getegid) : "x0", "x8", "memory");
	_exit(0);
}

/* si_code of the first stop after resuming the seccomp stop, -1 on error */
static int run_seccomp(void)
{
	int status, code = -1;
	pid_t pid;

	pid = fork();
	if (pid == 0)
		child_seccomp();
	if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status))
		return -1;
	ptrace(PTRACE_SETOPTIONS, pid, 0,
	       (void *)(PTRACE_O_TRACESECCOMP | PTRACE_O_EXITKILL));
	if (resume_and_wait(pid, PTRACE_CONT, 0, &status) == 0 &&
	    (status >> 8) == SECCOMP_STOP_STATUS &&
	    resume_and_wait(pid, PTRACE_SYSEMU_SINGLESTEP, 0, &status) == 0 &&
	    WSTOPSIG(status) == SIGTRAP)
		code = tracee_si_code(pid);
	tracee_kill(pid);
	return code;
}

int main(void)
{
	bool saw_exit, saw_pseudo;
	int code;

	ksft_print_header();
	ksft_set_plan(3);

	if (run_emulated(&saw_exit, &saw_pseudo) < 0) {
		ksft_test_result_fail("emulated syscall: test setup failed\n");
		ksft_test_result_fail("emulated syscall: test setup failed\n");
	} else {
		ksft_test_result(!saw_exit,
				 "emulated syscall has no syscall-exit-stop\n");
		ksft_test_result(!saw_pseudo,
				 "emulated syscall has no pseudo-step stop\n");
	}

	code = run_seccomp();
	if (code < 0)
		ksft_test_result_fail("seccomp stop: test setup failed\n");
	else
		ksft_test_result(code != SI_USER_CODE,
				 "no pseudo-step after resuming a seccomp stop with PTRACE_SYSEMU_SINGLESTEP\n");

	ksft_finished();
}

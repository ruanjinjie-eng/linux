// SPDX-License-Identifier: GPL-2.0
/*
 * Clearing SPSR.SS through PTRACE_SETREGSET must not stop
 * PTRACE_SINGLESTEP from executing the instruction at the PC.
 *
 * GDB does this when it reverse-steps: it writes back the saved pstate,
 * which has SS clear. Before commit 3a5a4366cecc ("arm64: ptrace:
 * Override SPSR.SS when single-stepping is enabled"), the step then
 * started in the active-pending state: the step exception was taken
 * before the instruction ran, so the PC did not move but the tracer
 * still got a SIGTRAP.
 */
#include "kselftest.h"
#include "ptrace_common.h"

extern char step_start[], step_next[];

static void child(void)
{
	if (traceme() < 0)
		_exit(1);
	raise(SIGSTOP);
	asm volatile(".globl step_start, step_next\n"
		     "step_start:\n"
		     "nop\n"
		     "step_next:\n"
		     "nop\n nop\n nop\n");
	_exit(0);
}

int main(void)
{
	struct user_pt_regs regs;
	unsigned long pc = 0;
	int status, i;
	pid_t pid;

	ksft_print_header();
	ksft_set_plan(1);

	pid = fork();
	if (pid == 0)
		child();
	if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status))
		ksft_exit_fail_msg("tracee did not stop\n");
	ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PTRACE_O_EXITKILL);

	/* Single-step until the tracee is at step_start. */
	for (i = 0; i < 100000; i++) {
		if (resume_and_wait(pid, PTRACE_SINGLESTEP, 0, &status) < 0)
			break;
		pc = tracee_pc(pid);
		if (pc == (unsigned long)step_start)
			break;
	}
	if (pc != (unsigned long)step_start) {
		tracee_kill(pid);
		ksft_exit_fail_msg("did not reach step_start\n");
	}

	/* Write the registers back with SS clear, as a reverse step does. */
	if (tracee_get_regs(pid, &regs) < 0)
		ksft_exit_fail_msg("PTRACE_GETREGSET failed\n");
	regs.pstate &= ~PSR_SS_BIT;
	if (tracee_set_regs(pid, &regs) < 0)
		ksft_exit_fail_msg("PTRACE_SETREGSET failed\n");

	pc = 0;
	if (resume_and_wait(pid, PTRACE_SINGLESTEP, 0, &status) == 0)
		pc = tracee_pc(pid);
	tracee_kill(pid);

	if (pc == (unsigned long)step_start)
		ksft_print_msg("PC did not move\n");
	ksft_test_result(pc == (unsigned long)step_next,
			 "single-step executes one instruction after SPSR.SS is cleared\n");

	ksft_finished();
}

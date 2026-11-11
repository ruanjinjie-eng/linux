// SPDX-License-Identifier: GPL-2.0
/*
 * PTRACE_SINGLESTEP into a signal handler must stop at the handler entry
 * without changing how the tracee handles SIGTRAP.
 *
 * Before commit ac2081cdc4d9 ("arm64: ptrace: Consistently use
 * pseudo-singlestep exceptions"), arm64 delivered that stop as a real
 * hardware step exception. If the tracee had SIGTRAP blocked, the forced
 * SIGTRAP unblocked it and reset its disposition to SIG_DFL. The fix
 * reports the stop with the generic pseudo-step, which leaves both alone.
 *
 * The tracee blocks SIGTRAP and installs its own SIGTRAP handler; after
 * stepping into its SIGUSR1 handler, both must be unchanged.
 */
#include <sys/mman.h>

#include "kselftest.h"
#include "ptrace_common.h"

/* Shared with the tracee, which writes it from its signal handler. */
struct shared {
	int handler_ran;
	int trap_blocked;
	int trap_handler_kept;
};

static struct shared *sh;

static void trap_handler(int sig)
{
	(void)sig;
}

static void usr1_handler(int sig)
{
	sigset_t cur;

	(void)sig;
	sigprocmask(SIG_BLOCK, NULL, &cur);
	sh->trap_blocked = sigismember(&cur, SIGTRAP);
	__atomic_store_n(&sh->handler_ran, 1, __ATOMIC_RELEASE);
}

static void child(void)
{
	struct sigaction sa;
	sigset_t blk;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = trap_handler;
	sigaction(SIGTRAP, &sa, NULL);
	sa.sa_handler = usr1_handler;
	sigaction(SIGUSR1, &sa, NULL);

	sigemptyset(&blk);
	sigaddset(&blk, SIGTRAP);
	sigprocmask(SIG_BLOCK, &blk, NULL);

	if (traceme() < 0)
		_exit(1);
	raise(SIGSTOP);
	while (!__atomic_load_n(&sh->handler_ran, __ATOMIC_ACQUIRE))
		;

	sigaction(SIGTRAP, NULL, &sa);
	sh->trap_handler_kept = (sa.sa_handler == trap_handler);
	_exit(0);
}

int main(void)
{
	unsigned long pc = 0;
	int status, i, sig = 0;
	pid_t pid;

	ksft_print_header();
	ksft_set_plan(3);

	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed\n");

	pid = fork();
	if (pid == 0)
		child();
	if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status))
		ksft_exit_fail_msg("tracee did not stop\n");
	ptrace(PTRACE_SETOPTIONS, pid, 0, (void *)PTRACE_O_EXITKILL);
	kill(pid, SIGUSR1);

	/* Step until the SIGUSR1 delivery stop, then step into the handler. */
	for (i = 0; i < 1000; i++) {
		if (resume_and_wait(pid, PTRACE_SINGLESTEP, sig, &status) < 0)
			break;
		if (sig) {
			pc = tracee_pc(pid);
			break;
		}
		if (WSTOPSIG(status) == SIGUSR1)
			sig = SIGUSR1;
	}
	ksft_test_result(pc == (unsigned long)usr1_handler,
			 "stopped at the signal handler entry\n");

	/* Let the tracee finish and report what it saw. */
	if (ptrace(PTRACE_CONT, pid, 0, 0) == 0)
		waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		tracee_kill(pid);
		ksft_test_result_fail("tracee did not exit normally\n");
		ksft_test_result_skip("SIGTRAP disposition not checked\n");
		ksft_finished();
	}
	ksft_test_result(sh->trap_blocked,
			 "SIGTRAP is still blocked in the handler\n");
	ksft_test_result(sh->trap_handler_kept,
			 "SIGTRAP handler was not reset to SIG_DFL\n");

	ksft_finished();
}

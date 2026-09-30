/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared helpers for the arm64 ptrace single-step self-tests.
 */
#ifndef ARM64_PTRACE_COMMON_H
#define ARM64_PTRACE_COMMON_H

#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <elf.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/ptrace.h>
#include <asm/ptrace.h>

#ifndef PTRACE_SYSEMU_SINGLESTEP
#define PTRACE_SYSEMU_SINGLESTEP	32
#endif

/* PSTATE.SS as seen in the saved SPSR (user_pt_regs.pstate) */
#define PSR_SS_BIT			0x00200000UL

/* Status of a SIGTRAP stop reported for PTRACE_EVENT_SECCOMP */
#define SECCOMP_STOP_STATUS		(SIGTRAP | (PTRACE_EVENT_SECCOMP << 8))

static inline int tracee_get_regs(pid_t pid, struct user_pt_regs *regs)
{
	struct iovec iov = { .iov_base = regs, .iov_len = sizeof(*regs) };

	return ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}

static inline int tracee_set_regs(pid_t pid, struct user_pt_regs *regs)
{
	struct iovec iov = { .iov_base = regs, .iov_len = sizeof(*regs) };

	return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}

/* Program counter of the tracee (0 on failure). */
static inline unsigned long tracee_pc(pid_t pid)
{
	struct user_pt_regs regs;

	if (tracee_get_regs(pid, &regs) < 0)
		return 0;
	return regs.pc;
}

/* si_code of the signal that caused the current stop (-1 on failure). */
static inline int tracee_si_code(pid_t pid)
{
	siginfo_t si;

	memset(&si, 0, sizeof(si));
	if (ptrace(PTRACE_GETSIGINFO, pid, 0, &si) < 0)
		return -1;
	return si.si_code;
}

/* PTRACE_SYSCALL_INFO_* op of the current stop, plus syscall nr on entry. */
static inline int syscall_info(pid_t pid, long *nr)
{
	struct ptrace_syscall_info info;

	memset(&info, 0, sizeof(info));
	if (ptrace(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof(info),
		   &info) < 0)
		return PTRACE_SYSCALL_INFO_NONE;
	if (info.op == PTRACE_SYSCALL_INFO_ENTRY && nr)
		*nr = (long)info.entry.nr;
	return info.op;
}

static inline int traceme(void)
{
	return ptrace(PTRACE_TRACEME, 0, 0, 0);
}

/* Resume the tracee with @req and wait for the next stop. */
static inline int resume_and_wait(pid_t pid, int req, int sig, int *status)
{
	if (ptrace(req, pid, 0, (void *)(long)sig) < 0)
		return -1;
	if (waitpid(pid, status, 0) < 0)
		return -1;
	return WIFSTOPPED(*status) ? 0 : -1;
}

/* Stop tracing the tracee and reap it so it does not linger as a zombie. */
static inline void tracee_kill(pid_t pid)
{
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

#endif /* ARM64_PTRACE_COMMON_H */

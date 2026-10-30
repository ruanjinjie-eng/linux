// SPDX-License-Identifier: GPL-2.0
/*
 * Check that the syscall arguments a ptrace tracer rewrites are the ones
 * seccomp observes.
 *
 * A tracer can rewrite the syscall arguments at a SECCOMP_RET_TRACE stop
 * (the syscall is re-evaluated afterwards) and at a PTRACE_SYSCALL entry
 * stop (seccomp runs after ptrace on the entry path).  In both cases the
 * filters and the tracepoint of the syscall that is actually executed have
 * to see the rewritten value.
 *
 * On arm32 the first syscall argument is copied into ARM_ORIG_r0 when the
 * syscall is entered because r0 is clobbered by the return value on the way
 * out; seccomp reads that copy through syscall_get_arguments().  A tracer
 * write to r0 therefore has to be mirrored into ARM_ORIG_r0 by the kernel.
 * The same register also feeds the syscall restart path, so stops at which
 * ARM_ORIG_r0 does not carry a syscall argument must be left alone.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/ptrace.h>
#include <linux/seccomp.h>
#include <elf.h>
#include <sys/uio.h>
#if defined(__arm__) || defined(__aarch64__)
#include <asm/ptrace.h>
#endif

#include "kselftest_harness.h"

#ifndef __NR_getpid
#define __NR_getpid 20
#endif

/* The first argument the tracee passes to getpid(). */
#define VAL_ORIG	0x11112222UL
/* The first argument the tracer rewrites it to. */
#define VAL_NEW		0x33334444UL
#define ERRNO_MARK	0x7f
#define TRACE_MARK	0x1001

#if defined(__arm__)
#define NATIVE_AUDIT_ARCH	AUDIT_ARCH_ARM
#elif defined(__aarch64__)
#define NATIVE_AUDIT_ARCH	AUDIT_ARCH_AARCH64
#elif defined(__x86_64__)
#define NATIVE_AUDIT_ARCH	AUDIT_ARCH_X86_64
#elif defined(__i386__)
#define NATIVE_AUDIT_ARCH	AUDIT_ARCH_I386
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define syscall_arg0	offsetof(struct seccomp_data, args[0])
#else
#define syscall_arg0	(offsetof(struct seccomp_data, args[0]) + sizeof(__u32))
#endif

#if defined(__arm__)
# define HAVE_ARG0		1
# define HAVE_ORIG_ARG0		1
# define ARCH_REGS		struct pt_regs
# define SYSCALL_ARG0(_regs)	((_regs).ARM_r0)
# define SYSCALL_ARG0_ORIG(_regs) ((_regs).ARM_ORIG_r0)
# define SYSCALL_RET_SET(_regs, _val) ((_regs).ARM_r0 = (_val))
#elif defined(__x86_64__)
# define HAVE_ARG0		1
# define ARCH_REGS		struct user_regs_struct
# define SYSCALL_ARG0(_regs)	((_regs).rdi)
# define SYSCALL_RET_SET(_regs, _val) ((_regs).rax = (_val))
#elif defined(__i386__)
# define HAVE_ARG0		1
# define ARCH_REGS		struct user_regs_struct
# define SYSCALL_ARG0(_regs)	((_regs).ebx)
# define SYSCALL_RET_SET(_regs, _val) ((_regs).eax = (_val))
#elif defined(__aarch64__)
# define HAVE_ARG0		1
# define USE_REGSET		1
# define ARCH_REGS		struct user_pt_regs
# define SYSCALL_ARG0(_regs)	((_regs).regs[0])
# define SYSCALL_RET_SET(_regs, _val) ((_regs).regs[0] = (_val))
/*
 * riscv and loongarch are deliberately left out: they hand orig_a0, not
 * a0, to the syscall and to syscall_get_arguments(), so the register this
 * test rewrites is not the one the syscall runs with.
 */
#endif

#ifdef HAVE_ARG0

/* getpid(VAL_NEW) is refused, getpid(VAL_ORIG) is traced. */
static const struct sock_filter filter_trace[] = {
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NATIVE_AUDIT_ARCH, 0, 7),
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 6),
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, syscall_arg0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, VAL_NEW, 0, 1),
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ERRNO_MARK),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, VAL_ORIG, 0, 2),
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE | TRACE_MARK),
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
};

/* getpid(VAL_NEW) is refused, anything else is allowed. */
static const struct sock_filter filter_allow[] = {
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NATIVE_AUDIT_ARCH, 0, 6),
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpid, 0, 3),
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, syscall_arg0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, VAL_NEW, 0, 1),
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ERRNO_MARK),
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
};

#define ERESTARTSYS	512

static int install_filter(const struct sock_filter *filter, unsigned int count)
{
	struct sock_fprog prog = {
		.len = (unsigned short)count,
		.filter = (struct sock_filter *)filter,
	};

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return -1;

	return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0);
}

static int get_regs(pid_t pid, ARCH_REGS *regs)
{
#ifdef USE_REGSET
	struct iovec iov = {
		.iov_base = regs,
		.iov_len = sizeof(*regs),
	};

	return ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov);
#else
	return ptrace(PTRACE_GETREGS, pid, NULL, regs);
#endif
}

static int set_regs(pid_t pid, ARCH_REGS *regs)
{
#ifdef USE_REGSET
	struct iovec iov = {
		.iov_base = regs,
		.iov_len = sizeof(*regs),
	};

	return ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov);
#else
	return ptrace(PTRACE_SETREGS, pid, NULL, regs);
#endif
}

static long get_syscall_info(pid_t pid, struct ptrace_syscall_info *info)
{
	return ptrace(PTRACE_GET_SYSCALL_INFO, pid, sizeof(*info), info);
}

/* waitpid() with a deadline, so that a kernel bug cannot hang the run. */
static pid_t wait_for_stop(pid_t pid, int *status, int timeout_ms)
{
	while (timeout_ms-- > 0) {
		pid_t ret = waitpid(pid, status, WNOHANG);

		if (ret == pid)
			return pid;
		if (ret < 0 && errno != EINTR)
			return ret;
		usleep(1000);
	}

	return 0;
}

/* Fork a tracee that stops itself, then calls getpid(VAL_ORIG). */
static pid_t spawn_tracee(const struct sock_filter *filter, unsigned int count)
{
	pid_t tracee = fork();

	if (tracee != 0)
		return tracee;

	ptrace(PTRACE_TRACEME, 0, NULL, NULL);
	raise(SIGSTOP);
	if (install_filter(filter, count))
		_exit(2);
	errno = 0;
	if (syscall(__NR_getpid, VAL_ORIG) == -1 && errno == ERRNO_MARK)
		_exit(0);	/* seccomp saw VAL_NEW */
	_exit(1);		/* seccomp saw VAL_ORIG */
}

static void attach(struct __test_metadata *_metadata, pid_t tracee, long options)
{
	int status;

	ASSERT_EQ(tracee, wait_for_stop(tracee, &status, 10000)) {
		kill(tracee, SIGKILL);
	}
	ASSERT_TRUE(WIFSTOPPED(status));
	ASSERT_EQ(0, ptrace(PTRACE_SETOPTIONS, tracee, NULL,
			    options | PTRACE_O_EXITKILL));
}

/*
 * The tracer rewrites the first argument at the PTRACE_EVENT_SECCOMP stop.
 * The syscall is re-evaluated after the stop, so the filter runs again with
 * the new value and refuses it.
 */
TEST(seccomp_trace_stop_sees_rewritten_arg)
{
	pid_t tracee = spawn_tracee(filter_trace, ARRAY_SIZE(filter_trace));
	int status;

	ASSERT_GE(tracee, 0);
	attach(_metadata, tracee,
	       PTRACE_O_TRACESECCOMP | PTRACE_O_TRACESYSGOOD);
	ASSERT_EQ(0, ptrace(PTRACE_CONT, tracee, NULL, NULL));

	for (;;) {
		ASSERT_EQ(tracee, wait_for_stop(tracee, &status, 10000)) {
			kill(tracee, SIGKILL);
		}
		if (WIFEXITED(status))
			break;
		ASSERT_FALSE(WIFSIGNALED(status));

		if ((status >> 8) ==
		    (SIGTRAP | (PTRACE_EVENT_SECCOMP << 8))) {
			ARCH_REGS regs;

			ASSERT_EQ(0, get_regs(tracee, &regs));
			SYSCALL_ARG0(regs) = VAL_NEW;
			ASSERT_EQ(0, set_regs(tracee, &regs));
		}
		ASSERT_EQ(0, ptrace(PTRACE_CONT, tracee, NULL, NULL));
	}

	EXPECT_EQ(0, WEXITSTATUS(status));
}

/*
 * The tracer rewrites the first argument at the syscall-entry stop.  seccomp
 * runs after ptrace, so the filter has to see the new value.
 */
TEST(syscall_entry_stop_sees_rewritten_arg)
{
	pid_t tracee = spawn_tracee(filter_allow, ARRAY_SIZE(filter_allow));
	int status, poked = 0;

	ASSERT_GE(tracee, 0);
	attach(_metadata, tracee, PTRACE_O_TRACESYSGOOD);
	ASSERT_EQ(0, ptrace(PTRACE_SYSCALL, tracee, NULL, NULL));

	for (;;) {
		struct ptrace_syscall_info info;
		long len;

		ASSERT_EQ(tracee, wait_for_stop(tracee, &status, 10000)) {
			kill(tracee, SIGKILL);
		}
		if (WIFEXITED(status))
			break;
		ASSERT_FALSE(WIFSIGNALED(status));

		len = get_syscall_info(tracee, &info);
		if (len < 0) {
			kill(tracee, SIGKILL);
			SKIP(return, "PTRACE_GET_SYSCALL_INFO unsupported");
		}
		if (!poked && info.op == PTRACE_SYSCALL_INFO_ENTRY &&
		    info.entry.nr == __NR_getpid) {
			ARCH_REGS regs;

			ASSERT_EQ(0, get_regs(tracee, &regs));
			SYSCALL_ARG0(regs) = VAL_NEW;
			ASSERT_EQ(0, set_regs(tracee, &regs));
			poked = 1;
		}
		ASSERT_EQ(0, ptrace(PTRACE_SYSCALL, tracee, NULL, NULL));
	}

	EXPECT_EQ(1, poked);
	EXPECT_EQ(0, WEXITSTATUS(status));
}

static void sighandler(int sig)
{
	(void)sig;
}

/*
 * The tracer makes write() return -ERESTARTSYS at the syscall-exit stop and
 * delivers a signal with SA_RESTART.  The syscall restarts with the first
 * argument as it was when the syscall was entered, so the restarted write
 * still reaches the pipe.  If the return value had replaced the argument,
 * write() would fail with EBADF instead.
 */
TEST(syscall_exit_stop_preserves_arg_for_restart)
{
	int pipefd[2];
	struct sigaction sa = {
		.sa_handler = sighandler,
		.sa_flags = SA_RESTART,
	};
	pid_t tracee;
	long last_nr = -1;
	int status, poked = 0;

	ASSERT_EQ(0, pipe(pipefd));

	tracee = fork();
	ASSERT_GE(tracee, 0);
	if (tracee == 0) {
		close(pipefd[0]);
		sigaction(SIGUSR1, &sa, NULL);
		ptrace(PTRACE_TRACEME, 0, NULL, NULL);
		raise(SIGSTOP);
		_exit(write(pipefd[1], "x", 1) == 1 ? 0 : 1);
	}
	close(pipefd[1]);

	attach(_metadata, tracee, PTRACE_O_TRACESYSGOOD);
	ASSERT_EQ(0, ptrace(PTRACE_SYSCALL, tracee, NULL, NULL));

	for (;;) {
		struct ptrace_syscall_info info;
		long len;

		ASSERT_EQ(tracee, wait_for_stop(tracee, &status, 10000)) {
			kill(tracee, SIGKILL);
		}
		if (WIFEXITED(status))
			break;
		ASSERT_FALSE(WIFSIGNALED(status));

		len = get_syscall_info(tracee, &info);
		if (len < 0) {
			kill(tracee, SIGKILL);
			SKIP(return, "PTRACE_GET_SYSCALL_INFO unsupported");
		}
		if (info.op == PTRACE_SYSCALL_INFO_ENTRY)
			last_nr = info.entry.nr;
		if (!poked && info.op == PTRACE_SYSCALL_INFO_EXIT &&
		    last_nr == __NR_write) {
			ARCH_REGS regs;

			ASSERT_EQ(0, get_regs(tracee, &regs));
			SYSCALL_RET_SET(regs, -ERESTARTSYS);
			ASSERT_EQ(0, set_regs(tracee, &regs));
			poked = 1;
			/* resume into the restart path */
			ASSERT_EQ(0, ptrace(PTRACE_CONT, tracee, NULL,
					    SIGUSR1));
			continue;
		}
		ASSERT_EQ(0, ptrace(PTRACE_SYSCALL, tracee, NULL, NULL));
	}

	close(pipefd[0]);
	EXPECT_EQ(1, poked);
	EXPECT_EQ(0, WEXITSTATUS(status));
}

#ifdef HAVE_ORIG_ARG0

/*
 * The tracer rewrites r0 while the tracee is stopped on a signal delivered
 * outside of a syscall, where ARM_ORIG_r0 holds the ~0 "no syscall" marker.
 * The marker must not be replaced by the value the tracer wrote.
 */
TEST(non_syscall_stop_keeps_orig_arg)
{
	sig_atomic_t *got_signal = mmap(NULL, sizeof(*got_signal),
					PROT_READ | PROT_WRITE,
					MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	struct sigaction sa = {
		.sa_handler = sighandler,
	};
	pid_t tracee;
	int status;
	ARCH_REGS regs;
	unsigned long before;

	ASSERT_TRUE(got_signal != MAP_FAILED);
	ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

	tracee = fork();
	ASSERT_GE(tracee, 0);
	if (tracee == 0) {
		ptrace(PTRACE_TRACEME, 0, NULL, NULL);
		raise(SIGSTOP);
		/*
		 * Spin in user mode so that the signal is not delivered on
		 * the return path of a syscall.
		 */
		while (!__atomic_load_n(got_signal, __ATOMIC_RELAXED))
			;
		_exit(0);
	}

	__atomic_store_n(got_signal, 0, __ATOMIC_RELAXED);
	attach(_metadata, tracee, 0);
	ASSERT_EQ(0, ptrace(PTRACE_CONT, tracee, NULL, NULL));
	usleep(200000);
	ASSERT_EQ(0, kill(tracee, SIGUSR1));

	ASSERT_EQ(tracee, wait_for_stop(tracee, &status, 10000)) {
		kill(tracee, SIGKILL);
	}
	ASSERT_TRUE(WIFSTOPPED(status));
	ASSERT_EQ(SIGUSR1, WSTOPSIG(status));

	ASSERT_EQ(0, get_regs(tracee, &regs));
	before = SYSCALL_ARG0_ORIG(regs);
	SYSCALL_ARG0(regs) = 0xdeadbeefUL;
	ASSERT_EQ(0, set_regs(tracee, &regs));
	ASSERT_EQ(0, get_regs(tracee, &regs));

	if (before == 0xffffffffUL)
		EXPECT_EQ(before, (unsigned long)SYSCALL_ARG0_ORIG(regs));

	__atomic_store_n(got_signal, 1, __ATOMIC_RELAXED);
	ASSERT_EQ(0, ptrace(PTRACE_CONT, tracee, NULL, SIGUSR1));
	ASSERT_EQ(tracee, wait_for_stop(tracee, &status, 10000)) {
		kill(tracee, SIGKILL);
	}
}

#endif /* HAVE_ORIG_ARG0 */

#else /* !HAVE_ARG0 */

TEST(unsupported_arch)
{
	SKIP(return, "no syscall argument 0 accessor for this architecture");
}

#endif /* HAVE_ARG0 */

TEST_HARNESS_MAIN

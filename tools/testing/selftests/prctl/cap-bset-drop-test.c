// SPDX-License-Identifier: GPL-2.0
/*
 * Tests for PR_CAPBSET_DROP_MASK, with thread and process scope.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/capability.h>

#include "../kselftest.h"

#ifndef PR_CAPBSET_DROP_MASK
#define PR_CAPBSET_DROP_MASK 82
#endif
#ifndef PR_CAPBSET_DROP_PROCESS_MASK
#define PR_CAPBSET_DROP_PROCESS_MASK 83
#endif

#define N_THREADS 4

#define CHILD_PASS	0
#define CHILD_FAIL	1
#define CHILD_SKIP	2

static int pipe_fd[2];
static atomic_int threads_ready;
static atomic_int threads_ok;
static unsigned long dropped_cap;

static atomic_int thr_go;
static atomic_int thr_sib_has;

static atomic_int fork_go;
static atomic_int fork_ok;

static atomic_int conc_stop;
static atomic_int conc_dropped;
static atomic_int conc_bad;

#define MULTI_DROP 8
static int multi_caps[MULTI_DROP];
static int multi_n;
static pthread_barrier_t multi_start;
static atomic_int multi_done;
static atomic_int multi_stop;
static atomic_int multi_bad;

static int last_cap;

static int read_last_cap(void)
{
	char buf[32];
	int fd, n, v = -1;

	fd = open("/proc/sys/kernel/cap_last_cap", O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	v = atoi(buf);
	return v;
}

static int bset_has(unsigned long cap)
{
	return prctl(PR_CAPBSET_READ, cap, 0, 0, 0) > 0;
}

static int drop_cap_process(unsigned long cap)
{
	unsigned long low = cap < 32 ? 1UL << cap : 0;
	unsigned long high = cap < 32 ? 0 : 1UL << (cap - 32);

	return prctl(PR_CAPBSET_DROP_PROCESS_MASK, low, high, 0, 0);
}

static int drop_cap_thread(unsigned long cap)
{
	unsigned long low = cap < 32 ? 1UL << cap : 0;
	unsigned long high = cap < 32 ? 0 : 1UL << (cap - 32);

	return prctl(PR_CAPBSET_DROP_MASK, low, high, 0, 0);
}

static int run_child(int (*fn)(void))
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return CHILD_FAIL;
	if (pid == 0)
		_exit(fn());
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
		return CHILD_FAIL;
	return WEXITSTATUS(status);
}

static void report_child(int ret, const char *name)
{
	if (ret == CHILD_PASS)
		ksft_test_result_pass("%s\n", name);
	else if (ret == CHILD_SKIP)
		ksft_test_result_skip("%s\n", name);
	else
		ksft_test_result_fail("%s\n", name);
}

static void run_test(int cond, int (*fn)(void), const char *name)
{
	if (!cond) {
		ksft_test_result_skip("%s\n", name);
		return;
	}
	report_child(run_child(fn), name);
}

static int pick_cap(int lo, int hi)
{
	int cap;

	for (cap = lo; cap <= hi; cap++)
		if (bset_has(cap))
			return cap;
	return -1;
}

static int have_cap_setpcap(void)
{
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	struct __user_cap_data_struct data[2];

	if (syscall(SYS_capget, &hdr, data))
		return 0;
	return !!(data[0].effective & (1U << CAP_SETPCAP));
}

static int drop_effective_cap_setpcap(void)
{
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	struct __user_cap_data_struct data[2];

	if (syscall(SYS_capget, &hdr, data))
		return -1;
	data[0].effective &= ~(1U << CAP_SETPCAP);
	return syscall(SYS_capset, &hdr, data);
}

static void *blocked_worker(void *arg)
{
	char c;

	(void)arg;

	atomic_fetch_add(&threads_ready, 1);

	/*
	 * Block until the main thread has issued the drop.  The drop is made
	 * effective for the whole group through the thread group's pending
	 * mask, so it must be visible here once we run again.
	 */
	for (;;) {
		ssize_t n = read(pipe_fd[0], &c, 1);

		if (n == 1)
			break;
		if (n < 0 && errno == EINTR)
			continue;
		return NULL;
	}

	if (prctl(PR_CAPBSET_READ, dropped_cap, 0, 0, 0) == 0)
		atomic_fetch_add(&threads_ok, 1);
	return NULL;
}

static void *late_worker(void *arg)
{
	(void)arg;

	/* Must inherit the reduced bounding set of the parent thread. */
	if (prctl(PR_CAPBSET_READ, dropped_cap, 0, 0, 0) == 0)
		atomic_fetch_add(&threads_ok, 1);
	return NULL;
}

/* The caller, blocked siblings and later threads all lose the capability. */
static int drop_test_child(void)
{
	unsigned long cap = dropped_cap;
	pthread_t t[N_THREADS], late;
	char c = 'x';
	int i, ret;

	atomic_store(&threads_ready, 0);
	atomic_store(&threads_ok, 0);

	if (pipe(pipe_fd))
		return CHILD_FAIL;

	for (i = 0; i < N_THREADS; i++) {
		if (pthread_create(&t[i], NULL, blocked_worker, NULL)) {
			close(pipe_fd[0]);
			close(pipe_fd[1]);
			return CHILD_FAIL;
		}
	}
	while (atomic_load(&threads_ready) < N_THREADS)
		sched_yield();

	ret = drop_cap_process(cap);
	if (ret) {
		ksft_print_msg("PR_CAPBSET_DROP_PROCESS_MASK(cap %lu) failed: %s\n",
			       cap, strerror(errno));
		ret = CHILD_FAIL;
		goto out;
	}

	for (i = 0; i < N_THREADS; i++) {
		if (write(pipe_fd[1], &c, 1) != 1) {
			ksft_print_msg("pipe write failed: %s\n",
				       strerror(errno));
			/* Close the write end so the workers see EOF. */
			close(pipe_fd[1]);
			pipe_fd[1] = -1;
			ret = CHILD_FAIL;
			goto out_join;
		}
	}
out_join:
	for (i = 0; i < N_THREADS; i++)
		pthread_join(t[i], NULL);

	if (atomic_load(&threads_ok) != N_THREADS) {
		ksft_print_msg("only %d of %d sibling threads saw the drop\n",
			       atomic_load(&threads_ok), N_THREADS);
		ret = CHILD_FAIL;
		goto out;
	}

	/* The calling thread must have dropped it synchronously. */
	if (bset_has(cap)) {
		ksft_print_msg("calling thread still has capability %lu\n",
			       cap);
		ret = CHILD_FAIL;
		goto out;
	}

	/* A thread created afterwards must inherit the reduced set. */
	if (pthread_create(&late, NULL, late_worker, NULL))
		goto out_ok;
	pthread_join(late, NULL);
	if (atomic_load(&threads_ok) != N_THREADS + 1) {
		ksft_print_msg("late thread did not inherit the drop\n");
		ret = CHILD_FAIL;
		goto out;
	}

out_ok:
	ret = CHILD_PASS;
out:
	if (pipe_fd[1] >= 0)
		close(pipe_fd[1]);
	close(pipe_fd[0]);
	return ret;
}

/* Thread scope: only the caller and its descendants lose the capability. */
static void *thread_sibling(void *arg)
{
	(void)arg;

	while (!atomic_load(&thr_go))
		sched_yield();
	atomic_store(&thr_sib_has, bset_has(dropped_cap));
	return NULL;
}

static int thread_scope_child(void)
{
	pthread_t sib, late;
	pid_t pid;
	int status;

	atomic_store(&thr_go, 0);
	atomic_store(&thr_sib_has, 0);
	atomic_store(&threads_ok, 0);

	if (pthread_create(&sib, NULL, thread_sibling, NULL))
		return CHILD_FAIL;

	if (drop_cap_thread(dropped_cap)) {
		ksft_print_msg("thread-scope drop failed: %s\n",
			       strerror(errno));
		atomic_store(&thr_go, 1);
		pthread_join(sib, NULL);
		return CHILD_FAIL;
	}
	atomic_store(&thr_go, 1);
	pthread_join(sib, NULL);

	if (bset_has(dropped_cap)) {
		ksft_print_msg("calling thread still has capability %lu\n",
			       dropped_cap);
		return CHILD_FAIL;
	}
	if (!atomic_load(&thr_sib_has)) {
		ksft_print_msg("sibling thread lost capability %lu\n",
			       dropped_cap);
		return CHILD_FAIL;
	}

	if (pthread_create(&late, NULL, late_worker, NULL))
		return CHILD_FAIL;
	pthread_join(late, NULL);
	if (atomic_load(&threads_ok) != 1) {
		ksft_print_msg("new thread did not inherit the drop\n");
		return CHILD_FAIL;
	}

	pid = fork();
	if (pid < 0)
		return CHILD_FAIL;
	if (pid == 0)
		_exit(bset_has(dropped_cap) ? 1 : 0);
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		ksft_print_msg("forked child did not inherit the drop\n");
		return CHILD_FAIL;
	}
	return CHILD_PASS;
}

/* A sibling whose cred was not updated must pass the drop to its children. */
static void *fork_sibling(void *arg)
{
	pid_t pid;
	int status;

	(void)arg;

	while (!atomic_load(&fork_go))
		sched_yield();

	if (bset_has(dropped_cap))
		atomic_store(&fork_ok, 0);

	pid = fork();
	if (pid == 0)
		_exit(bset_has(dropped_cap) ? 1 : 0);
	if (pid < 0) {
		atomic_store(&fork_ok, 0);
		return NULL;
	}
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		atomic_store(&fork_ok, 0);
	return NULL;
}

static int fork_test_child(void)
{
	pthread_t sib;

	atomic_store(&fork_go, 0);
	atomic_store(&fork_ok, 1);

	if (pthread_create(&sib, NULL, fork_sibling, NULL))
		return CHILD_FAIL;

	if (drop_cap_process(dropped_cap)) {
		atomic_store(&fork_go, 1);
		pthread_join(sib, NULL);
		return CHILD_FAIL;
	}
	atomic_store(&fork_go, 1);
	pthread_join(sib, NULL);

	return atomic_load(&fork_ok) ? CHILD_PASS : CHILD_FAIL;
}

/*
 * Workers do not wait for the drop, so the spawners keep cloning while it
 * runs; any worker checking after it returned must not see the capability.
 */
static void *conc_worker(void *arg)
{
	(void)arg;

	if (atomic_load(&conc_dropped) && bset_has(dropped_cap))
		atomic_fetch_add(&conc_bad, 1);
	return NULL;
}

static void *conc_spawner(void *arg)
{
	(void)arg;

	while (!atomic_load(&conc_stop)) {
		pthread_t t;

		if (pthread_create(&t, NULL, conc_worker, NULL) == 0)
			pthread_join(t, NULL);
	}
	return NULL;
}

static int conc_test_child(void)
{
	pthread_t sp[4];
	int i;

	atomic_store(&conc_stop, 0);
	atomic_store(&conc_dropped, 0);
	atomic_store(&conc_bad, 0);

	for (i = 0; i < 4; i++) {
		if (pthread_create(&sp[i], NULL, conc_spawner, NULL))
			return CHILD_FAIL;
	}

	if (drop_cap_process(dropped_cap)) {
		atomic_store(&conc_stop, 1);
		for (i = 0; i < 4; i++)
			pthread_join(sp[i], NULL);
		return CHILD_FAIL;
	}
	atomic_store(&conc_dropped, 1);
	usleep(20000);
	atomic_store(&conc_stop, 1);
	for (i = 0; i < 4; i++)
		pthread_join(sp[i], NULL);

	return atomic_load(&conc_bad) ? CHILD_FAIL : CHILD_PASS;
}

/* Concurrent drops of different masks, racing with clone(), must all apply. */
static int multi_bset_has_any(void)
{
	int i;

	for (i = 0; i < multi_n; i++)
		if (bset_has(multi_caps[i]))
			return 1;
	return 0;
}

static void *multi_reader(void *arg)
{
	(void)arg;

	if (atomic_load(&multi_done) && multi_bset_has_any())
		atomic_fetch_add(&multi_bad, 1);
	return NULL;
}

static void *multi_spawner(void *arg)
{
	(void)arg;

	while (!atomic_load(&multi_stop)) {
		pthread_t t;

		if (pthread_create(&t, NULL, multi_reader, NULL) == 0)
			pthread_join(t, NULL);
	}
	return NULL;
}

static void *multi_dropper(void *arg)
{
	long i = (long)arg;

	pthread_barrier_wait(&multi_start);
	/* Drop its own cap, then immediately race a second time. */
	drop_cap_process(multi_caps[i]);
	drop_cap_process(multi_caps[(i + 1) % multi_n]);
	return NULL;
}

static int multi_drop_test_child(void)
{
	pthread_t dr[MULTI_DROP], sp[4], late;
	int i;

	multi_n = 0;
	for (i = 0; i <= last_cap && multi_n < MULTI_DROP; i++) {
		if (i == CAP_SETPCAP)
			continue;
		if (bset_has(i))
			multi_caps[multi_n++] = i;
	}
	if (multi_n < 2)
		return CHILD_SKIP;

	atomic_store(&multi_done, 0);
	atomic_store(&multi_stop, 0);
	atomic_store(&multi_bad, 0);
	pthread_barrier_init(&multi_start, NULL, multi_n + 1);

	for (i = 0; i < 4; i++) {
		if (pthread_create(&sp[i], NULL, multi_spawner, NULL))
			return CHILD_FAIL;
	}
	for (i = 0; i < multi_n; i++) {
		if (pthread_create(&dr[i], NULL, multi_dropper,
				   (void *)(long)i))
			return CHILD_FAIL;
	}
	pthread_barrier_wait(&multi_start);
	for (i = 0; i < multi_n; i++)
		pthread_join(dr[i], NULL);
	atomic_store(&multi_done, 1);
	usleep(20000);
	atomic_store(&multi_stop, 1);
	for (i = 0; i < 4; i++)
		pthread_join(sp[i], NULL);

	if (multi_bset_has_any()) /* the calling thread itself */
		return CHILD_FAIL;
	if (atomic_load(&multi_bad))
		return CHILD_FAIL;

	if (pthread_create(&late, NULL, multi_reader, NULL))
		return CHILD_FAIL;
	pthread_join(late, NULL);
	if (atomic_load(&multi_bad))
		return CHILD_FAIL;

	return CHILD_PASS;
}

static int userns_unavailable(void)
{
	return errno == EPERM || errno == EINVAL || errno == ENOSPC ||
	       errno == EUSERS || errno == EACCES;
}

/* A new user namespace starts with a full bounding set. */
static int userns_child(void)
{
	pid_t pid;
	int status;

	if (drop_cap_process(dropped_cap))
		return CHILD_FAIL;

	pid = syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, 0, 0, 0, 0);
	if (pid < 0)
		return userns_unavailable() ? CHILD_SKIP : CHILD_FAIL;
	if (pid == 0)
		_exit(bset_has(dropped_cap) ? 0 : 1);
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		ksft_print_msg("clone(CLONE_NEWUSER) child lacks capability %lu\n",
			       dropped_cap);
		return CHILD_FAIL;
	}

	if (unshare(CLONE_NEWUSER))
		return userns_unavailable() ? CHILD_SKIP : CHILD_FAIL;
	if (!bset_has(dropped_cap)) {
		ksft_print_msg("unshare(CLONE_NEWUSER) lacks capability %lu\n",
			       dropped_cap);
		return CHILD_FAIL;
	}
	return CHILD_PASS;
}

static int eperm_child(void)
{
	if (drop_effective_cap_setpcap()) {
		ksft_print_msg("capset failed: %s\n", strerror(errno));
		return CHILD_FAIL;
	}

	if (prctl(PR_CAPBSET_DROP_MASK, 1, 0, 0, 0) == 0 ||
	    errno != EPERM) {
		ksft_print_msg("PR_CAPBSET_DROP_MASK without CAP_SETPCAP: %s\n",
			       strerror(errno));
		return CHILD_FAIL;
	}
	return CHILD_PASS;
}

static int empty_child(void)
{
	if (drop_effective_cap_setpcap()) {
		ksft_print_msg("capset failed: %s\n", strerror(errno));
		return CHILD_FAIL;
	}

	if (prctl(PR_CAPBSET_DROP_MASK, 0, 0, 0, 0) != 0) {
		ksft_print_msg("empty mask without CAP_SETPCAP: %s\n",
			       strerror(errno));
		return CHILD_FAIL;
	}
	return CHILD_PASS;
}

int main(void)
{
	int privileged = have_cap_setpcap();
	int cap_lo, cap_hi;
	unsigned long long unknown;

	last_cap = read_last_cap();

	ksft_print_header();
	ksft_set_plan(12);

	/* Argument validation, independent of privileges. */
	if (prctl(PR_CAPBSET_DROP_MASK, 0, 0, 1, 0) == 0 || errno != EINVAL)
		ksft_test_result_fail("PR_CAPBSET_DROP_MASK nonzero arg4\n");
	else
		ksft_test_result_pass("PR_CAPBSET_DROP_MASK nonzero arg4\n");

	if (prctl(PR_CAPBSET_DROP_PROCESS_MASK, 0, 0, 0, 1) == 0 ||
	    errno != EINVAL)
		ksft_test_result_fail("PR_CAPBSET_DROP_MASK nonzero arg5\n");
	else
		ksft_test_result_pass("PR_CAPBSET_DROP_MASK nonzero arg5\n");

	if (privileged) {
		ksft_test_result(run_child(empty_child) == CHILD_PASS,
				 "empty mask without CAP_SETPCAP\n");
	} else if (prctl(PR_CAPBSET_DROP_MASK, 0, 0, 0, 0) != 0) {
		ksft_test_result_fail("empty mask without CAP_SETPCAP (errno=%d, old kernel?)\n",
				      errno);
	} else {
		ksft_test_result_pass("empty mask without CAP_SETPCAP\n");
	}

	if (privileged) {
		ksft_test_result(run_child(eperm_child) == CHILD_PASS,
				 "EPERM without CAP_SETPCAP\n");
	} else if (prctl(PR_CAPBSET_DROP_MASK, 1, 0, 0, 0) == 0 ||
		   errno != EPERM) {
		ksft_test_result_fail("EPERM without CAP_SETPCAP (errno=%d, old kernel?)\n",
				      errno);
	} else {
		ksft_test_result_pass("EPERM without CAP_SETPCAP\n");
	}

	/* Bits for capabilities unknown to the kernel are silently ignored. */
	unknown = (last_cap >= 0 && last_cap < 63) ? (~0ULL << (last_cap + 1)) : 0;
	if (unknown) {
		unsigned long low = (unsigned int)unknown;
		unsigned long high = (unsigned int)(unknown >> 32);

		if (!privileged) {
			ksft_test_result_skip("unknown capabilities ignored (needs CAP_SETPCAP)\n");
		} else if (prctl(PR_CAPBSET_DROP_MASK, low, high, 0, 0)) {
			ksft_test_result_fail("unknown capabilities ignored (errno=%d)\n",
					      errno);
		} else {
			ksft_test_result_pass("unknown capabilities ignored\n");
		}
	} else {
		ksft_test_result_skip("unknown capabilities ignored\n");
	}

	/* Each functional test runs in a child to keep the parent's set. */
	cap_lo = last_cap >= 0 ? pick_cap(0, last_cap < 31 ? last_cap : 31) : -1;
	cap_hi = last_cap > 31 ? pick_cap(32, last_cap) : -1;

	dropped_cap = cap_lo;
	run_test(privileged && cap_lo >= 0, thread_scope_child,
		 "PR_CAPBSET_DROP_MASK drops only in the calling thread");

	run_test(privileged && cap_lo >= 0, drop_test_child,
		 "PR_CAPBSET_DROP_PROCESS_MASK drops all threads");

	dropped_cap = cap_hi;
	run_test(privileged && cap_hi >= 0, drop_test_child,
		 "PR_CAPBSET_DROP_PROCESS_MASK high word");

	dropped_cap = cap_lo;

	run_test(privileged && cap_lo >= 0, fork_test_child,
		 "PR_CAPBSET_DROP_PROCESS_MASK inherited by forked child");

	run_test(privileged && cap_lo >= 0, conc_test_child,
		 "PR_CAPBSET_DROP_PROCESS_MASK concurrent threads");

	run_test(privileged, multi_drop_test_child,
		 "PR_CAPBSET_DROP_PROCESS_MASK concurrent masks");

	run_test(privileged && cap_lo >= 0, userns_child,
		 "PR_CAPBSET_DROP_PROCESS_MASK not applied in new user namespace");

	ksft_finished();
}

#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>

#include "syscallmeter.h"

static void
timer_handler(int sig, siginfo_t *si, void *uc)
{
	static struct meter_stats *stats = NULL;
	static long count = 0;
	static long prev = 0;

	struct timespec ts;
	struct timespec ts2;

	long total;

	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);

	if (sig == 2048) {
		stats = (struct meter_stats *)uc;
		return;
	} else if (sig == 2049) {
		count = (long)uc;
		return;
	}

	total = 0;
	if (stats != NULL && count > 0) {
		for (int i = 0; i < count; i++) {
			total += stats[i].cycles;
		}
	} else {
		total = -1;
	}

	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts2);

	printf("[%ld.%09ld] ops = %ld\n", ts.tv_sec, ts.tv_nsec,
	    (total - prev));

	prev = total;
}

int
enable_progress(struct meter_stats *stats, long count)
{
	struct sigevent sev;
	struct sigaction sa;

	timer_t timerid;
	struct itimerspec timer_spec;

	int err;

	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = timer_handler;

	timer_handler(2048, NULL, stats);
	timer_handler(2049, NULL, (void *)count);

	sigemptyset(&sa.sa_mask);
	sigaction(SIGRTMIN, &sa, NULL);

	sev.sigev_notify = SIGEV_SIGNAL;
	sev.sigev_signo = SIGRTMIN;
	sev.sigev_value.sival_ptr = &timerid;

	if (timer_create(CLOCK_MONOTONIC, &sev, &timerid) == -1) {
		perror("timer_create");
		return 1;
	}

	timer_spec.it_value.tv_sec = 1;
	timer_spec.it_value.tv_nsec = 0;
	timer_spec.it_interval.tv_sec = 1;
	timer_spec.it_interval.tv_nsec = 0;

	if (timer_settime(timerid, 0, &timer_spec, NULL) == -1) {
		perror("timer_settime");
		return (-1);
	}

	return (0);
}

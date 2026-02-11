
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "syscallmeter.h"
#include "ticks.h"

enum w_clock_gettime_mode { SYSCALL, RDTSC };

int curr_mode = SYSCALL;
bool has_histo = false;
bool has_modehisto = false;
bool has_stats = false;
bool has_show = false;

/**
 * Shared memory >>> +70ns (barrier?)
 * *ptr = value; ptr++ vs arr[i] = value; >>> +20ns (why??)
 *
 */
int
w_clock_gettime_opt(char *option)
{
	if (strcmp("rdtsc", option) == 0) {
		curr_mode = RDTSC;
	} else if (strcmp("histo", option) == 0) {
		has_histo = true;
		has_stats = true;
	} else if (strcmp("modehisto", option) == 0) {
		has_modehisto = true;
		has_stats = true;
	} else if (strcmp("show", option) == 0) {
		has_show = true;
	} else {
		printf("unexpected option: %s\n", option);
		return (-1);
	}

	return (0);
}

int
w_clock_gettime_init(struct meter_settings *s, int dirfd)
{
	s->cycles *= 1000;
	return (0);
}

long
w_clock_gettime_job(int workerid, struct meter_worker_state *s, int dirfd)
{
	enum { MEASURECYCLES = (1 << 14), HISTOSIZE = 32, MODEHISTOSIZE = 31 };
	struct timespec ts, tsprev;
	uint64_t ps;
	uint64_t ticks, prev, delta;
	uint64_t sum, avg, avg2, t1, t2, max, t1_count, t2_count;
	struct rusage rusage_before, rusage_after;

	uint64_t *stats_base, *stats_ptr;
	uint64_t histo[HISTOSIZE];		   // exponentional histogram
	uint64_t modehisto[2 * MODEHISTOSIZE + 1]; // near-avg

	if (has_histo)
		memset(histo, 0, HISTOSIZE * sizeof(uint64_t));

	if (has_modehisto)
		memset(modehisto, 0,
		    (2 * MODEHISTOSIZE + 1) * sizeof(uint64_t));

	if (has_stats) {
		stats_base = malloc(
		    sizeof(uint64_t) * (MEASURECYCLES + s->settings->cycles));
		if (stats_base == NULL) {
			return (-1);
		}

		stats_ptr = stats_base;
		memset(stats_base, 0,
		    MEASURECYCLES + s->settings->cycles * sizeof(uint64_t));
	}

	switch (curr_mode) {
	case RDTSC:

		if (has_show)
			printf("Mode RDTSC\n");

		sum = 0;
		clock_gettime(CLOCK_MONOTONIC, &tsprev);
		for (long i = 0; i < MEASURECYCLES; i++) {
			prev = vi_tmGetTicks();
			ticks = vi_tmGetTicks();
			delta = ticks - prev;
			sum += delta;
			if (has_stats)
				stats_base[i] = delta;
		}

		clock_gettime(CLOCK_MONOTONIC, &ts);
		delta = ((ts.tv_sec - tsprev.tv_sec) * 1000000000 +
		    (ts.tv_nsec - tsprev.tv_nsec));

		ps = (1000 * delta) / (2 * sum);

		if (has_show)
			printf("[%d] Ticks vs time: %ld %ld %ld\n", workerid,
			    2 * sum, delta, ps);

		avg = sum >> 14;
		t1 = avg << 2;
		t2 = avg >> 2;

		max = 0;
		t1_count = 0;
		t2_count = 0;

		getrusage(RUSAGE_SELF, &rusage_before);
		sum = 0;
		for (long i = 0; i < s->settings->cycles; i++) {
			prev = vi_tmGetTicks();

			ticks = vi_tmGetTicks();
			delta = ticks - prev;
			if (max < delta) {
				max = delta;
			}
			if (t1 <= delta) {
				t1_count++;
			} else if (t2 >= delta) {
				t2_count++;
			} else {
				sum += delta;
			}

			if (has_stats)
				stats_base[i] = delta;

			// for (volatile int zzz = 0; zzz < 256; zzz++)
			// 	asm("");
			//  if ((i & 0xf) == 0xf)
			//  sched_yield();
		}
		getrusage(RUSAGE_SELF, &rusage_after);
		printf("involuntary vs voluntary: %ld %ld\n",
		    rusage_after.ru_nivcsw - rusage_before.ru_nivcsw,
		    rusage_after.ru_nvcsw - rusage_before.ru_nvcsw);

		if (s->settings->cycles > t1_count + t2_count) {
			avg2 = sum /
			    (s->settings->cycles - t1_count - t2_count);
		} else {
			avg2 = avg;
		}

		if (has_show)
			printf(
			    "[%d] average = %ld ns vs %ld ns\n[%d] more than %ld ns = %ld, less than %ld ns = %ld, max = %ld\n",
			    workerid, (avg * ps) / 1000, (avg2 * ps) / 1000,
			    workerid, t1, t1_count, t2, t2_count, max);
		break;
	default:
		if (has_show)
			printf("Mode clock_gettime\n");

		clock_gettime(CLOCK_MONOTONIC, &ts);
		prev = (ts.tv_sec * 1000000000 + ts.tv_nsec);

		sum = 0;
		for (long i = 0; i < MEASURECYCLES; i++) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			ticks = (ts.tv_sec * 1000000000 + ts.tv_nsec);
			delta = ticks - prev;
			prev = ticks;
			sum += delta;
			if (has_stats)
				stats_base[i] = delta;
		}

		avg = sum >> 14;
		t1 = avg << 2;
		t2 = avg >> 1;
		max = 0;

		sum = 0;
		t1_count = 0;
		t2_count = 0;
		for (long i = 0; i < s->settings->cycles; i++) {
			clock_gettime(CLOCK_MONOTONIC, &tsprev);
			clock_gettime(CLOCK_MONOTONIC, &ts);
			delta = ((ts.tv_sec - tsprev.tv_sec) * 1000000000 +
			    (ts.tv_nsec - tsprev.tv_nsec));

			if (has_stats)
				stats_base[i] = delta;

			if (max < delta) {
				max = delta;
			}

			if (t1 <= delta) {
				t1_count++;
			} else if (t2 >= delta) {
				t2_count++;
			} else {
				sum += delta;
			}
		}

		if (s->settings->cycles > t1_count + t2_count) {
			avg2 = sum /
			    (s->settings->cycles - t1_count - t2_count);
		} else {
			avg2 = avg;
		}

		if (has_show)
			printf(
			    "[%d] average = %ld ns vs %ld ns\n[%d] less than %ld ns = %ld, more than %ld ns = %ld, max = %ld\n",
			    workerid, avg, avg2, workerid, t1, t1_count, t2,
			    t2_count, max);

		break;
	}

	if (workerid == s->settings->cpu_limit - 2) {
		for (long i = 0; i < s->settings->cycles; i++) {
			int j;

			if (has_histo)
				histo[sizeof(unsigned int) * 8 -
				    __builtin_clz(stats_base[i])]++;

			if (has_modehisto) {
				j = stats_base[i] - avg2 + MODEHISTOSIZE;
				if (j >= 0 && j < 2 * MODEHISTOSIZE + 1)
					modehisto[j]++;
			}
		}

		if (has_histo) {
			int first = -1, last = -1;
			for (int i = 0; i < HISTOSIZE; i++) {
				if (histo[i] > 0) {
					if (first < 0) {
						first = i;
					}
					last = i;
				}
			}

			if (first > 0) {
				first--;
			}
			if (last < HISTOSIZE - 1) {
				last++;
			}

			printf("[%d] ===== Log2 histogram ==== \n", workerid);
			for (int i = first; i <= last; i++) {
				printf("[%d]\t%d..%d\t= %ld\n", workerid,
				    (1 << (i - 1)), ((1 << (i)) - 1), histo[i]);
			}
		}

		if (has_modehisto) {
			printf("[%d] ===== Near-avg values ==== \n", workerid);

			int first = -1, last = -1;
			for (int i = 0; i < 2 * MODEHISTOSIZE + 1; i++) {
				if (modehisto[i] > 0) {
					if (first < 0) {
						first = i;
					}
					last = i;
				}
			}

			if (first > 0) {
				first--;
			}
			if (last < 2 * MODEHISTOSIZE) {
				last++;
			}

			if (first >= 0)
				for (int i = first; i <= last; i++) {
					printf("[%d] %ld \t= %ld\n", workerid,
					    (avg2 + i - MODEHISTOSIZE),
					    modehisto[i]);
				}
		}
	}

	return (s->settings->cycles + MEASURECYCLES);
}

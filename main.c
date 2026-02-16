/*
 * main.c
 *
 *  Created on: Aug 14, 2024
 *      Author: mizhka
 */

#define _GNU_SOURCE

#include <sys/param.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "progress.h"
#include "syscallmeter.h"
#include "w_clock_gettime.h"
#include "w_open.h"
#include "w_rename.h"
#include "w_write_sync.h"
#include "w_write_unlink.h"

/**
 * Default settings
 */
#define CPULIMIT_DEF  128
#define CYCLES_DEF    1024
#define FILECOUNT_DEF 4 * 1024
#define FILESIZE_DEF  32 * 1024
#define TEMPDIR_DEF   "temp_syscallmeter"
#define MODE_DEF      "open"

const struct meter_settings default_settings = { .cpu_limit = CPULIMIT_DEF,
	.cycles = CYCLES_DEF,
	.file_count = FILECOUNT_DEF,
	.file_size = FILESIZE_DEF,
	.temp_dir = TEMPDIR_DEF,
	.mode = MODE_DEF,
	.options = NULL,
	.ncpu = 0,
	.progress = 0 };

/* Context functions */
static struct meter_ctx *new_context();
static int init_directory(struct meter_ctx *mctx);
static int parse_opts(struct meter_ctx *mctx, int argc, char **argv);
static int lookup_test_callbacks(char *mode, worker_func *func);

int
main(int argc, char **argv)
{
	struct meter_ctx *ctx;
	int dirfd, err;

	pid_t child;
	worker_func func;

	const char delim[] = ",";
	char *saveptr, *option;

	ctx = new_context();
	if (ctx == NULL)
		return (-1);

	if (parse_opts(ctx, argc, argv) != 0)
		return (-1);

	err = init_directory(ctx);
	if (err)
		return -1;

	dirfd = open(ctx->settings->temp_dir, 0);
	if (dirfd < 0) {
		printf("Can\'t open directory\n");
		return -1;
	}
	printf("Created directory successfully\n");

	err = lookup_test_callbacks(ctx->settings->mode, &func);

	// Parse options and push them
	if (ctx->settings->options != NULL && func.opt != NULL) {
		option = strtok_r(ctx->settings->options, delim, &saveptr);
		while (option != NULL) {
			err = func.opt(option);
			if (err != 0) {
				return (err);
			}
			option = strtok_r(NULL, delim, &saveptr);
		}
	}

	// Initialize test
	err = func.init(ctx->settings, dirfd);
	//ctx->settings->ncpu = ctx->settings->cpu_limit;
	for (long i = 0; i < ctx->settings->ncpu; i++) {
		child = fork();
		if (child == 0) {
			cpu_set_t mask;
			double speed;
			long iter;
			struct meter_worker_state mystate;
			struct timespec ts_start, ts_end;

			mystate.my_stats = &(ctx->stats[i]);
			mystate.settings = ctx->settings;
			child = getpid();

			CPU_ZERO(&mask);
			CPU_SET(i, &mask);
			//err = sched_setaffinity(0, sizeof(cpu_set_t), &mask);
			if (err == -1) {
				printf("[%d] Can\'t set affinity\n", child);
				return -1;
			}
			printf("[%d] I\'m on CPU: %d\n", child, sched_getcpu());
			sem_post(&(ctx->sems->fork_completed));
			sem_wait(&(ctx->sems->starting));
			clock_gettime(CLOCK_MONOTONIC, &ts_start);

			iter = func.job(i, &mystate, dirfd);

			clock_gettime(CLOCK_MONOTONIC, &ts_end);

			ts_end.tv_sec = ts_end.tv_sec - ts_start.tv_sec;
			ts_end.tv_nsec = ts_end.tv_nsec - ts_start.tv_nsec;
			if (ts_end.tv_nsec < 0) {
				ts_end.tv_nsec += 1000 * 1000 * 1000;
				ts_end.tv_sec -= 1;
			}

			speed = (double)((long long)ts_end.tv_sec * 1000 *
					1000 * 1000 +
				    ts_end.tv_nsec) /
			    (double)(iter);

			printf(
			    "[%ld / %d] Worker is done with %ld in %lld.%.9ld sec (avg.time = %f ns)\n",
			    i, child, iter, (long long)ts_end.tv_sec,
			    ts_end.tv_nsec, speed);
			return 0;
		}
	}

	// Hackish - TODO: cosmetic change is required
	do {
		cpu_set_t mask;
		double speed;
		long iter;

		CPU_ZERO(&mask);
		CPU_SET(ctx->settings->ncpu, &mask);
		//err = sched_setaffinity(0, sizeof(cpu_set_t), &mask);
		if (err == -1) {
			printf("[main] Can\'t set affinity\n");
			return -1;
		}
	} while (1 == 0);

	for (int i = 0; i < ctx->settings->ncpu; i++) {
		sem_wait(&(ctx->sems->fork_completed));
	}

	if (ctx->settings->progress == 1)
		enable_progress(ctx->stats, ctx->settings->ncpu);

	printf("Starting...\n");
	for (int i = 0; i < ctx->settings->ncpu; i++) {
		sem_post(&(ctx->sems->starting));
	}

	do {
		child = wait(&child);
	} while (child > 0 || (child == -1 && errno == EINTR));

	printf("Done\n");
	return 0;
}

static int
parse_opts(struct meter_ctx *mctx, int argc, char **argv)
{
	int opt;
	while ((opt = getopt(argc, argv, "j:c:f:s:d:m:o:hp")) != -1) {
		switch (opt) {
		case 'j':
			mctx->settings->cpu_limit = strtol(optarg, NULL, 10);
			if (errno == EINVAL || errno == ERANGE ||
			    mctx->settings->cpu_limit <= 0) {
				printf(
				    "invalid arg %s for option -j expected integer grater than 0\n",
				    optarg);
				return -1;
			}
			break;

		case 'c':
			mctx->settings->cycles = strtol(optarg, NULL, 10);
			if (errno == EINVAL || errno == ERANGE ||
			    mctx->settings->cycles <= 0) {
				printf(
				    "invalid arg %s for option -c expected integer grater than 0\n",
				    optarg);
				return -1;
			}
			break;
		case 'f':
			mctx->settings->file_count = strtol(optarg, NULL, 10);
			if (errno == EINVAL || errno == ERANGE ||
			    mctx->settings->file_count <= 0) {
				printf(
				    "invalid arg %s for option -f expected integer grater than 0\n",
				    optarg);
				return -1;
			}
			break;
		case 's':
			mctx->settings->file_size = strtol(optarg, NULL, 10);
			if (errno == EINVAL || errno == ERANGE ||
			    mctx->settings->file_size <= 0) {
				printf(
				    "invalid arg %s for option -s expected integer grater than 0\n",
				    optarg);
				return -1;
			}
			break;
		case 'd':
			mctx->settings->temp_dir = optarg;
			break;
		case 'm':
			mctx->settings->mode = optarg;
			break;
		case 'o':
			mctx->settings->options = optarg;
			break;
		case 'p':
			mctx->settings->progress = 1;
			break;
		case 'h':
			printf(
			    "Usage:\n"
			    " -c number of cycles, default %d\n"
			    " -d directory path, default %s\n"
			    " -f number of files to create, default %d\n"
			    " -h no arg, use to dispay this message\n"
			    " -j number of max number of cpu, default %d\n"
			    " -m defines worker job, valid jobs: open, rename, write_unlink. Default %s\n"
			    " -s number of bytes in each file, default %d\n",
			    CYCLES_DEF, TEMPDIR_DEF, FILECOUNT_DEF,
			    CPULIMIT_DEF, MODE_DEF, FILESIZE_DEF);
			return -1;
		default:
			printf("unexpected argument %c", opt);
			return -1;
		}
	}

	mctx->settings->ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	printf("Found %ld cores\n", mctx->settings->ncpu);
	mctx->settings->ncpu = MIN(mctx->settings->ncpu - 1,
	    mctx->settings->cpu_limit);
	mctx->settings->ncpu = MIN(MAX_WORKERS, mctx->settings->ncpu);

	printf("Settings:\n");
	printf("\tCYCLES = %ld\n", mctx->settings->cycles);
	printf("\tWORKERS = %ld\n", mctx->settings->ncpu);
	printf("\tFILECOUNT = %d\n", mctx->settings->file_count);
	printf("\tFILESIZE = %lu\n", mctx->settings->file_size);

	return 0;
}

int
make_files(struct meter_settings *s, int dirfd)
{
	char *rndbytes;
	char filename[128];
	int fd;
	size_t written;

	rndbytes = alloc_rndbytes(s->file_size);
	if (rndbytes == NULL) {
		printf("Can\'t allocate random bytes");
		return -1;
	}

	for (int k = 0; k < s->file_count; k++) {
		sprintf(filename, FNAME, k);
		fd = openat(dirfd, filename, O_CREAT | O_TRUNC | O_RDWR, 0644);
		if (fd < 0) {
			printf("Can't create or open file %s", filename);
			return -1;
		}
		written = write(fd, rndbytes, s->file_size);
		if (written == -1) {
			printf("Can't write file %s: %s\n", filename,
			    strerror(errno));
			return -1;
		} else if (written != s->file_size) {
			printf("Can't fully write file %s: %zu\n", filename,
			    written);
			return -1;
		}

		close(fd);
	}

	free(rndbytes);
	return 0;
}

static int
init_directory(struct meter_ctx *mctx)
{
	int err;
	struct stat st;

	err = mkdir(mctx->settings->temp_dir, 0775);
	if (err) {
		if (errno != EEXIST) {
			printf("Can't create directory: %s\n", strerror(errno));
			return -1;
		}

		err = stat(mctx->settings->temp_dir, &st);
		if (err) {
			printf("Error on stat(TEMPDIR): %s\n", strerror(errno));
			return -1;
		}

		if (!S_ISDIR(st.st_mode)) {
			printf("Error: Found non-directory with name %s\n",
			    mctx->settings->temp_dir);
			return -1;
		}
		printf("Warning! Found old directory\n");
	}

	return 0;
}

char *
alloc_rndbytes(size_t size)
{
	char *ret;
	unsigned int g_seed;
	g_seed = random();

	ret = malloc(size);
	if (ret == NULL) {
		return ret;
	}

	for (int i = 0; i < size; i++) {
		g_seed = (214013 * g_seed + 2531011);
		ret[i] = 'A' + (char)(((g_seed >> 16) & 0x7FFF) % 0x1a);
	}

	return ret;
}

static struct meter_ctx *
new_context()
{
	struct meter_ctx *ctx;
	ctx = malloc(sizeof(struct meter_ctx));
	if (ctx == NULL) {
		perror("No memory to allocate context");
		goto fail;
	}

	ctx->settings = malloc(sizeof(struct meter_settings));
	if (ctx->settings == NULL) {
		perror("No memory to allocate context");
		goto free_ctx;
	}

	memcpy(ctx->settings, &default_settings, sizeof(struct meter_settings));

	ctx->sems = mmap(0, sizeof(struct meter_semaphores),
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);

	if (ctx->sems == MAP_FAILED) {
		printf("Can't mmap area: %s\n", strerror(errno));
		goto free_settings;
	}

	sem_init(&(ctx->sems->fork_completed), 1, 0);
	sem_init(&(ctx->sems->starting), 1, 0);

	ctx->stats = mmap(0, sizeof(struct meter_stats) * MAX_WORKERS,
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);

	if (ctx->stats == MAP_FAILED) {
		printf("Can't mmap area: %s\n", strerror(errno));
		goto free_settings;
	}

	for (int i = 0; i < MAX_WORKERS; i++) {
		ctx->stats[i].cycles = 0;
	}

	return (ctx);

free_sems:
	munmap(ctx->sems, sizeof(struct meter_semaphores));
free_settings:
	free(ctx->settings);
free_ctx:
	free(ctx);
fail:
	return (NULL);
}

static int
lookup_test_callbacks(char *mode, worker_func *func)
{
	if (strcmp(mode, "open") == 0) {
		func->init = &w_open_init;
		func->job = &w_open_job;
		func->opt = NULL;
	} else if (strcmp(mode, "rename") == 0) {
		func->init = &w_rename_init;
		func->job = &w_rename_job;
		func->opt = NULL;
	} else if (strcmp(mode, "write_unlink") == 0) {
		func->init = &w_write_unlink_init;
		func->job = &w_write_unlink_job;
		func->opt = NULL;
	} else if (strcmp(mode, "write_sync") == 0) {
		func->init = &w_write_sync_init;
		func->job = &w_write_sync_job;
		func->opt = &w_write_sync_option;
	} else if (strcmp(mode, "clock_gettime") == 0) {
		func->init = &w_clock_gettime_init;
		func->job = &w_clock_gettime_job;
		func->opt = &w_clock_gettime_opt;
	} else {
		printf("Unknown worker job (-m): %s,"
		       " use -h to see valid job names\n",
		    mode);
		return -1;
	}
	return (0);
}

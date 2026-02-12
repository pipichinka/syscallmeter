#define _GNU_SOURCE
#include <sys/param.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "syscallmeter.h"
#include "w_write_sync.h"
#include <x86intrin.h>
#define CHUNKSIZE 64 * 1024

/*
 * Joined - single hold of lock for write and sync
 * Dual - separate locks for write and sync
 * OnlyWrite - lock for write and no lock for sync
 * ExWr_ShSy - exclusive lock for write and shared lock for sync
 */
enum w_lockmode { JOINED, DUAL, ONLYWRITE, EXWR_SHSY };

#define DO_LOCK(_p)                                                                \
	do {                                                                       \
		err = sem_wait((_p));                                              \
		if (err != 0) {                                                    \
			switch (err) {                                             \
			case EINVAL:                                               \
				printf(                                            \
				    "The argument points to invalid semaphore\n"); \
				exit(-1);                                          \
			case EINTR:                                                \
				exit(-1);                                          \
			}                                                          \
		}                                                                  \
	} while (1 == 0);

#define DO_UNLOCK(_p)                                                              \
	do {                                                                       \
		err = sem_post((_p));                                              \
		if (err != 0) {                                                    \
			switch (err) {                                             \
			case EINVAL:                                               \
				printf(                                            \
				    "The argument points to invalid semaphore\n"); \
				exit(-1);                                          \
			case EOVERFLOW:                                            \
				printf("Unexpected overflow of semaphore\n");      \
				exit(-1);                                          \
			}                                                          \
		}                                                                  \
	} while (1 == 0);

#define DO_WORK(_p)                                             \
	do {                                                    \
		unsigned long long start, end;                  \
		start = __rdtsc();                              \
		unsigned long long wait_cycles = 3000 * ((_p)); \
		do {                                            \
			end = __rdtsc();                        \
		} while ((end - start) < wait_cycles);          \
	} while (1 == 0);

typedef struct workers_sharedmem {
	sem_t mx_write;
	sem_t mx_sync;
	unsigned long position_write;
	unsigned long position_sync;
} workers_sharedmem_t;

/* Global variables - constant over time */
typedef struct workers_test_params {
	enum w_lockmode w_mode;
	int sync_concurrency;
	int direct;
	int shift_position;
} workers_test_params_t;

struct workers_sharedmem *w_state = NULL;
struct workers_test_params w_params = { .sync_concurrency = 1,
	.w_mode = JOINED,
	.direct = 0,
	.shift_position = 0 };

#define WORKER_FILE_INDEX(_s) (w_state->position_write / _s->settings->file_size)
#define WORKER_FILE_INDEX_SYNC(_pos,_s) (_pos / _s->settings->file_size)

static inline char
pg_atomic_compare_exchange_u64_impl(volatile unsigned long *ptr,
									unsigned long *expected, unsigned long newval)
{
	char	ret;


	/*
	 * Perform cmpxchg and use the zero flag which it implicitly sets when
	 * equal to measure the success.
	 */
	__asm__ __volatile__(
		"	lock				\n"
		"	cmpxchgq	%4,%5	\n"
		"   setz		%2		\n"
:		"=a" (*expected), "=m"(*ptr), "=q" (ret)
:		"a" (*expected), "r" (newval), "m"(*ptr)
:		"memory", "cc");
	return ret;
}


static inline void
update_fsync_pos(unsigned long new_pos)
{
	unsigned long current = w_state->position_sync;
	while (current < new_pos && !pg_atomic_compare_exchange_u64_impl(&w_state->position_sync, &current, new_pos));
}


int
w_write_sync_option(char *option)
{
	if (strcmp(option, "joined") == 0) {
		w_params.w_mode = JOINED;
	} else if (strcmp(option, "dual") == 0) {
		w_params.w_mode = DUAL;
	} else if (strcmp(option, "onlywrite") == 0) {
		w_params.w_mode = ONLYWRITE;
	} else if (strcmp(option, "sharesync8") == 0) {
		w_params.w_mode = EXWR_SHSY;
		w_params.sync_concurrency = 8;
	} else if (strcmp(option, "sharesync16") == 0) {
		w_params.w_mode = EXWR_SHSY;
		w_params.sync_concurrency = 16;
	} else if (strcmp(option, "direct") == 0) {
		w_params.direct = 1;
	} else if (strcmp(option, "doublelast") == 0) {
		w_params.shift_position = 8 * 1024;
	} else {
		printf("unexpected option: %s\n", option);
		return -1;
	}
	return (0);
}

int
w_write_sync_init(struct meter_settings *s, int dirfd)
{
	w_state = mmap(0, sizeof(struct workers_sharedmem),
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);

	if (w_state == MAP_FAILED) {
		return (-1);
	}

	w_state->position_write = 0;
	w_state->position_sync = 0;

	sem_init(&w_state->mx_write, 1, 1);
	sem_init(&w_state->mx_sync, 1, w_params.sync_concurrency);

	if (make_files(s, dirfd))
		return (-1);
	printf("Created files successfully\n");

	return (0);
}

long
w_write_sync_job(int workerid, struct meter_worker_state *s, int dirfd)
{
	char filename[128];
	int fd, err, curr_index, flags;
	ssize_t write_res;
	long position_to_add;
	unsigned long needed_pos;
	long write_pos_diff;
	unsigned long save_write_pos;

	curr_index = WORKER_FILE_INDEX(s);

	char *data = alloc_rndbytes(s->settings->file_size);
	sprintf(filename, "temp_syscallmeter/file_%d", curr_index);

	flags = O_CREAT | O_RDWR | (((w_params.direct != 0) ? O_DIRECT : 0));
	fd = open(filename, flags, 0644);
	srandom(workerid);

	for (;;) {
		position_to_add = random() % CHUNKSIZE;
		needed_pos = w_state->position_write + position_to_add;
		if (WORKER_FILE_INDEX(s) >= s->settings->file_count)
			break;

		DO_WORK(20);

		DO_LOCK(&w_state->mx_write);

		if (w_state->position_sync >= needed_pos)
		{
			//__asm__ __volatile__ ("lock; addl $0,0(%%rsp)" : : : "memory", "cc");
			DO_UNLOCK(&w_state->mx_write);
			continue;
		}
		write_pos_diff = (long) needed_pos - (long) w_state->position_write;
		while (write_pos_diff > 0)
		{
			int index_to_open = WORKER_FILE_INDEX(s);
			unsigned long pos_in_file = w_state->position_write % s->settings->file_size;
			unsigned long bytes_to_write = MIN(needed_pos, (index_to_open + 1) * s->settings->file_size) - w_state->position_write;
			if (curr_index != index_to_open)
			{
				close(fd);
				// TODO: err check
				curr_index = WORKER_FILE_INDEX(s);
				sprintf(filename, "temp_syscallmeter/file_%d", curr_index);
				fd = open(filename, flags, 0644);
				// TODO: err check
			}
			pwrite(fd, &data[pos_in_file], bytes_to_write, pos_in_file);
			// TODO: err check
			w_state->position_write += bytes_to_write;
			write_pos_diff -= bytes_to_write;
			if (index_to_open != WORKER_FILE_INDEX(s))
			{
				fdatasync(fd);
				update_fsync_pos(w_state->position_write);
			}
		}
		switch (w_params.w_mode) {
		case DUAL:
		case EXWR_SHSY:
			DO_LOCK(&w_state->mx_sync);
			DO_UNLOCK(&w_state->mx_write);
			break;
		case ONLYWRITE:
			DO_UNLOCK(&w_state->mx_write);
			break;
		default:
			break;
		}
		save_write_pos = w_state->position_write;
		if (w_state->position_sync < needed_pos)
		{
			if (curr_index != WORKER_FILE_INDEX_SYNC(needed_pos, s))
			{
				close(fd);
				// TODO: err check
				curr_index = WORKER_FILE_INDEX_SYNC(needed_pos, s);
				sprintf(filename, "temp_syscallmeter/file_%d", curr_index);
				fd = open(filename, flags, 0644);
				// TODO: err check
			}
			err = fdatasync(fd);
			if (err != 0) {
				printf("fdatasync failed with error %s\n",
			    	strerror(errno));
				exit(1);
			}
			save_write_pos = MIN(save_write_pos, (curr_index + 1) * s->settings->file_size);
			update_fsync_pos(save_write_pos);
		}
		s->my_stats->cycles++;

		switch (w_params.w_mode) {
		case JOINED:
			DO_UNLOCK(&w_state->mx_write);
			break;
		case DUAL:
		case EXWR_SHSY:
			DO_UNLOCK(&w_state->mx_sync);
			break;
		default:
			break;
		}
	}

	free(data);
	close(fd);

	return (s->my_stats->cycles);
}

#ifndef _SYSCALLMETER_H_
#define _SYSCALLMETER_H_

#include <sys/param.h>
#include <semaphore.h>

#define FNAME	"file_%d"
#define MAX_WORKERS 256

/**
 * Settings
 */
typedef struct meter_settings {
	int cpu_limit;
	long cycles;
	int file_count;
	unsigned long file_size;
	char *temp_dir;
	char *mode;
	char *options;
	long ncpu;
	char progress;
} meter_setting_t;

typedef struct meter_stats {
    long cycles;
} metet_stats_t;

typedef struct meter_worker_state {
    struct meter_settings *settings;
    struct meter_stats *my_stats;
    void *opaque;
} meter_worker_state_t;

typedef struct meter_semaphores {
	sem_t fork_completed;
	sem_t starting;
} meter_semaphores_t;

typedef struct meter_ctx {
	struct meter_settings *settings; /* Global params  */
	struct meter_semaphores *sems;	 /* Semaphores to launch workers */
	struct meter_stats *stats;	 /* Actual results */
} meter_ctx_t;

typedef int (*worker_init_t)(struct meter_settings *, int);
/*
 * args: workerid, ncpu, dirfd
 * returns: positive - amount of iterations
 *          negative - error
 */
typedef long (*worker_job_t)(int, struct meter_worker_state *, int);
typedef int (*worker_opt_t)(char*);

typedef struct {
	worker_init_t init;
	worker_opt_t opt;
	worker_job_t job;
} worker_func;

int make_files(struct meter_settings *, int);
char *alloc_rndbytes(size_t);

#endif /* !_SYSCALLMETER_H_ */

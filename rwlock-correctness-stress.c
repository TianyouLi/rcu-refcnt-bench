/*
 * rwlock-correctness-stress.c — Stress correctness of rwlock under various
 * reader/writer ratios.
 *
 * Verifies:
 *   1. No data corruption (writers modify shared state, readers validate)
 *   2. No deadlocks (test completes within timeout)
 *   3. Writer exclusivity (no concurrent writers or readers during write)
 *   4. Proper phase transitions under all write ratios
 *
 * Build:
 *   gcc -O2 -pthread -o rwlock-correctness-stress rwlock-correctness-stress.c
 *
 * Run:
 *   ./rwlock-correctness-stress [threads] [duration_sec] [write_pct]
 *   ./rwlock-correctness-stress 64 5 50   # 50% writers, heavy contention
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>

#define CACHE_LINE 64
#define DATA_SIZE 64

static pthread_rwlock_t g_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static atomic_int g_running;
static pthread_barrier_t g_barrier;

/* Protected shared state: a "consistent" data structure.
 * Writers set all entries to the same value (their thread_id).
 * Readers verify all entries are the same (no torn writes).  */
static int g_data[DATA_SIZE] __attribute__((aligned(CACHE_LINE)));

/* Track concurrency violations */
static atomic_int g_active_writers;
static atomic_int g_active_readers;
static atomic_long g_violations;

/* Per-thread stats */
struct thread_stats {
    unsigned long reads;
    unsigned long writes;
    unsigned long read_violations;
    unsigned long write_violations;
    char pad[CACHE_LINE - 4 * sizeof(unsigned long)];
} __attribute__((aligned(CACHE_LINE)));

static inline unsigned int xorshift32(unsigned int *state) {
    unsigned int x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

struct bench_config {
    int thread_id;
    int write_pct;
    struct thread_stats *stats;
};

static void *worker(void *arg) {
    struct bench_config *cfg = arg;
    unsigned int rng = cfg->thread_id + 1;
    struct thread_stats *stats = cfg->stats;

    pthread_barrier_wait(&g_barrier);

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        int is_write = (cfg->write_pct > 0) &&
                       ((xorshift32(&rng) % 100) < (unsigned)cfg->write_pct);

        if (is_write) {
            pthread_rwlock_wrlock(&g_rwlock);

            /* Check exclusivity: no other writers or readers */
            int prev_writers = atomic_fetch_add(&g_active_writers, 1);
            int cur_readers = atomic_load(&g_active_readers);
            if (prev_writers != 0 || cur_readers != 0) {
                stats->write_violations++;
                atomic_fetch_add(&g_violations, 1);
            }

            /* Write: set all entries to our thread_id */
            int val = cfg->thread_id + 1;
            for (int i = 0; i < DATA_SIZE; i++)
                g_data[i] = val;

            atomic_fetch_sub(&g_active_writers, 1);
            pthread_rwlock_unlock(&g_rwlock);
            stats->writes++;
        } else {
            pthread_rwlock_rdlock(&g_rwlock);

            /* Check: no writer active during our read */
            atomic_fetch_add(&g_active_readers, 1);
            int cur_writers = atomic_load(&g_active_writers);
            if (cur_writers != 0) {
                stats->read_violations++;
                atomic_fetch_add(&g_violations, 1);
            }

            /* Read: verify all entries are consistent (same value) */
            int first = g_data[0];
            for (int i = 1; i < DATA_SIZE; i++) {
                if (g_data[i] != first) {
                    stats->read_violations++;
                    atomic_fetch_add(&g_violations, 1);
                    break;
                }
            }

            atomic_fetch_sub(&g_active_readers, 1);
            pthread_rwlock_unlock(&g_rwlock);
            stats->reads++;
        }
    }

    return NULL;
}

int main(int argc, char **argv) {
    int num_threads = 64;
    int duration_sec = 5;
    int write_pct = 10;

    if (argc > 1) num_threads = atoi(argv[1]);
    if (argc > 2) duration_sec = atoi(argv[2]);
    if (argc > 3) write_pct = atoi(argv[3]);

    int cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_threads > cores) num_threads = cores;
    if (num_threads < 1) num_threads = 1;

    /* Initialize shared data */
    memset(g_data, 0, sizeof(g_data));

    printf("rwlock correctness stress test\n");
    printf("  threads=%d  duration=%ds  write_pct=%d%%  cores=%d\n\n",
           num_threads, duration_sec, write_pct, cores);

    pthread_t *threads = calloc(num_threads, sizeof(pthread_t));
    struct bench_config *cfgs = calloc(num_threads, sizeof(struct bench_config));
    struct thread_stats *stats = aligned_alloc(CACHE_LINE,
        num_threads * sizeof(struct thread_stats));
    memset(stats, 0, num_threads * sizeof(struct thread_stats));

    pthread_barrier_init(&g_barrier, NULL, num_threads + 1);
    atomic_store(&g_running, 1);

    for (int i = 0; i < num_threads; i++) {
        cfgs[i].thread_id = i;
        cfgs[i].write_pct = write_pct;
        cfgs[i].stats = &stats[i];
        pthread_create(&threads[i], NULL, worker, &cfgs[i]);
    }

    pthread_barrier_wait(&g_barrier);
    sleep(duration_sec);
    atomic_store(&g_running, 0);

    unsigned long total_reads = 0, total_writes = 0;
    unsigned long total_read_violations = 0, total_write_violations = 0;

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_reads += stats[i].reads;
        total_writes += stats[i].writes;
        total_read_violations += stats[i].read_violations;
        total_write_violations += stats[i].write_violations;
    }

    printf("Results:\n");
    printf("  Total reads:  %lu\n", total_reads);
    printf("  Total writes: %lu\n", total_writes);
    printf("  Read violations:  %lu\n", total_read_violations);
    printf("  Write violations: %lu\n", total_write_violations);
    printf("\n");

    if (total_read_violations == 0 && total_write_violations == 0) {
        printf("PASS: No correctness violations detected.\n");
    } else {
        printf("FAIL: %lu violations detected!\n",
               total_read_violations + total_write_violations);
        pthread_barrier_destroy(&g_barrier);
        free(threads);
        free(cfgs);
        free(stats);
        return 1;
    }

    pthread_barrier_destroy(&g_barrier);
    free(threads);
    free(cfgs);
    free(stats);
    return 0;
}

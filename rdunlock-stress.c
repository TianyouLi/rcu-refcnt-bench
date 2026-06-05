/*
 * rdunlock-stress.c — Stress the rwlock rdunlock path specifically.
 *
 * Short critical section (just increment a counter) so that unlock
 * contention dominates.  This is where the rcuref optimization helps
 * most: N threads doing CAS retry loops on __readers simultaneously.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#define CACHE_LINE 64

static pthread_rwlock_t g_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static atomic_int g_running;
static pthread_barrier_t g_barrier;

static volatile int g_shared;

struct thread_result {
    unsigned long ops;
    char pad[CACHE_LINE - sizeof(unsigned long)];
} __attribute__((aligned(CACHE_LINE)));

struct bench_config {
    int thread_id;
    struct thread_result *result;
};

static void *worker(void *arg) {
    struct bench_config *cfg = arg;
    unsigned long ops = 0;
    int sink = 0;

    pthread_barrier_wait(&g_barrier);

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        pthread_rwlock_rdlock(&g_rwlock);
        sink += g_shared;
        pthread_rwlock_unlock(&g_rwlock);
        ops++;
    }

    cfg->result->ops = ops;
    return (void *)(long)sink;
}

static double run_bench(int num_threads, int duration_sec) {
    pthread_t *threads = calloc(num_threads, sizeof(pthread_t));
    struct bench_config *cfgs = calloc(num_threads, sizeof(struct bench_config));
    struct thread_result *results = aligned_alloc(CACHE_LINE,
        num_threads * sizeof(struct thread_result));
    memset(results, 0, num_threads * sizeof(struct thread_result));

    pthread_barrier_init(&g_barrier, NULL, num_threads + 1);
    atomic_store(&g_running, 1);

    for (int i = 0; i < num_threads; i++) {
        cfgs[i].thread_id = i;
        cfgs[i].result = &results[i];
        pthread_create(&threads[i], NULL, worker, &cfgs[i]);
    }

    pthread_barrier_wait(&g_barrier);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    sleep(duration_sec);
    atomic_store(&g_running, 0);
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    unsigned long total_ops = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_ops += results[i].ops;
    }

    pthread_barrier_destroy(&g_barrier);
    free(threads);
    free(cfgs);
    free(results);

    return total_ops / elapsed;
}

int main(int argc, char **argv) {
    int max_threads = 160;
    int duration_sec = 2;

    if (argc > 1) max_threads = atoi(argv[1]);
    if (argc > 2) duration_sec = atoi(argv[2]);

    int cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (max_threads > cores) max_threads = cores;

    printf("rdunlock stress benchmark (minimal critical section)\n");
    printf("  max_threads=%d  duration=%ds  cores=%d\n\n",
           max_threads, duration_sec, cores);
    printf("%6s  %15s\n", "threads", "rwlock (Mops/s)");
    printf("%6s  %15s\n", "------", "---------------");

    int thread_counts[] = {1, 2, 4, 8, 16, 32, 64, 96, 128, 160};
    int n_counts = sizeof(thread_counts) / sizeof(thread_counts[0]);

    for (int i = 0; i < n_counts; i++) {
        int t = thread_counts[i];
        if (t > max_threads) break;

        double ops = run_bench(t, duration_sec);
        printf("%6d  %15.2f\n", t, ops / 1e6);
    }

    return 0;
}

/*
 * asymmetric-rwlock-bench.c — Test if non-atomic read-side counting
 * can achieve true O(1) per-reader cost.
 *
 * Key insight: if readers use per-CPU or per-thread counters (just
 * a plain MOV to their own cache line), there is ZERO cross-core
 * traffic for the read-side lock/unlock.  Only writers pay the cost
 * (they must sum all per-thread counters).
 *
 * This is the principle behind:
 *   - Linux percpu_ref (per-CPU counters)
 *   - Linux brlock (big-reader lock)
 *   - userspace urcu (user-space RCU)
 *
 * Implementation: per-thread reader count on separate cache lines.
 * - rdlock: thread-local_count++ (plain MOV, no LOCK, no bounce)
 * - rdunlock: thread-local_count-- (plain MOV, no LOCK, no bounce)
 * - wrlock: set writer_flag, then for each thread: wait until count==0
 * - wrunlock: clear writer_flag, signal readers
 *
 * This gives O(1) reader-side vs pthread_rwlock's O(N) cache bouncing.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <emmintrin.h>

#define CACHE_LINE 64
#define MAX_THREADS 256

/* Per-thread reader counter — each on its own cache line to avoid false sharing.
 * Readers only touch their own entry (plain store/load, no atomic needed).
 * Writers scan all entries (expensive for writer, free for readers). */
struct percpu_slot {
    volatile unsigned int count;
    char pad[CACHE_LINE - sizeof(unsigned int)];
} __attribute__((aligned(CACHE_LINE)));

static struct percpu_slot g_reader_counts[MAX_THREADS]
    __attribute__((aligned(CACHE_LINE)));

/* Writer state: readers check this with a load before proceeding. */
static atomic_int g_writer_active;
static pthread_mutex_t g_writer_mutex = PTHREAD_MUTEX_INITIALIZER;

/* The per-thread rwlock interface */
static inline void percpu_rdlock(int tid) {
    /* Plain store to our own cache line — no bus traffic */
    g_reader_counts[tid].count = 1;
    /* Full fence to ensure writer sees our count before we proceed.
     * On x86, a compiler barrier suffices because stores are not reordered
     * past loads (TSO). But we need to ensure the writer_active load
     * happens after our count store is visible. */
    atomic_thread_fence(memory_order_seq_cst);
    /* If writer is active, undo and wait */
    while (__builtin_expect(atomic_load_explicit(&g_writer_active,
                            memory_order_acquire), 0)) {
        g_reader_counts[tid].count = 0;
        while (atomic_load_explicit(&g_writer_active, memory_order_relaxed))
            _mm_pause();
        g_reader_counts[tid].count = 1;
        atomic_thread_fence(memory_order_seq_cst);
    }
}

static inline void percpu_rdunlock(int tid) {
    /* Plain store — no atomic, no bus traffic */
    atomic_thread_fence(memory_order_release);
    g_reader_counts[tid].count = 0;
}

static inline void percpu_wrlock(int num_threads) {
    pthread_mutex_lock(&g_writer_mutex);
    atomic_store_explicit(&g_writer_active, 1, memory_order_release);
    /* Full fence, then wait for all readers to drain */
    atomic_thread_fence(memory_order_seq_cst);
    for (int i = 0; i < num_threads; i++) {
        while (g_reader_counts[i].count != 0)
            _mm_pause();
    }
}

static inline void percpu_wrunlock(void) {
    atomic_store_explicit(&g_writer_active, 0, memory_order_release);
    pthread_mutex_unlock(&g_writer_mutex);
}

/* Benchmark infrastructure */
static atomic_int g_running;
static pthread_barrier_t g_barrier;
static volatile int g_shared;
static int g_num_threads;

struct thread_result {
    unsigned long ops;
    char pad[CACHE_LINE - sizeof(unsigned long)];
} __attribute__((aligned(CACHE_LINE)));

struct bench_config {
    int thread_id;
    int write_pct;
    struct thread_result *result;
};

static inline unsigned int xorshift32(unsigned int *state) {
    unsigned int x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Worker using per-thread counter lock */
static void *worker_percpu(void *arg) {
    struct bench_config *cfg = arg;
    unsigned long ops = 0;
    unsigned int rng = cfg->thread_id + 1;
    int sink = 0;

    pthread_barrier_wait(&g_barrier);

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        int is_write = (cfg->write_pct > 0) &&
                       ((xorshift32(&rng) % 100) < (unsigned)cfg->write_pct);
        if (is_write) {
            percpu_wrlock(g_num_threads);
            g_shared = cfg->thread_id;
            percpu_wrunlock();
        } else {
            percpu_rdlock(cfg->thread_id);
            sink += g_shared;
            percpu_rdunlock(cfg->thread_id);
        }
        ops++;
    }

    cfg->result->ops = ops;
    return (void *)(long)sink;
}

/* Worker using pthread_rwlock for comparison */
static pthread_rwlock_t g_rwlock = PTHREAD_RWLOCK_INITIALIZER;

static void *worker_pthread(void *arg) {
    struct bench_config *cfg = arg;
    unsigned long ops = 0;
    unsigned int rng = cfg->thread_id + 1;
    int sink = 0;

    pthread_barrier_wait(&g_barrier);

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        int is_write = (cfg->write_pct > 0) &&
                       ((xorshift32(&rng) % 100) < (unsigned)cfg->write_pct);
        if (is_write) {
            pthread_rwlock_wrlock(&g_rwlock);
            g_shared = cfg->thread_id;
            pthread_rwlock_unlock(&g_rwlock);
        } else {
            pthread_rwlock_rdlock(&g_rwlock);
            sink += g_shared;
            pthread_rwlock_unlock(&g_rwlock);
        }
        ops++;
    }

    cfg->result->ops = ops;
    return (void *)(long)sink;
}

static double run_bench(int num_threads, int duration_sec, int write_pct,
                        int use_percpu) {
    pthread_t *threads = calloc(num_threads, sizeof(pthread_t));
    struct bench_config *cfgs = calloc(num_threads, sizeof(struct bench_config));
    struct thread_result *results = aligned_alloc(CACHE_LINE,
        num_threads * sizeof(struct thread_result));
    memset(results, 0, num_threads * sizeof(struct thread_result));
    memset((void *)g_reader_counts, 0, sizeof(g_reader_counts));

    g_num_threads = num_threads;
    pthread_barrier_init(&g_barrier, NULL, num_threads + 1);
    atomic_store(&g_running, 1);

    for (int i = 0; i < num_threads; i++) {
        cfgs[i].thread_id = i;
        cfgs[i].write_pct = write_pct;
        cfgs[i].result = &results[i];
        pthread_create(&threads[i], NULL,
                       use_percpu ? worker_percpu : worker_pthread, &cfgs[i]);
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
    int write_pct = 0;

    if (argc > 1) max_threads = atoi(argv[1]);
    if (argc > 2) duration_sec = atoi(argv[2]);
    if (argc > 3) write_pct = atoi(argv[3]);

    int cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (max_threads > cores) max_threads = cores;
    if (max_threads > MAX_THREADS) max_threads = MAX_THREADS;

    printf("Per-CPU reader lock vs pthread_rwlock (write_pct=%d%%)\n", write_pct);
    printf("  max_threads=%d  duration=%ds  cores=%d\n\n", max_threads, duration_sec, cores);
    printf("%6s  %15s  %15s  %10s\n",
           "threads", "pthread_rwlock", "percpu_rwlock", "speedup");
    printf("%6s  %15s  %15s  %10s\n",
           "------", "---------------", "---------------", "----------");

    int thread_counts[] = {1, 2, 4, 8, 16, 32, 64, 96, 128, 160};
    int n_counts = sizeof(thread_counts) / sizeof(thread_counts[0]);

    for (int i = 0; i < n_counts; i++) {
        int t = thread_counts[i];
        if (t > max_threads) break;

        double pthread_ops = run_bench(t, duration_sec, write_pct, 0);
        double percpu_ops = run_bench(t, duration_sec, write_pct, 1);
        double speedup = percpu_ops / pthread_ops;

        printf("%6d  %12.2f M  %12.2f M  %9.1fx\n",
               t, pthread_ops / 1e6, percpu_ops / 1e6, speedup);
    }

    return 0;
}

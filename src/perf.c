#include "perf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERF_RING_SIZE 4096
#define PERF_REPORT_INTERVAL 200   /* report more frequently during tuning */

static perf_sample_t g_perf_ring[PERF_RING_SIZE] __attribute__((aligned(64)));
static volatile uint32_t g_perf_idx = 0;
static uint32_t g_perf_count = 0;

static inline uint64_t u64_min(uint64_t a, uint64_t b) { return a < b ? a : b; }
static inline uint64_t u64_max(uint64_t a, uint64_t b) { return a > b ? a : b; }

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static uint64_t percentile(uint64_t *arr, int n, double pct) {
    if (n == 0) return 0;
    double idx = pct * (double)(n - 1);
    int lo = (int)idx;
    int hi = lo + 1;
    if (hi >= n) hi = n - 1;
    double frac = idx - (double)lo;
    return (uint64_t)((double)arr[lo] * (1.0 - frac) + (double)arr[hi] * frac);
}

static void report_field(const char *name, uint64_t *vals, int n) {
    if (n == 0) {
        fprintf(stderr, " %s: N/A", name);
        return;
    }
    qsort(vals, (size_t)n, sizeof(uint64_t), cmp_u64);
    uint64_t p50 = percentile(vals, n, 0.50);
    uint64_t p99 = percentile(vals, n, 0.99);
    uint64_t mx = vals[n - 1];
    fprintf(stderr, " %s: p50=%.3f p99=%.3f max=%.3f",
            name,
            (double)p50 / 1000000.0,
            (double)p99 / 1000000.0,
            (double)mx / 1000000.0);
}

void perf_record(const perf_sample_t *s) {
    uint32_t idx = __atomic_fetch_add(&g_perf_idx, 1, __ATOMIC_RELAXED);
    g_perf_ring[idx % PERF_RING_SIZE] = *s;
    uint32_t c = __atomic_add_fetch(&g_perf_count, 1, __ATOMIC_RELAXED);
    if (c % PERF_REPORT_INTERVAL == 0) {
        perf_report();
    }
}

void perf_report(void) {
    uint32_t count = g_perf_count;
    int n = (int)(count < PERF_RING_SIZE ? count : PERF_RING_SIZE);
    uint32_t base = g_perf_idx;

    uint64_t *recv_vals = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *json_vals = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *knn_vals = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *send_vals = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *total_vals = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));

    uint64_t clusters_sum = 0, vectors_sum = 0;
    uint64_t repair_count = 0, early_exit_count = 0;

    if (!recv_vals || !json_vals || !knn_vals || !send_vals || !total_vals) {
        free(recv_vals); free(json_vals); free(knn_vals); free(send_vals); free(total_vals);
        return;
    }

    for (int i = 0; i < n; i++) {
        uint32_t ri = (base - (uint32_t)n + (uint32_t)i) % PERF_RING_SIZE;
        recv_vals[i] = g_perf_ring[ri].recv_ns;
        json_vals[i] = g_perf_ring[ri].json_ns;
        knn_vals[i] = g_perf_ring[ri].knn_ns;
        send_vals[i] = g_perf_ring[ri].send_ns;
        total_vals[i] = g_perf_ring[ri].total_ns;

        clusters_sum += g_perf_ring[ri].clusters_probed;
        vectors_sum  += g_perf_ring[ri].vectors_scanned;
        repair_count += g_perf_ring[ri].repair_triggered;
        early_exit_count += g_perf_ring[ri].early_exit_taken;
    }

    fprintf(stderr, "[PERF] N=%u |", count);
    report_field("recv", recv_vals, n);
    report_field("json", json_vals, n);
    report_field("knn", knn_vals, n);
    report_field("send", send_vals, n);
    report_field("total", total_vals, n);

    double avg_clusters = (double)clusters_sum / (double)n;
    double avg_vectors  = (double)vectors_sum  / (double)n;
    double repair_pct   = (double)repair_count   * 100.0 / (double)n;
    double early_pct    = (double)early_exit_count * 100.0 / (double)n;

    fprintf(stderr, " | knn_detail: avg_clusters=%.1f avg_vecs=%.1f repair=%.1f%% early_exit=%.1f%%\n",
            avg_clusters, avg_vectors, repair_pct, early_pct);

    free(recv_vals); free(json_vals); free(knn_vals); free(send_vals); free(total_vals);
}

void perf_force_report(void) {
    perf_report();
}

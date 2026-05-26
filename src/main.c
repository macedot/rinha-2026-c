#define _GNU_SOURCE
#include "config.h"
#include "knn.h"
#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

static struct timespec ts_start;

static void ts_init(void) {
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
}

static long ts_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - ts_start.tv_sec) * 1000L
         + (now.tv_nsec - ts_start.tv_nsec) / 1000000L;
}
static void maybe_pin_cpu(const config_t *cfg) {
    cpu_set_t avail;
    CPU_ZERO(&avail);
    if (sched_getaffinity(0, sizeof(avail), &avail) != 0) return;

    int ncpu = CPU_COUNT(&avail);
    fprintf(stderr, "available CPUs: %d\n", ncpu);

    /* Only pin on 1-2 core machines to avoid scheduler migration thrashing */
    if (ncpu > 2) return;

    /* Pick core based on UDS path hash so api1 and api2 get different cores */
    unsigned long h = 5381;
    for (const char *p = cfg->uds_path; *p; p++)
        h = ((h << 5) + h) + (unsigned char)*p;
    int core = (int)(h % (unsigned long)ncpu);

    cpu_set_t pin;
    CPU_ZERO(&pin);
    CPU_SET(core, &pin);
    if (sched_setaffinity(0, sizeof(pin), &pin) == 0)
        fprintf(stderr, "pinned to CPU %d (2-core mode)\n", core);
    else
        fprintf(stderr, "pin to CPU %d failed\n", core);
}

int main(void) {
    ts_init();
    config_t cfg = config_load();

    fprintf(stderr, "[%ldms] engine: C/AVX2 KNN implementation\n", ts_ms());

    maybe_pin_cpu(&cfg);

    fprintf(stderr, "[%ldms] loading index: %s\n", ts_ms(), cfg.index_path);
    if (rinha_load_index(cfg.index_path) != 0) {
        fprintf(stderr, "[%ldms] failed to load index: %s\n", ts_ms(), cfg.index_path);
        config_free(&cfg);
        return 1;
    }
    rinha_set_search_params(cfg.ivf_nprobe, cfg.ivf_full_nprobe, cfg.candidates);

    fprintf(stderr, "[%ldms] index loaded, warming caches...\n", ts_ms());
    {
        uint32_t state = 0x12345678;
        for (int i = 0; i < 500; i++) {
            float q[16];
            for (int j = 0; j < 16; j++) {
                state = state * 1664525 + 1013904223;
                q[j] = (float)(state >> 8) / (float)(1u << 24);
            }
            float fs;
            rinha_search(q, &fs);
        }
    }
    fprintf(stderr, "[%ldms] cache warmup done\n", ts_ms());

    fprintf(stderr, "[%ldms] starting server...\n", ts_ms());
    int rc = server_run(&cfg);
    config_free(&cfg);
    return rc;
}

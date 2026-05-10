#define _GNU_SOURCE
#include "config.h"
#include "bridge.h"
#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>

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
    config_t cfg = config_load();

    fprintf(stderr, "engine: C/AVX2 bridge implementation\n");

    maybe_pin_cpu(&cfg);

    if (rinha_load_index(cfg.index_path) != 0) {
        fprintf(stderr, "failed to load index: %s\n", cfg.index_path);
        config_free(&cfg);
        return 1;
    }
    rinha_set_search_params(cfg.ivf_nprobe, cfg.ivf_full_nprobe, cfg.candidates);

    fprintf(stderr, "warming caches...\n");
    {
        uint32_t state = 0x12345678;
        for (int i = 0; i < 500; i++) {
            float q[14];
            for (int j = 0; j < 14; j++) {
                state = state * 1664525 + 1013904223;
                q[j] = (float)(state >> 8) / (float)(1u << 24);
            }
            rinha_search(q);
        }
    }
    fprintf(stderr, "cache warmup done\n");

    int rc = server_run(&cfg);
    config_free(&cfg);
    return rc;
}

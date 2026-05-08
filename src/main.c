#include "config.h"
#include "bridge.h"
#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(void) {
    config_t cfg = config_load();

    fprintf(stderr, "engine: C/AVX2 bridge implementation\n");
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

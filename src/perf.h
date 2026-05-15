#ifndef PERF_H
#define PERF_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t recv_ns;
    uint64_t json_ns;
    uint64_t knn_ns;
    uint64_t send_ns;
    uint64_t total_ns;
} perf_sample_t;

void perf_record(const perf_sample_t *s);

void perf_report(void);

#endif

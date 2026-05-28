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

    /* KNN internals for identifying latency offenders */
    uint32_t clusters_probed;     /* total clusters we actually called scan on */
    uint32_t vectors_scanned;     /* total vectors we computed distance for */
    uint8_t  repair_triggered;    /* 1 if we entered the repair path */
    uint8_t  early_exit_taken;    /* 1 if we early-exited before finishing probes */
    uint8_t  _pad[2];
} perf_sample_t;

void perf_record(const perf_sample_t *s);

void perf_report(void);

/* Force an immediate report (useful from /inst) */
void perf_force_report(void);

#endif

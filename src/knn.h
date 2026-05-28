#ifndef KNN_H
#define KNN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int rinha_load_index(const char *path);
void rinha_set_search_params(int nprobe, int full_nprobe, int candidates);
int rinha_search(const float q[16], float *fraud_score_out);

/* Optional detailed stats for the just-completed search (filled by rinha_search if non-NULL) */
typedef struct {
    uint32_t clusters_probed;
    uint32_t vectors_scanned;
    uint8_t  repair_triggered;
    uint8_t  early_exit_taken;
} knn_stats_t;

int rinha_search_with_stats(const float q[16], float *fraud_score_out, knn_stats_t *stats_out);

void rinha_get_inst(uint64_t out[7]);
void rinha_reset_inst(void);

#ifdef __cplusplus
}
#endif

#endif

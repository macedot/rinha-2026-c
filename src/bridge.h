#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int rinha_load_index(const char *path);
void rinha_set_search_params(int nprobe, int full_nprobe, int candidates);
int rinha_search(const float q[14]);
void rinha_get_inst(uint64_t out[7]);
void rinha_reset_inst(void);

#ifdef __cplusplus
}
#endif

#endif

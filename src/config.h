#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *index_path;
    int ivf_nprobe;
    int ivf_full_nprobe;
    int candidates;
    char *uds_path;
    uint32_t uds_mode;
    int unlink_uds;
} config_t;

config_t config_load(void);
void config_free(config_t *cfg);

#endif

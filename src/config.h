#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *index_path;
    int ivf_nprobe;
    int ivf_full_nprobe;
    int candidates;
    int use_tcp;
    uint16_t port;
    char *host;
    char *uds_path;
    uint32_t uds_mode;
    int unlink_uds;
    int tcp_nodelay;
    int reuse_port;
} config_t;

config_t config_load(void);
void config_free(config_t *cfg);

#endif

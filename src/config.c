#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *env_str(const char *name, const char *def) {
    const char *v = getenv(name);
    if (v) return strdup(v);
    return strdup(def);
}

static int env_int(const char *name, int def, int minv, int maxv) {
    const char *v = getenv(name);
    if (!v) return def;
    int x = atoi(v);
    if (x < minv) return minv;
    if (x > maxv) return maxv;
    return x;
}

static int env_bool(const char *name, int def) {
    return env_int(name, def, 0, 1);
}

config_t config_load(void) {
    config_t cfg;
    cfg.index_path = env_str("INDEX_PATH", "resources/index.bin");
    cfg.ivf_nprobe = env_int("IVF_NPROBE", 8, 1, 64);
    cfg.ivf_full_nprobe = env_int("IVF_FULL_NPROBE", 24, 1, 64);
    cfg.candidates = env_int("CANDIDATES", 0, 0, 2000000);
    cfg.uds_path = env_str("UDS_PATH", getenv("SOCKET_PATH") ? getenv("SOCKET_PATH") : "/tmp/rinha.sock");
    cfg.uds_mode = (uint32_t)env_int("UDS_MODE", 666, 0, 777);
    cfg.unlink_uds = env_bool("UNLINK_UDS", 1);

    return cfg;
}

void config_free(config_t *cfg) {
    free(cfg->index_path);
    free(cfg->uds_path);
}

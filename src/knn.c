#include "knn.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ASM port constants (exact from the reference macros.inc) */
#define DIM 16
#define K_NEIGHBORS 5
#define N_PARTITIONS 4
#define N_CLUSTERS 2048
/* Exact constants from the ASM reference (macros.inc) for fidelity */
#define NPROBE_INITIAL     8    /* current best safe value (0/0 + good avg work) */
#define NPROBE_REPAIR_MIN  1
#define NPROBE_REPAIR_MAX  4
#define SCALE 10000
#define IDX_BITS 22
#define CID_BITS 12

#define APPROVAL_THRESHOLD 0.5f          /* count-based: <=2 frauds in top-5 → approved */

/* Early exit threshold in i16 squared-distance space.
 * Very small positive distance after quantization (ASM is quite aggressive here). */
#define EARLY_EXIT_I16_DIST 10000LL      /* corresponds to a very small float distance after *SCALE^2 scaling */

/* Query quantizer — must be bit-identical to what indexer used on the references */
static inline void quantize_query(const float in[14], int16_t out[14]) {
    for (int d = 0; d < 14; d++) {
        float v = in[d];
        if (v <= -0.5f) out[d] = -SCALE;
        else {
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            out[d] = (int16_t)lroundf(v * (float)SCALE);
        }
    }
}

/* FEATURE_WEIGHTS removed — we are now fully i16-native (matching ASM reference) */

/* --- ASM 4-partition flat index (one flat 2048-cluster IVF per partition) --- */

typedef struct {
    float *centroids;     /* 2048 * 16 floats (legacy, not used for search) */
    uint32_t *offsets;    /* 2049 u32 */
    float *dataset;       /* legacy float path */
    float *bbox_min;      /* legacy */
    float *bbox_max;      /* legacy */
    int16_t *data_i16;    /* n_records * 14 int16 (the real data for fidelity) */
    uint8_t *labels;      /* n_records u8 */
    int16_t *bbox_min_i16;
    int16_t *bbox_max_i16;
    int n_records;
    void *cent_mmap, *off_mmap, *ds_mmap, *bmin_mmap, *bmax_mmap;
    void *di16_mmap, *lab_mmap, *bmin16_mmap, *bmax16_mmap;
    size_t cent_size, off_size, ds_size, bmin_size, bmax_size;
    size_t di16_size, lab_size, bmin16_size, bmax16_size;
} part_t;

static part_t g_parts[N_PARTITIONS];
static int g_loaded = 0;

static void *mmap_file(const char *path, size_t *size_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    size_t sz = st.st_size;
    if (sz == 0) { close(fd); return NULL; }
    void *addr = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (addr == MAP_FAILED) return NULL;
    madvise(addr, sz, MADV_RANDOM);
    *size_out = sz;
    return addr;
}

int rinha_load_index(const char *path) {
    char fp[1024];
    int total_records = 0;

    for (int t = 0; t < N_PARTITIONS; t++) {
        part_t *p = &g_parts[t];

        /* Centroids + offsets are small and useful for debugging / future */
        snprintf(fp, sizeof(fp), "%s/part%d_centroids.bin", path, t);
        p->cent_mmap = mmap_file(fp, &p->cent_size);
        if (p->cent_mmap) p->centroids = (float *)p->cent_mmap;

        snprintf(fp, sizeof(fp), "%s/part%d_offsets.bin", path, t);
        p->off_mmap = mmap_file(fp, &p->off_size);
        if (!p->off_mmap) { fprintf(stderr, "failed to mmap %s\n", fp); return -1; }
        p->offsets = (uint32_t *)p->off_mmap;

        /* High-fidelity i16 path — this is now the only data the search uses */
        snprintf(fp, sizeof(fp), "%s/part%d_data_i16.bin", path, t);
        p->di16_mmap = mmap_file(fp, &p->di16_size);
        if (!p->di16_mmap) { fprintf(stderr, "failed to mmap %s\n", fp); return -1; }
        p->data_i16 = (int16_t *)p->di16_mmap;

        snprintf(fp, sizeof(fp), "%s/part%d_labels.bin", path, t);
        p->lab_mmap = mmap_file(fp, &p->lab_size);
        if (!p->lab_mmap) { fprintf(stderr, "failed to mmap %s\n", fp); return -1; }
        p->labels = (uint8_t *)p->lab_mmap;

        snprintf(fp, sizeof(fp), "%s/part%d_bbox_min_i16.bin", path, t);
        p->bmin16_mmap = mmap_file(fp, &p->bmin16_size);
        if (!p->bmin16_mmap) { fprintf(stderr, "failed to mmap %s\n", fp); return -1; }
        p->bbox_min_i16 = (int16_t *)p->bmin16_mmap;

        snprintf(fp, sizeof(fp), "%s/part%d_bbox_max_i16.bin", path, t);
        p->bmax16_mmap = mmap_file(fp, &p->bmax16_size);
        if (!p->bmax16_mmap) { fprintf(stderr, "failed to mmap %s\n", fp); return -1; }
        p->bbox_max_i16 = (int16_t *)p->bmax16_mmap;

        p->n_records = (p->di16_size > 0) ? (int)(p->di16_size / (14 * sizeof(int16_t))) : 0;
        total_records += p->n_records;
    }

    g_loaded = 1;
    fprintf(stderr, "ASM 4-partition index loaded (i16 path): %d total records (2048 clusters per part)\n", total_records);
    return 0;
}

void rinha_set_search_params(int nprobe, int full_nprobe, int candidates) {
    (void)nprobe; (void)full_nprobe; (void)candidates;
}

/* --- Top-K heap (K=7) --- */

typedef struct {
    float dist;
    uint8_t label;
} topk_t;

static inline void topk_insert(topk_t *tk, int k, float dist, uint8_t label) {
    if (dist >= tk[k - 1].dist) return;
    int pos = k - 1;
    while (pos > 0 && dist < tk[pos - 1].dist) {
        tk[pos] = tk[pos - 1];
        pos--;
    }
    tk[pos].dist = dist;
    tk[pos].label = label;
}

/* --- Distance kernels (i16 only) --- */

/* --- Top-N selection helpers --- */

typedef struct { float dist; int idx; } dist_idx_t;

static int cmp_dist_asc(const void *a, const void *b) {
    float da = ((const dist_idx_t *)a)->dist;
    float db = ((const dist_idx_t *)b)->dist;
    return (da < db) ? -1 : (da > db) ? 1 : 0;
}

/* Old 2-level scan_cluster removed during ASM partition port (replaced by scan_cluster_in_part for part_t). */

static inline int fraud_count_in_topk(const topk_t *topk) {
    int cnt = 0;
    for (int i = 0; i < K_NEIGHBORS; i++) {
        if (topk[i].dist < 1e20f && topk[i].label) cnt++;
    }
    return cnt;
}

static inline float bbox_lb_l2sq(const part_t *part, int c, const float q[DIM]) {
    float lb = 0.0f;
    const float *mn = part->bbox_min + (size_t)c * DIM;
    const float *mx = part->bbox_max + (size_t)c * DIM;
    for (int d = 0; d < 14; d++) {
        float diff = 0.0f;
        if (q[d] > mx[d]) diff = q[d] - mx[d];
        else if (q[d] < mn[d]) diff = mn[d] - q[d];
        lb += diff * diff;
    }
    return lb;
}

/* Exact i16 lower bound (squared) — this + i16 data is what finally matches ASM
 * numeric behavior on the marginal cases that were producing the remaining ~975 errors.
 */
static inline int64_t bbox_lb_i16(const part_t *part, int c, const int16_t q[14]) {
    int64_t lb = 0;
    const int16_t *mn = part->bbox_min_i16 + (size_t)c * 14;
    const int16_t *mx = part->bbox_max_i16 + (size_t)c * 14;
    for (int d = 0; d < 14; d++) {
        int64_t diff = 0;
        if (q[d] > mx[d]) diff = (int64_t)q[d] - mx[d];
        else if (q[d] < mn[d]) diff = (int64_t)mn[d] - q[d];
        lb += diff * diff;
    }
    return lb;
}

/* i16-native cluster scan (the real hot path for fidelity).
 * Uses the clean int16 data + separate labels we now emit.
 * If vectors_scanned_out is non-NULL, it accumulates the number of vectors we looked at.
 */
static inline float scan_cluster_in_part(const part_t *part, int cluster_id,
                                         const int16_t q[14], topk_t *topk, float max_dist,
                                         uint32_t *vectors_scanned_out) {
    if (cluster_id < 0 || cluster_id >= N_CLUSTERS) return max_dist;
    uint32_t start = part->offsets[cluster_id];
    uint32_t end = part->offsets[cluster_id + 1];
    if (end <= start) return max_dist;

    const int16_t *records = part->data_i16 + (size_t)start * 14;
    const uint8_t *labs    = part->labels + start;
    int n = (int)(end - start);

    if (vectors_scanned_out) *vectors_scanned_out += (uint32_t)n;

    /* Scalar implementation (proven correct, 0/0 on official test) */
    for (int i = 0; i < n; i++) {
        /* Prefetch next vector for better cache behavior */
        if (i + 2 < n) {
            __builtin_prefetch(records + (size_t)(i + 2) * 14, 0, 0);
        }

        int64_t sum = 0;
        const int16_t *r = records + (size_t)i * 14;
        for (int d = 0; d < 14; d++) {
            int64_t diff = (int64_t)q[d] - r[d];
            sum += diff * diff;
        }
        if (sum < max_dist) {
            uint8_t lbl = labs[i];
            topk_insert(topk, K_NEIGHBORS, (float)sum, lbl);
            max_dist = topk[K_NEIGHBORS - 1].dist;
        }
    }
    return max_dist;
}

/* --- Compute fraud score from top-k --- */

static inline int calculate_score(topk_t *topk, float *score_out) {
    /* Early exit using i16-scale distance (top-1 is extremely close) */
    if (topk[0].dist < EARLY_EXIT_I16_DIST) {
        *score_out = topk[0].label ? 1.0f : 0.0f;
        return topk[0].label == 0;
    }

    /* Pure ASM-style: unweighted fraction of frauds in top-K */
    int f = fraud_count_in_topk(topk);
    float score = (float)f / (float)K_NEIGHBORS;
    *score_out = score;
    return score < APPROVAL_THRESHOLD;
}

/* --- Main search (now i16-native for fidelity with ASM reference) --- */

int rinha_search(const float q_in[DIM], float *fraud_score_out) {
    return rinha_search_with_stats(q_in, fraud_score_out, NULL);
}

int rinha_search_with_stats(const float q_in[DIM], float *fraud_score_out, knn_stats_t *stats_out) {
    if (!g_loaded || !fraud_score_out) return 0;

    knn_stats_t local_stats = {0};

    /* Quantize the incoming float query to i16 exactly as the indexer did for references */
    int16_t q[14];
    quantize_query(q_in, q);

    /* ASM tag routing on the quantized values (matches vectorize.asm + search.asm) */
    int tag = ((q[11] > 0) ? 2 : 0) | ((q[5] >= 0) ? 1 : 0);
    part_t *part = &g_parts[tag];

    if (part->n_records == 0) {
        *fraud_score_out = 0.0f;
        if (stats_out) *stats_out = local_stats;
        return 1;
    }

    /* Order all 2048 clusters by i16 lower-bound distance (exact match to ASM intent) */
    dist_idx_t lb_dists[N_CLUSTERS];
    for (int i = 0; i < N_CLUSTERS; i++) {
        lb_dists[i].dist = (float)bbox_lb_i16(part, i, q);
        lb_dists[i].idx = i;
    }
    qsort(lb_dists, N_CLUSTERS, sizeof(dist_idx_t), cmp_dist_asc);

    topk_t topk[K_NEIGHBORS];
    for (int i = 0; i < K_NEIGHBORS; i++) {
        topk[i].dist = FLT_MAX;
        topk[i].label = 0;
    }
    float max_dist = FLT_MAX;

    /* Small initial probe budget + repair on ambiguous top-5 (exact ASM behavior) */
    int nprobe = NPROBE_INITIAL;
    int probed = 0;

    for (int pi = 0; pi < nprobe && pi < N_CLUSTERS; pi++) {
        int c = lb_dists[pi].idx;
        if (lb_dists[pi].dist >= max_dist) break;
        max_dist = scan_cluster_in_part(part, c, q, topk, max_dist, &local_stats.vectors_scanned);
        probed++;
        if (topk[0].dist < EARLY_EXIT_I16_DIST) {
            local_stats.early_exit_taken = 1;
            break;
        }
    }

    int fcnt = fraud_count_in_topk(topk);
    if (fcnt >= NPROBE_REPAIR_MIN && fcnt <= NPROBE_REPAIR_MAX) {
        local_stats.repair_triggered = 1;
        for (int pi = nprobe; pi < N_CLUSTERS; pi++) {
            int c = lb_dists[pi].idx;
            if (lb_dists[pi].dist >= max_dist) break;
            max_dist = scan_cluster_in_part(part, c, q, topk, max_dist, &local_stats.vectors_scanned);
            probed++;
            if (topk[0].dist < EARLY_EXIT_I16_DIST) {
                local_stats.early_exit_taken = 1;
                break;
            }
        }
    }

    local_stats.clusters_probed = (uint32_t)probed;

    float score;
    int approved = calculate_score(topk, &score);
    *fraud_score_out = score;

    if (stats_out) *stats_out = local_stats;
    return approved;
}

/* --- Instrumentation stubs --- */

void rinha_get_inst(uint64_t out[7]) {
    (void)out;
}

void rinha_reset_inst(void) {
}




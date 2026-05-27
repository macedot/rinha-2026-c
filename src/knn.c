#include "knn.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ASM port constants (exact from the reference macros.inc) */
#define DIM 16
#define K_NEIGHBORS 5
#define N_PARTITIONS 4
#define N_CLUSTERS 2048
#define NPROBE_INITIAL 12
#define NPROBE_REPAIR_MIN 1
#define NPROBE_REPAIR_MAX 4
#define SCALE 10000
#define IDX_BITS 22
#define CID_BITS 12

#define APPROVAL_THRESHOLD 0.5f          /* count-based: <=2 frauds in top-5 → approved */
#define EARLY_EXIT_DIST 0.01f
#define MAX_DIST_TRIGGER 2.0f

/* All 1.0 — ASM linear normalized space needs no extra weighting */
static const float FEATURE_WEIGHTS[DIM] = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    0.0f, 0.0f
};

/* --- ASM 4-partition flat index (one flat 2048-cluster IVF per partition) --- */

typedef struct {
    float *centroids;   /* 2048 * 16 floats */
    uint32_t *offsets;  /* 2049 u32 */
    float *dataset;     /* n_part * 16 floats, label in [15] */
    float *bbox_min;    /* 2048 * 16 floats */
    float *bbox_max;    /* 2048 * 16 floats */
    int n_records;
    void *cent_mmap, *off_mmap, *ds_mmap, *bmin_mmap, *bmax_mmap;
    size_t cent_size, off_size, ds_size, bmin_size, bmax_size;
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

        snprintf(fp, sizeof(fp), "%s/part%d_centroids.bin", path, t);
        p->cent_mmap = mmap_file(fp, &p->cent_size);
        if (!p->cent_mmap) { fprintf(stderr, "failed to mmap %s\n", fp); return -1; }
        p->centroids = (float *)p->cent_mmap;

        snprintf(fp, sizeof(fp), "%s/part%d_offsets.bin", path, t);
        p->off_mmap = mmap_file(fp, &p->off_size);
        if (!p->off_mmap) { fprintf(stderr, "failed to mmap %s\n", fp); return -1; }
        p->offsets = (uint32_t *)p->off_mmap;

        snprintf(fp, sizeof(fp), "%s/part%d_dataset.bin", path, t);
        p->ds_mmap = mmap_file(fp, &p->ds_size);
        if (!p->ds_mmap) { fprintf(stderr, "failed to mmap %s\n", fp); return -1; }
        p->dataset = (float *)p->ds_mmap;
        p->n_records = (p->ds_size > 0) ? (int)(p->ds_size / (DIM * sizeof(float))) : 0;

        snprintf(fp, sizeof(fp), "%s/part%d_bbox_min.bin", path, t);
        p->bmin_mmap = mmap_file(fp, &p->bmin_size);
        if (!p->bmin_mmap) { fprintf(stderr, "failed to mmap %s\n", fp); return -1; }
        p->bbox_min = (float *)p->bmin_mmap;

        snprintf(fp, sizeof(fp), "%s/part%d_bbox_max.bin", path, t);
        p->bmax_mmap = mmap_file(fp, &p->bmax_size);
        if (!p->bmax_mmap) { fprintf(stderr, "failed to mmap %s\n", fp); return -1; }
        p->bbox_max = (float *)p->bmax_mmap;

        total_records += p->n_records;
    }

    g_loaded = 1;
    fprintf(stderr, "ASM 4-partition index loaded: %d total records (2048 clusters per part)\n", total_records);
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

static inline uint8_t unpack_label(float packed) {
    uint32_t bits;
    memcpy(&bits, &packed, sizeof(uint32_t));
    return (uint8_t)(bits & 1u);
}

/* --- Distance kernels --- */

static inline float manhattan_scalar(const float a[DIM], const float b[DIM]) {
    float sum = 0.0f;
    for (int i = 0; i < 14; i++) {
        sum += fabsf(a[i] - b[i]);
    }
    return sum;
}

#ifdef __AVX2__
#include <immintrin.h>

static inline float hsum_ps(__m128 v) {
    v = _mm_hadd_ps(v, v);
    v = _mm_hadd_ps(v, v);
    return _mm_cvtss_f32(v);
}

static inline float manhattan_avx2(const float a[DIM], const float b[DIM]) {
    __m256 q0 = _mm256_loadu_ps(a);
    __m256 q1 = _mm256_loadu_ps(a + 8);
    __m256 c0 = _mm256_loadu_ps(b);
    __m256 c1 = _mm256_loadu_ps(b + 8);
    __m256 absm = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    __m256 d0 = _mm256_and_ps(_mm256_sub_ps(q0, c0), absm);
    __m256 d1 = _mm256_and_ps(_mm256_sub_ps(q1, c1), absm);
    __m256 s = _mm256_add_ps(d0, d1);
    __m128 lo = _mm256_castps256_ps128(s);
    __m128 hi = _mm256_extractf128_ps(s, 1);
    return hsum_ps(_mm_add_ps(lo, hi));
}

static inline void manhattan_avx2_x4(const float q[DIM],
                                     const float *r0, const float *r1,
                                     const float *r2, const float *r3,
                                     float out[4]) {
    __m256 q0 = _mm256_loadu_ps(q);
    __m256 q1 = _mm256_loadu_ps(q + 8);
    __m256 absm = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    const float *rr[4] = {r0, r1, r2, r3};
    for (int i = 0; i < 4; i++) {
        __m256 c0 = _mm256_loadu_ps(rr[i]);
        __m256 c1 = _mm256_loadu_ps(rr[i] + 8);
        __m256 d0 = _mm256_and_ps(_mm256_sub_ps(q0, c0), absm);
        __m256 d1 = _mm256_and_ps(_mm256_sub_ps(q1, c1), absm);
        __m256 s = _mm256_add_ps(d0, d1);
        __m128 lo = _mm256_castps256_ps128(s);
        __m128 hi = _mm256_extractf128_ps(s, 1);
        out[i] = hsum_ps(_mm_add_ps(lo, hi));
    }
}
#endif

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

static inline float scan_cluster_in_part(const part_t *part, int cluster_id,
                                         const float q[DIM], topk_t *topk, float max_dist) {
    if (cluster_id < 0 || cluster_id >= N_CLUSTERS) return max_dist;
    uint32_t start = part->offsets[cluster_id];
    uint32_t end = part->offsets[cluster_id + 1];
    if (end <= start) return max_dist;

    const float *records = part->dataset + (size_t)start * DIM;
    int n = (int)(end - start);

#ifdef __AVX2__
    int i = 0;
    while (i + 3 < n) {
        float dists[4];
        manhattan_avx2_x4(q,
            records + (size_t)i * DIM,
            records + (size_t)(i + 1) * DIM,
            records + (size_t)(i + 2) * DIM,
            records + (size_t)(i + 3) * DIM,
            dists);
        for (int j = 0; j < 4; j++) {
            if (dists[j] < max_dist) {
                uint8_t lbl = unpack_label(records[(size_t)(i + j) * DIM + 15]);
                topk_insert(topk, K_NEIGHBORS, dists[j], lbl);
                max_dist = topk[K_NEIGHBORS - 1].dist;
            }
        }
        i += 4;
    }
    while (i < n) {
        float d = manhattan_avx2(q, records + (size_t)i * DIM);
        if (d < max_dist) {
            uint8_t lbl = unpack_label(records[(size_t)i * DIM + 15]);
            topk_insert(topk, K_NEIGHBORS, d, lbl);
            max_dist = topk[K_NEIGHBORS - 1].dist;
        }
        i++;
    }
#else
    for (int i = 0; i < n; i++) {
        float d = manhattan_scalar(q, records + (size_t)i * DIM);
        if (d < max_dist) {
            uint8_t lbl = unpack_label(records[(size_t)i * DIM + 15]);
            topk_insert(topk, K_NEIGHBORS, d, lbl);
            max_dist = topk[K_NEIGHBORS - 1].dist;
        }
    }
#endif
    return max_dist;
}

/* --- Compute fraud score from top-k --- */

static inline int calculate_score(topk_t *topk, float *score_out) {
    if (topk[0].dist < EARLY_EXIT_DIST) {
        *score_out = topk[0].label ? 1.0f : 0.0f;
        return topk[0].label == 0;
    }

    float fraud_weight = 0.0f;
    float total_weight = 0.0f;
    for (int i = 0; i < K_NEIGHBORS; i++) {
        if (topk[i].dist >= 1e20f) continue;
        float w = expf(-topk[i].dist * 0.5f);
        total_weight += w;
        fraud_weight += w * topk[i].label;
    }
    float score = (total_weight > 0.0f) ? (fraud_weight / total_weight) : 0.0f;
    *score_out = score;
    return score < APPROVAL_THRESHOLD;
}

/* --- Main search --- */

int rinha_search(const float q_in[DIM], float *fraud_score_out) {
    if (!g_loaded || !fraud_score_out) return 0;

    float q[DIM];
    for (int i = 0; i < 14; i++) q[i] = q_in[i] * FEATURE_WEIGHTS[i];
    q[14] = 0.0f; q[15] = 0.0f;

    /* ASM tag routing */
    int tag = ((q[11] > 0.5f) ? 2 : 0) | ((q[5] >= 0.0f) ? 1 : 0);
    part_t *part = &g_parts[tag];

    if (part->n_records == 0) {
        *fraud_score_out = 0.0f;
        return 1;
    }

    /* Simple full centroid scan for this partition (2048) — bbox pruning comes next */
    dist_idx_t c_dists[N_CLUSTERS];
    for (int i = 0; i < N_CLUSTERS; i++) {
        c_dists[i].dist = manhattan_scalar(q, part->centroids + (size_t)i * DIM);
        c_dists[i].idx = i;
    }
    qsort(c_dists, N_CLUSTERS, sizeof(dist_idx_t), cmp_dist_asc);

    topk_t topk[K_NEIGHBORS];
    for (int i = 0; i < K_NEIGHBORS; i++) {
        topk[i].dist = FLT_MAX;
        topk[i].label = 0;
    }
    float max_dist = FLT_MAX;

    int nprobe = NPROBE_INITIAL;
    for (int pi = 0; pi < nprobe && pi < N_CLUSTERS; pi++) {
        max_dist = scan_cluster_in_part(part, c_dists[pi].idx, q, topk, max_dist);
        if (topk[0].dist < EARLY_EXIT_DIST) break;
    }

    int fcnt = fraud_count_in_topk(topk);
    if (fcnt >= NPROBE_REPAIR_MIN && fcnt <= NPROBE_REPAIR_MAX) {
        /* repair: extend significantly */
        for (int pi = nprobe; pi < N_CLUSTERS; pi++) {
            max_dist = scan_cluster_in_part(part, c_dists[pi].idx, q, topk, max_dist);
            if (topk[0].dist < EARLY_EXIT_DIST) break;
        }
    }

    float score;
    int approved = calculate_score(topk, &score);
    *fraud_score_out = score;
    return approved;
}

/* --- Instrumentation stubs --- */

void rinha_get_inst(uint64_t out[7]) {
    (void)out;
}

void rinha_reset_inst(void) {
}

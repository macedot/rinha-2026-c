#include "bridge.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <time.h>

static inline uint64_t get_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t g_time_quant = 0;
static uint64_t g_time_centroid = 0;
static uint64_t g_time_topn = 0;
static uint64_t g_time_reorder = 0;
static uint64_t g_time_fast = 0;
static uint64_t g_time_full = 0;
static uint64_t g_inst_count = 0;

void rinha_get_inst(uint64_t out[7]) {
    out[0] = g_time_quant;
    out[1] = g_time_centroid;
    out[2] = g_time_topn;
    out[3] = g_time_reorder;
    out[4] = g_time_fast;
    out[5] = g_time_full;
    out[6] = g_inst_count;
}

void rinha_reset_inst(void) {
    g_time_quant = 0;
    g_time_centroid = 0;
    g_time_topn = 0;
    g_time_reorder = 0;
    g_time_fast = 0;
    g_time_full = 0;
    g_inst_count = 0;
}

#define DIM 14
#define K_NEIGHBORS 5
#define FIX_SCALE 10000.0f
#define VECTOR_SCALE (1.0f / FIX_SCALE)
#define IVF_CLUSTERS 4096
#define IVF_MAX_NPROBE 64
#define CACHELINE 64
#define BLOCK_STRIDE 112  /* 8 slots * 14 dims */

#if defined(__GNUC__) || defined(__clang__)
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#endif

static inline void *xaligned_alloc(size_t alignment, size_t size) {
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) return NULL;
    memset(ptr, 0, size);
    return ptr;
}

static inline int16_t quantize_fixed(float x) {
    if (x < -1.0f) x = -1.0f;
    if (x >  1.0f) x =  1.0f;
    float scaled = x * FIX_SCALE;
    scaled += scaled >= 0.0f ? 0.5f : -0.5f;
    if (scaled < -10000.0f) scaled = -10000.0f;
    if (scaled >  10000.0f) scaled =  10000.0f;
    return (int16_t)scaled;
}

/* Dataset: IVF1 AoSoA layout
 *
 * centroids_t[dim][cluster]: float, transposed column-major (dim-major)
 *   - centroid distance reads dim0 of all clusters contiguously, then dim1, etc.
 * block_offsets[K+1]: cumulative block counts per cluster
 * labels[total_blocks * 8]: padded to block boundaries
 * blocks[total_blocks * BLOCK_STRIDE]: AoSoA
 *   - each block: dim0[0..7], dim1[0..7], ..., dim13[0..7] = 112 i16
 */
typedef struct {
    int n;
    int total_blocks;
    float *centroids_t;       /* [DIM * IVF_CLUSTERS], transposed, align(32) */
    uint32_t block_offsets[IVF_CLUSTERS + 1];
    uint8_t *labels;          /* [total_blocks * 8], padded */
    int16_t *blocks;          /* [total_blocks * BLOCK_STRIDE], AoSoA, align(32) */
} dataset_t;

static dataset_t g_dataset;
static int g_nprobe = 8;
static int g_full_nprobe = 24;
static int g_candidates = 0;

static int read_exact(FILE *f, void *ptr, size_t len) {
    return fread(ptr, 1, len, f) == len ? 0 : -1;
}

int rinha_load_index(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }

    char magic[4];
    uint32_t n = 0, k = 0, d = 0;
    if (read_exact(f, magic, 4) != 0 || memcmp(magic, "IVF1", 4) != 0 ||
        read_exact(f, &n, sizeof(n)) != 0 ||
        read_exact(f, &k, sizeof(k)) != 0 ||
        read_exact(f, &d, sizeof(d)) != 0) {
        fprintf(stderr, "index invalido ou magic != IVF1: %s\n", path);
        fclose(f); return -1;
    }

    if (k != IVF_CLUSTERS || d != DIM) {
        fprintf(stderr, "index incompativel: K=%u D=%u\n", k, d);
        fclose(f); return -1;
    }

    memset(&g_dataset, 0, sizeof(g_dataset));
    g_dataset.n = (int)n;

    /* Transposed centroids: DIM * IVF_CLUSTERS floats, align(32) */
    g_dataset.centroids_t = (float *)xaligned_alloc(32,
        (size_t)DIM * IVF_CLUSTERS * sizeof(float));
    if (!g_dataset.centroids_t) { fclose(f); return -1; }
    if (read_exact(f, g_dataset.centroids_t,
        (size_t)DIM * IVF_CLUSTERS * sizeof(float)) != 0) {
        fclose(f); return -1;
    }

    /* Block offsets: K+1 uint32 */
    if (read_exact(f, g_dataset.block_offsets,
        (size_t)(IVF_CLUSTERS + 1) * sizeof(uint32_t)) != 0) {
        fclose(f); return -1;
    }
    g_dataset.total_blocks = (int)g_dataset.block_offsets[IVF_CLUSTERS];
    int padded_n = g_dataset.total_blocks * 8;

    /* Labels: padded_n uint8 */
    g_dataset.labels = (uint8_t *)xaligned_alloc(CACHELINE,
        (size_t)padded_n * sizeof(uint8_t));
    if (!g_dataset.labels) { fclose(f); return -1; }
    if (read_exact(f, g_dataset.labels, (size_t)padded_n) != 0) {
        fclose(f); return -1;
    }

    /* Blocks: total_blocks * BLOCK_STRIDE int16, align(32) */
    g_dataset.blocks = (int16_t *)xaligned_alloc(32,
        (size_t)g_dataset.total_blocks * BLOCK_STRIDE * sizeof(int16_t));
    if (!g_dataset.blocks) { fclose(f); return -1; }
    if (read_exact(f, g_dataset.blocks,
        (size_t)g_dataset.total_blocks * BLOCK_STRIDE * sizeof(int16_t)) != 0) {
        fclose(f); return -1;
    }

    fclose(f);

    double mb = ((double)g_dataset.total_blocks * BLOCK_STRIDE * sizeof(int16_t) +
                 (double)padded_n +
                 (double)DIM * IVF_CLUSTERS * sizeof(float) +
                 (double)(IVF_CLUSTERS + 1) * sizeof(uint32_t)) / (1024.0 * 1024.0);
    fprintf(stderr, "index IVF1 carregado (C bridge): N=%u K=%u blocks=%d memoria=%.2f MB\n",
        n, k, g_dataset.total_blocks, mb);
#ifdef __AVX2__
    fprintf(stderr, "C bridge: AVX2 enabled (AoSoA blocks + centroid dist)\n");
#else
    fprintf(stderr, "C bridge: scalar fallback\n");
#endif
    return 0;
}

void rinha_set_search_params(int nprobe, int full_nprobe, int candidates) {
    g_nprobe = nprobe;
    g_full_nprobe = full_nprobe;
    g_candidates = candidates;
}

/* --- Top-5 maintenance --- */

static inline int worst_index5_f(const float d[5]) {
    int w = 0;
    if (d[1] > d[w]) w = 1;
    if (d[2] > d[w]) w = 2;
    if (d[3] > d[w]) w = 3;
    if (d[4] > d[w]) w = 4;
    return w;
}

static inline void try_insert_top5_f(float d, uint8_t label,
                                     float best_d[5], uint8_t best_l[5],
                                     int *worst, float *worst_d) {
    if (d < *worst_d) {
        int pos = 4;
        while (pos > 0 && d < best_d[pos - 1]) pos--;
        for (int i = 4; i > pos; i--) {
            best_d[i] = best_d[i - 1];
            best_l[i] = best_l[i - 1];
        }
        best_d[pos] = d;
        best_l[pos] = label;
        *worst_d = best_d[4];
    }
}

/* --- Centroid distance: scalar fallback --- */

static inline float centroid_sqdist_scalar(const float q[DIM], int c) {
    float s = 0.0f;
    for (int j = 0; j < DIM; j++) {
        float d = q[j] - g_dataset.centroids_t[(size_t)j * IVF_CLUSTERS + c];
        s += d * d;
    }
    return s;
}

static inline void insert_probe_cluster(int cluster, float penalty,
                                        int *best_c, float *best_p, int nprobe) {
    if (penalty >= best_p[nprobe - 1]) return;
    int pos = nprobe - 1;
    while (pos > 0 && penalty < best_p[pos - 1]) pos--;
    for (int i = nprobe - 1; i > pos; i--) {
        best_p[i] = best_p[i - 1];
        best_c[i] = best_c[i - 1];
    }
    best_p[pos] = penalty;
    best_c[pos] = cluster;
}

/* --- AVX2 centroid distance (process 16 centroids per iter) --- */

#ifdef __AVX2__
#include <immintrin.h>

static void compute_centroid_dists_avx2(const float q[DIM], float dists[IVF_CLUSTERS]) {
    const float *cp = g_dataset.centroids_t;
    float *dp = dists;
    const int k = IVF_CLUSTERS;

    /* Dim 0: initialize squared differences */
    {
        __m256 qd = _mm256_set1_ps(q[0]);
        int ci = 0;
        while (ci + 16 <= k) {
            __m256 c0 = _mm256_loadu_ps(cp + ci);
            __m256 c1 = _mm256_loadu_ps(cp + ci + 8);
            __m256 d0 = _mm256_sub_ps(c0, qd);
            __m256 d1 = _mm256_sub_ps(c1, qd);
            _mm256_storeu_ps(dp + ci, _mm256_mul_ps(d0, d0));
            _mm256_storeu_ps(dp + ci + 8, _mm256_mul_ps(d1, d1));
            ci += 16;
        }
        while (ci + 8 <= k) {
            __m256 c0 = _mm256_loadu_ps(cp + ci);
            __m256 d0 = _mm256_sub_ps(c0, qd);
            _mm256_storeu_ps(dp + ci, _mm256_mul_ps(d0, d0));
            ci += 8;
        }
        while (ci < k) {
            float diff = cp[ci] - q[0];
            dp[ci] = diff * diff;
            ci++;
        }
    }

    /* Dims 1..13: accumulate with FMA */
    for (int d = 1; d < DIM; d++) {
        size_t base = (size_t)d * k;
        __m256 qd = _mm256_set1_ps(q[d]);
        int ci = 0;
        while (ci + 16 <= k) {
            __m256 cv0 = _mm256_loadu_ps(cp + base + ci);
            __m256 cv1 = _mm256_loadu_ps(cp + base + ci + 8);
            __m256 d0 = _mm256_sub_ps(cv0, qd);
            __m256 d1 = _mm256_sub_ps(cv1, qd);
            __m256 a0 = _mm256_loadu_ps(dp + ci);
            __m256 a1 = _mm256_loadu_ps(dp + ci + 8);
            _mm256_storeu_ps(dp + ci, _mm256_fmadd_ps(d0, d0, a0));
            _mm256_storeu_ps(dp + ci + 8, _mm256_fmadd_ps(d1, d1, a1));
            ci += 16;
        }
        while (ci + 8 <= k) {
            __m256 cv = _mm256_loadu_ps(cp + base + ci);
            __m256 d0 = _mm256_sub_ps(cv, qd);
            __m256 a0 = _mm256_loadu_ps(dp + ci);
            _mm256_storeu_ps(dp + ci, _mm256_fmadd_ps(d0, d0, a0));
            ci += 8;
        }
        while (ci < k) {
            float diff = cp[base + ci] - q[d];
            dp[ci] += diff * diff;
            ci++;
        }
    }
}

static void top_n_from_dists_avx2(int nprobe, const float dists[IVF_CLUSTERS],
                                  int best_c[IVF_MAX_NPROBE], float best_p[IVF_MAX_NPROBE]) {
    for (int i = 0; i < nprobe; i++) {
        best_c[i] = -1;
        best_p[i] = FLT_MAX;
    }

    int ci = 0;
    while (ci + 8 <= IVF_CLUSTERS) {
        __m256 d8 = _mm256_loadu_ps(dists + ci);
        __m256 threshold = _mm256_set1_ps(best_p[nprobe - 1]);
        int mask = _mm256_movemask_ps(_mm256_cmp_ps(d8, threshold, _CMP_LT_OQ));
        if (mask != 0) {
            float buf[8];
            _mm256_storeu_ps(buf, d8);
            int m = mask;
            while (m != 0) {
                int s = __builtin_ctz(m);
                m &= m - 1;
                float di = buf[s];
                if (di < best_p[nprobe - 1]) {
                    int pos = nprobe - 1;
                    while (pos > 0 && di < best_p[pos - 1]) pos--;
                    for (int i = nprobe - 1; i > pos; i--) {
                        best_p[i] = best_p[i - 1];
                        best_c[i] = best_c[i - 1];
                    }
                    best_p[pos] = di;
                    best_c[pos] = ci + s;
                }
            }
        }
        ci += 8;
    }

    while (ci < IVF_CLUSTERS) {
        float di = dists[ci];
        if (di < best_p[nprobe - 1]) {
            int pos = nprobe - 1;
            while (pos > 0 && di < best_p[pos - 1]) pos--;
            for (int i = nprobe - 1; i > pos; i--) {
                best_p[i] = best_p[i - 1];
                best_c[i] = best_c[i - 1];
            }
            best_p[pos] = di;
            best_c[pos] = ci;
        }
        ci++;
    }
}
#endif

static void compute_centroid_dists_scalar(const float q[DIM], float dists[IVF_CLUSTERS]) {
    for (int c = 0; c < IVF_CLUSTERS; c++) {
        dists[c] = centroid_sqdist_scalar(q, c);
    }
}

static void top_n_from_dists_scalar(int nprobe, const float dists[IVF_CLUSTERS],
                                    int best_c[IVF_MAX_NPROBE], float best_p[IVF_MAX_NPROBE]) {
    for (int i = 0; i < nprobe; i++) {
        best_c[i] = -1;
        best_p[i] = FLT_MAX;
    }
    for (int c = 0; c < IVF_CLUSTERS; c++) {
        insert_probe_cluster(c, dists[c], best_c, best_p, nprobe);
    }
}

/* --- AoSoA block-based scan kernels --- */

static inline void scan_blocks_scalar(int start_block, int end_block, const int16_t q[DIM],
                                      float best_d[5], uint8_t best_l[5],
                                      int *worst, float *worst_d) {
    const int16_t *blocks = g_dataset.blocks;
    const uint8_t *labels = g_dataset.labels;
    const float vscale = VECTOR_SCALE;

    for (int bi = start_block; bi < end_block; bi++) {
        const int16_t *b = blocks + (size_t)bi * BLOCK_STRIDE;
        const uint8_t *lb = labels + (size_t)bi * 8;

        for (int slot = 0; slot < 8; slot++) {
            if (b[slot] == INT16_MAX) continue;

            float dist = 0.0f;
            for (int j = 0; j < DIM; j++) {
                float dv = (float)b[(size_t)j * 8 + slot] * vscale;
                float diff = dv - (float)q[j] * vscale;
                dist += diff * diff;
            }
            if (dist < *worst_d) {
                try_insert_top5_f(dist, lb[slot], best_d, best_l, worst, worst_d);
            }
        }
    }
}

#ifdef __AVX2__

static void scan_blocks_avx2(int start_block, int end_block, const int16_t q[DIM],
                             float best_d[5], uint8_t best_l[5],
                             int *worst, float *worst_d) {
    const int16_t *blocks = g_dataset.blocks;
    const uint8_t *labels = g_dataset.labels;
    const __m256 scale = _mm256_set1_ps(VECTOR_SCALE);

    const __m256 qv0  = _mm256_set1_ps((float)q[0]  * VECTOR_SCALE);
    const __m256 qv1  = _mm256_set1_ps((float)q[1]  * VECTOR_SCALE);
    const __m256 qv2  = _mm256_set1_ps((float)q[2]  * VECTOR_SCALE);
    const __m256 qv3  = _mm256_set1_ps((float)q[3]  * VECTOR_SCALE);
    const __m256 qv4  = _mm256_set1_ps((float)q[4]  * VECTOR_SCALE);
    const __m256 qv5  = _mm256_set1_ps((float)q[5]  * VECTOR_SCALE);
    const __m256 qv6  = _mm256_set1_ps((float)q[6]  * VECTOR_SCALE);
    const __m256 qv7  = _mm256_set1_ps((float)q[7]  * VECTOR_SCALE);
    const __m256 qv8  = _mm256_set1_ps((float)q[8]  * VECTOR_SCALE);
    const __m256 qv9  = _mm256_set1_ps((float)q[9]  * VECTOR_SCALE);
    const __m256 qv10 = _mm256_set1_ps((float)q[10] * VECTOR_SCALE);
    const __m256 qv11 = _mm256_set1_ps((float)q[11] * VECTOR_SCALE);
    const __m256 qv12 = _mm256_set1_ps((float)q[12] * VECTOR_SCALE);
    const __m256 qv13 = _mm256_set1_ps((float)q[13] * VECTOR_SCALE);

    float dists_buf[8] __attribute__((aligned(32)));

    for (int bi = start_block; bi < end_block; bi++) {
        int pf_block = bi + 8;
        if (pf_block < end_block) {
            _mm_prefetch((const char *)(blocks + (size_t)pf_block * BLOCK_STRIDE),
                         _MM_HINT_T0);
            _mm_prefetch((const char *)(blocks + (size_t)pf_block * BLOCK_STRIDE + 56),
                         _MM_HINT_T0);
        }

        size_t bb = (size_t)bi * BLOCK_STRIDE;
        const uint8_t *lb = labels + (size_t)bi * 8;
        __m256 threshold = _mm256_set1_ps(*worst_d);

        __m256 acc0 = _mm256_setzero_ps();
        __m256 acc1 = _mm256_setzero_ps();

        /* Pair (0,1) */
        { __m128i r0 = _mm_loadu_si128((const __m128i *)(blocks + bb + 0));
          __m128i r1 = _mm_loadu_si128((const __m128i *)(blocks + bb + 8));
          __m256 v0 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r0)), scale);
          __m256 v1 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r1)), scale);
          acc0 = _mm256_fmadd_ps(_mm256_sub_ps(v0, qv0), _mm256_sub_ps(v0, qv0), acc0);
          acc1 = _mm256_fmadd_ps(_mm256_sub_ps(v1, qv1), _mm256_sub_ps(v1, qv1), acc1); }

        /* Pair (2,3) */
        { __m128i r0 = _mm_loadu_si128((const __m128i *)(blocks + bb + 16));
          __m128i r1 = _mm_loadu_si128((const __m128i *)(blocks + bb + 24));
          __m256 v0 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r0)), scale);
          __m256 v1 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r1)), scale);
          acc0 = _mm256_fmadd_ps(_mm256_sub_ps(v0, qv2), _mm256_sub_ps(v0, qv2), acc0);
          acc1 = _mm256_fmadd_ps(_mm256_sub_ps(v1, qv3), _mm256_sub_ps(v1, qv3), acc1); }

        /* Pair (4,5) */
        { __m128i r0 = _mm_loadu_si128((const __m128i *)(blocks + bb + 32));
          __m128i r1 = _mm_loadu_si128((const __m128i *)(blocks + bb + 40));
          __m256 v0 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r0)), scale);
          __m256 v1 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r1)), scale);
          acc0 = _mm256_fmadd_ps(_mm256_sub_ps(v0, qv4), _mm256_sub_ps(v0, qv4), acc0);
          acc1 = _mm256_fmadd_ps(_mm256_sub_ps(v1, qv5), _mm256_sub_ps(v1, qv5), acc1); }

        /* Pair (6,7) */
        { __m128i r0 = _mm_loadu_si128((const __m128i *)(blocks + bb + 48));
          __m128i r1 = _mm_loadu_si128((const __m128i *)(blocks + bb + 56));
          __m256 v0 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r0)), scale);
          __m256 v1 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r1)), scale);
          acc0 = _mm256_fmadd_ps(_mm256_sub_ps(v0, qv6), _mm256_sub_ps(v0, qv6), acc0);
          acc1 = _mm256_fmadd_ps(_mm256_sub_ps(v1, qv7), _mm256_sub_ps(v1, qv7), acc1); }

        /* Early termination */
        __m256 partial = _mm256_add_ps(acc0, acc1);
        if (_mm256_movemask_ps(_mm256_cmp_ps(partial, threshold, _CMP_LT_OQ)) == 0)
            continue;

        /* Pair (8,9) */
        { __m128i r0 = _mm_loadu_si128((const __m128i *)(blocks + bb + 64));
          __m128i r1 = _mm_loadu_si128((const __m128i *)(blocks + bb + 72));
          __m256 v0 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r0)), scale);
          __m256 v1 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r1)), scale);
          acc0 = _mm256_fmadd_ps(_mm256_sub_ps(v0, qv8), _mm256_sub_ps(v0, qv8), acc0);
          acc1 = _mm256_fmadd_ps(_mm256_sub_ps(v1, qv9), _mm256_sub_ps(v1, qv9), acc1); }

        /* Pair (10,11) */
        { __m128i r0 = _mm_loadu_si128((const __m128i *)(blocks + bb + 80));
          __m128i r1 = _mm_loadu_si128((const __m128i *)(blocks + bb + 88));
          __m256 v0 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r0)), scale);
          __m256 v1 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r1)), scale);
          acc0 = _mm256_fmadd_ps(_mm256_sub_ps(v0, qv10), _mm256_sub_ps(v0, qv10), acc0);
          acc1 = _mm256_fmadd_ps(_mm256_sub_ps(v1, qv11), _mm256_sub_ps(v1, qv11), acc1); }

        /* Pair (12,13) */
        { __m128i r0 = _mm_loadu_si128((const __m128i *)(blocks + bb + 96));
          __m128i r1 = _mm_loadu_si128((const __m128i *)(blocks + bb + 104));
          __m256 v0 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r0)), scale);
          __m256 v1 = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(r1)), scale);
          acc0 = _mm256_fmadd_ps(_mm256_sub_ps(v0, qv12), _mm256_sub_ps(v0, qv12), acc0);
          acc1 = _mm256_fmadd_ps(_mm256_sub_ps(v1, qv13), _mm256_sub_ps(v1, qv13), acc1); }

        __m256 acc = _mm256_add_ps(acc0, acc1);
        int mask = _mm256_movemask_ps(_mm256_cmp_ps(acc, threshold, _CMP_LT_OQ));
        if (mask == 0) continue;

        _mm256_store_ps(dists_buf, acc);
        int m = mask;
        while (m != 0) {
            int slot = __builtin_ctz(m);
            m &= m - 1;
            float di = dists_buf[slot];
            if (di < *worst_d) {
                try_insert_top5_f(di, lb[slot], best_d, best_l, worst, worst_d);
            }
        }
    }
}
#endif

static inline void scan_blocks(int start_block, int end_block, const int16_t q[DIM],
                               float best_d[5], uint8_t best_l[5],
                               int *worst, float *worst_d) {
#ifdef __AVX2__
    scan_blocks_avx2(start_block, end_block, q, best_d, best_l, worst, worst_d);
#else
    scan_blocks_scalar(start_block, end_block, q, best_d, best_l, worst, worst_d);
#endif
}

/* --- Search with nprobe (block-based) --- */

static inline int count_frauds5(const uint8_t best_l[5]) {
    return (best_l[0] == 1) + (best_l[1] == 1) + (best_l[2] == 1) +
           (best_l[3] == 1) + (best_l[4] == 1);
}

static int search_with_nprobe(int nprobe_to_use, const int best_c[],
                              const int16_t q[DIM]) {
    int nprobe = nprobe_to_use;
    if (nprobe > IVF_CLUSTERS) nprobe = IVF_CLUSTERS;
    if (nprobe < 1) nprobe = 1;

    float best_d[5] = { INFINITY, INFINITY, INFINITY, INFINITY, INFINITY };
    uint8_t best_l[5] = {0, 0, 0, 0, 0};
    int worst = 0;
    float worst_d = INFINITY;

    for (int pi = 0; pi < nprobe; pi++) {
        int c = best_c[pi];
        if (c < 0) continue;
        int start_block = (int)g_dataset.block_offsets[c];
        int end_block   = (int)g_dataset.block_offsets[c + 1];
        if (end_block <= start_block) continue;

        int nblocks = end_block - start_block;
        if (g_candidates > 0) {
            int max_blocks = (g_candidates + 7) / 8;
            if (nblocks > max_blocks) nblocks = max_blocks;
            end_block = start_block + nblocks;
        }
        scan_blocks(start_block, end_block, q, best_d, best_l, &worst, &worst_d);
    }

    return count_frauds5(best_l);
}

int rinha_search(const float q_float[DIM]) {
    uint64_t t0 = get_nanos();
    int16_t q[DIM];
    float q_grid[DIM];
    for (int j = 0; j < DIM; j++) {
        q[j] = quantize_fixed(q_float[j]);
        q_grid[j] = (float)q[j] / FIX_SCALE;
    }
    uint64_t t1 = get_nanos();

    int fast_nprobe = g_nprobe;
    if (fast_nprobe < 1) fast_nprobe = 1;
    if (fast_nprobe > IVF_MAX_NPROBE) fast_nprobe = IVF_MAX_NPROBE;
    if (fast_nprobe > IVF_CLUSTERS) fast_nprobe = IVF_CLUSTERS;

    int full_nprobe = g_full_nprobe;
    if (full_nprobe < fast_nprobe) full_nprobe = fast_nprobe;
    if (full_nprobe > IVF_MAX_NPROBE) full_nprobe = IVF_MAX_NPROBE;
    if (full_nprobe > IVF_CLUSTERS) full_nprobe = IVF_CLUSTERS;

    /* Compute ALL centroid distances — AVX2 vectorized */
    float dists[IVF_CLUSTERS];
#ifdef __AVX2__
    compute_centroid_dists_avx2(q_grid, dists);
#else
    compute_centroid_dists_scalar(q_grid, dists);
#endif
    uint64_t t2 = get_nanos();

    /* Keep top full_nprobe clusters */
    int best_c[IVF_MAX_NPROBE];
    float best_p[IVF_MAX_NPROBE];
#ifdef __AVX2__
    top_n_from_dists_avx2(full_nprobe, dists, best_c, best_p);
#else
    top_n_from_dists_scalar(full_nprobe, dists, best_c, best_p);
#endif
    uint64_t t3 = get_nanos();

    /* Reorder: scan smallest clusters first */
    for (int i = 0; i < full_nprobe - 1; i++) {
        for (int j = i + 1; j < full_nprobe; j++) {
            int si = (int)(g_dataset.block_offsets[best_c[i] + 1] - g_dataset.block_offsets[best_c[i]]);
            int sj = (int)(g_dataset.block_offsets[best_c[j] + 1] - g_dataset.block_offsets[best_c[j]]);
            if (sj < si) {
                int tc = best_c[i]; best_c[i] = best_c[j]; best_c[j] = tc;
                float tp = best_p[i]; best_p[i] = best_p[j]; best_p[j] = tp;
            }
        }
    }
    uint64_t t4 = get_nanos();

    /* Fast pass */
    int result = search_with_nprobe(fast_nprobe, best_c, q);
    uint64_t t5 = get_nanos();

    /* Two-stage: if ambiguous (2 or 3 frauds), re-run with full probes */
    if (result == 2 || result == 3) {
        result = search_with_nprobe(full_nprobe, best_c, q);
    }
    uint64_t t6 = get_nanos();

    g_time_quant += t1 - t0;
    g_time_centroid += t2 - t1;
    g_time_topn += t3 - t2;
    g_time_reorder += t4 - t3;
    g_time_fast += t5 - t4;
    g_time_full += t6 - t5;
    g_inst_count++;

    return result;
}

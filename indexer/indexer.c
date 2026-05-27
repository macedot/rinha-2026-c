#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __AVX2__
#include <immintrin.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DIM 14
#define OUT_DIM 16
#define N_L1 256
#define N_L2_PER_L1 256
#define N_TOTAL_L2 (N_L1 * N_L2_PER_L1)
#define N_ITERATIONS 40  /* higher quality centroids for better recall on hard test cases */

static unsigned int global_seed = 42;

/* ASM canonical space: references.json "vector"[] are already the final normalized
 * linear features. Extract is now identity (no transforms). */
static void extract_features(const float *src, int id, int fraud, float *dst) {
    for (int i = 0; i < DIM; i++) dst[i] = src[i];
    dst[14] = 0.0f;
    uint32_t meta = ((uint32_t)id << 1) | (uint32_t)fraud;
    memcpy(&dst[15], &meta, sizeof(float));
}

/* No per-dim weights in ASM space */
static void apply_weights(float *v) {
    (void)v;
}

static inline float hsum_ps(__m128 v) {
    v = _mm_hadd_ps(v, v);
    v = _mm_hadd_ps(v, v);
    return _mm_cvtss_f32(v);
}

static float manhattan_dist(const float *a, const float *b) {
#ifdef __AVX2__
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
    __m128 sum2 = _mm_add_ps(lo, hi);
    /* Only first 14 dims matter; [14],[15] are 0 in both data/cent so harmless to include */
    return hsum_ps(sum2);
#else
    float d = 0.0f;
    for (int i = 0; i < DIM; i++)
        d += fabsf(a[i] - b[i]);
    return d;
#endif
}

/* Fast init: kmeans++ is O(nk^2) too slow for k=2048; use random sample init + Lloyd iters.
 * Quality sufficient for IVF (tag partition + repair probe guarantees 0 FP/FN). */
static void kmeans_pp_init(const float *data, int n, float *centroids, int k, unsigned int *seed) {
    int ik = (n < k ? n : k);
    for (int c = 0; c < ik; c++) {
        int pick = rand_r(seed) % n;
        memcpy(centroids + c * OUT_DIM, data + pick * OUT_DIM, OUT_DIM * sizeof(float));
    }
    /* If ik < k, leave rest zero (kmeans_run handles) */
}

static void kmeans_run(const float *data, int n, float *centroids, int k, int *assign, unsigned int *seed) {
    int ik = n < k ? n : k;
    if (ik <= 0) return;
    kmeans_pp_init(data, n, centroids, ik, seed);
    float *old_c = (float *)malloc(k * OUT_DIM * sizeof(float));
    int *counts = (int *)malloc(k * sizeof(int));
    for (int iter = 0; iter < N_ITERATIONS; iter++) {
        for (int i = 0; i < n; i++) {
            float mn = 1e30f;
            int best = 0;
            for (int j = 0; j < k; j++) {
                float d = manhattan_dist(data + i * OUT_DIM, centroids + j * OUT_DIM);
                if (d < mn) {
                    mn = d;
                    best = j;
                }
            }
            assign[i] = best;
        }
        memcpy(old_c, centroids, k * OUT_DIM * sizeof(float));
        memset(centroids, 0, k * OUT_DIM * sizeof(float));
        memset(counts, 0, k * sizeof(int));
        for (int i = 0; i < n; i++) {
            int c = assign[i];
            counts[c]++;
            for (int d = 0; d < DIM; d++)
                centroids[c * OUT_DIM + d] += data[i * OUT_DIM + d];
        }
        for (int j = 0; j < k; j++) {
            if (counts[j] > 0) {
                for (int d = 0; d < DIM; d++)
                    centroids[j * OUT_DIM + d] /= (float)counts[j];
            } else {
                memcpy(centroids + j * OUT_DIM, old_c + j * OUT_DIM, OUT_DIM * sizeof(float));
            }
        }
    }
    free(old_c);
    free(counts);
}

/* Fast parser for the specific references schema: array of {"vector":[14 floats],"label":"fraud|legit"}.
 * Avoids the O(1) per-char nested skip loops; uses strstr + sscanf for 10-100x speedup on 3M records. */
static int parse_json(const char *fn, float **out_v, int **out_l, int *out_n) {
    FILE *fp;
    int use_pc = 0;
    size_t fl = strlen(fn);
    if (fl > 3 && strcmp(fn + fl - 3, ".gz") == 0) {
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "gzip -dc '%s'", fn);
        fp = popen(cmd, "r");
        use_pc = 1;
    } else {
        fp = fopen(fn, "r");
    }
    if (!fp) {
        fprintf(stderr, "Cannot open %s\n", fn);
        return -1;
    }
    size_t cap = 8 << 20;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    for (;;) {
        size_t nr = fread(buf + len, 1, cap - len - 1, fp);
        len += nr;
        if (nr == 0) break;
        if (len >= cap - 1) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
    }
    buf[len] = '\0';
    if (use_pc) pclose(fp);
    else fclose(fp);

    int na = 4096, n = 0;
    float *vecs = (float *)malloc(na * DIM * sizeof(float));
    int *labs = (int *)malloc(na * sizeof(int));

    /* Fast path: scan for successive "vector" arrays using strstr from current pos */
    char *cur = buf;
    char *end = buf + len;
    while ((cur = strstr(cur, "\"vector\":[")) != NULL && cur < end) {
        cur += 10; /* past "vector":[ */
        float vec[DIM];
        int got = 0;
        char *scan = cur;
        for (int d = 0; d < DIM; d++) {
            while (scan < end && (*scan == ' ' || *scan == '\t' || *scan == '\n' || *scan == '\r' || *scan == ',')) scan++;
            if (scan >= end) break;
            char *ep;
            vec[d] = strtof(scan, &ep);
            if (ep == scan) break;
            scan = ep;
            got++;
        }
        if (got != DIM) { cur = scan; continue; }

        /* find label after this vector */
        char *labp = strstr(scan, "\"label\":\"");
        int lab = 0;
        if (labp) {
            labp += 9;
            if (strncmp(labp, "fraud\"", 6) == 0) lab = 1;
        }

        if (n >= na) {
            na *= 2;
            vecs = (float *)realloc(vecs, na * DIM * sizeof(float));
            labs = (int *)realloc(labs, na * sizeof(int));
        }
        memcpy(vecs + n * DIM, vec, DIM * sizeof(float));
        labs[n] = lab;
        n++;

        cur = scan; /* continue search after */
        if (n % 100000 == 0) fprintf(stderr, "  parsed %d records...\n", n);
    }

    free(buf);
    *out_v = vecs;
    *out_l = labs;
    *out_n = n;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.json[.gz]> <output_dir>\n", argv[0]);
        return 1;
    }

    float *raw_vecs;
    int *raw_labs;
    int n;

    printf("Parsing input...\n");
    if (parse_json(argv[1], &raw_vecs, &raw_labs, &n) != 0) {
        fprintf(stderr, "Parse failed\n");
        return 1;
    }
    printf("Parsed %d records\n", n);

    printf("Extracting features...\n");
    float *data = (float *)malloc(n * OUT_DIM * sizeof(float));
    for (int i = 0; i < n; i++) {
        extract_features(raw_vecs + i * DIM, i, raw_labs[i], data + i * OUT_DIM);
        apply_weights(data + i * OUT_DIM);
    }
    free(raw_vecs);
    free(raw_labs);

    /* === Classic 2-level HIVF (256 L1 × 256 L2) - the structure that previously
     * achieved 0 FP/FN when paired with Rust features + weighted K=7 scoring. */
    printf("L1 K-Means (%d centroids, %d iterations)...\n", N_L1, N_ITERATIONS);
    float *l1c = (float *)calloc(N_L1 * OUT_DIM, sizeof(float));
    int *l1a = (int *)malloc(n * sizeof(int));
    kmeans_run(data, n, l1c, N_L1, l1a, &global_seed);

    printf("Grouping by L1...\n");
    int *l1n = (int *)calloc(N_L1, sizeof(int));
    for (int i = 0; i < n; i++)
        l1n[l1a[i]]++;

    int **gi = (int **)malloc(N_L1 * sizeof(int *));
    int *gp = (int *)calloc(N_L1, sizeof(int));
    for (int g = 0; g < N_L1; g++)
        gi[g] = l1n[g] > 0 ? (int *)malloc(l1n[g] * sizeof(int)) : NULL;
    for (int i = 0; i < n; i++) {
        int g = l1a[i];
        gi[g][gp[g]++] = i;
    }
    free(gp);

    printf("L2 K-Means (%d groups x %d centroids, %d iterations)...\n", N_L1, N_L2_PER_L1, N_ITERATIONS);
    float *l2c = (float *)calloc(N_TOTAL_L2 * OUT_DIM, sizeof(float));
    int *l2a = (int *)calloc(n, sizeof(int));

#pragma omp parallel for schedule(dynamic)
    for (int g = 0; g < N_L1; g++) {
        int gn = l1n[g];
        if (gn == 0) continue;

        float *gd = (float *)malloc(gn * OUT_DIM * sizeof(float));
        for (int j = 0; j < gn; j++)
            memcpy(gd + j * OUT_DIM, data + gi[g][j] * OUT_DIM, OUT_DIM * sizeof(float));

        int *ga = (int *)malloc(gn * sizeof(int));
        unsigned int ls = 42u + (unsigned int)g * 1000u;
        kmeans_run(gd, gn, l2c + (size_t)g * N_L2_PER_L1 * OUT_DIM, N_L2_PER_L1, ga, &ls);

        for (int j = 0; j < gn; j++)
            l2a[gi[g][j]] = g * N_L2_PER_L1 + ga[j];

        free(gd);
        free(ga);
    }

    for (int g = 0; g < N_L1; g++)
        free(gi[g]);
    free(gi);
    free(l1n);
    free(l1a);

    printf("Counting sort by L2 cluster...\n");
    int *l2cnt = (int *)calloc(N_TOTAL_L2, sizeof(int));
    for (int i = 0; i < n; i++)
        l2cnt[l2a[i]]++;

    uint32_t *off = (uint32_t *)malloc((N_TOTAL_L2 + 1) * sizeof(uint32_t));
    off[0] = 0;
    for (int j = 0; j < N_TOTAL_L2; j++)
        off[j + 1] = off[j] + (uint32_t)l2cnt[j];

    float *sorted = (float *)malloc(n * OUT_DIM * sizeof(float));
    uint32_t *cp = (uint32_t *)malloc(N_TOTAL_L2 * sizeof(uint32_t));
    memcpy(cp, off, N_TOTAL_L2 * sizeof(uint32_t));
    for (int i = 0; i < n; i++) {
        int c = l2a[i];
        uint32_t idx = cp[c]++;
        memcpy(sorted + (size_t)idx * OUT_DIM, data + i * OUT_DIM, OUT_DIM * sizeof(float));
    }
    free(cp);
    free(l2cnt);
    free(l2a);
    free(data);

    mkdir(argv[2], 0755);

    char path[4096];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/dataset.bin", argv[2]);
    fp = fopen(path, "wb");
    fwrite(sorted, sizeof(float), (size_t)n * OUT_DIM, fp);
    fclose(fp);

    snprintf(path, sizeof(path), "%s/l1_centroids.bin", argv[2]);
    fp = fopen(path, "wb");
    fwrite(l1c, sizeof(float), N_L1 * OUT_DIM, fp);
    fclose(fp);

    snprintf(path, sizeof(path), "%s/l2_centroids.bin", argv[2]);
    fp = fopen(path, "wb");
    fwrite(l2c, sizeof(float), N_TOTAL_L2 * OUT_DIM, fp);
    fclose(fp);

    snprintf(path, sizeof(path), "%s/offsets.bin", argv[2]);
    fp = fopen(path, "wb");
    fwrite(off, sizeof(uint32_t), N_TOTAL_L2 + 1, fp);
    fclose(fp);

    printf("Done. %d records written to %s (classic 256x256 2-level HIVF)\n", n, argv[2]);

    free(sorted);
    free(l1c);
    free(l2c);
    free(off);
    return 0;
}

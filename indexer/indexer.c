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
#define N_ITERATIONS 80  /* high quality for fidelity with ASM (squared L2 kmeans + bbox) */

/* ASM 4-partition flat IVF constants */
#define N_PARTITIONS 4
#define N_CLUSTERS 2048

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

/* Squared L2 distance — must match the distance used at search time in knn.c
 * (and the ASM reference). Using Manhattan during kmeans produced clusters
 * whose geometry did not match query-time ranking → persistent FP/FN.
 */
static float l2sq_dist(const float *a, const float *b) {
#ifdef __AVX2__
    __m256 q0 = _mm256_loadu_ps(a);
    __m256 q1 = _mm256_loadu_ps(a + 8);
    __m256 c0 = _mm256_loadu_ps(b);
    __m256 c1 = _mm256_loadu_ps(b + 8);
    __m256 d0 = _mm256_sub_ps(q0, c0);
    __m256 d1 = _mm256_sub_ps(q1, c1);
    __m256 s0 = _mm256_mul_ps(d0, d0);
    __m256 s1 = _mm256_mul_ps(d1, d1);
    __m256 s = _mm256_add_ps(s0, s1);
    __m128 lo = _mm256_castps256_ps128(s);
    __m128 hi = _mm256_extractf128_ps(s, 1);
    /* Only first 14 dims matter; [14],[15] are 0 in both data/cent */
    return hsum_ps(_mm_add_ps(lo, hi));
#else
    float sum = 0.0f;
    for (int i = 0; i < DIM; i++) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
#endif
}

/* Random sample init (kmeans++ is too slow for k=2048 on 3M points).
 * Lloyd iterations with squared L2 assignment for fidelity with ASM search. */
static void random_init(const float *data, int n, float *centroids, int k, unsigned int *seed) {
    int ik = (n < k ? n : k);
    for (int c = 0; c < ik; c++) {
        int pick = rand_r(seed) % n;
        memcpy(centroids + c * OUT_DIM, data + pick * OUT_DIM, OUT_DIM * sizeof(float));
    }
}

static void kmeans_run(const float *data, int n, float *centroids, int k, int *assign, unsigned int *seed) {
    int ik = n < k ? n : k;
    if (ik <= 0) return;
    random_init(data, n, centroids, ik, seed);
    float *old_c = (float *)malloc(k * OUT_DIM * sizeof(float));
    int *counts = (int *)malloc(k * sizeof(int));
    for (int iter = 0; iter < N_ITERATIONS; iter++) {
        for (int i = 0; i < n; i++) {
            float mn = 1e30f;
            int best = 0;
            for (int j = 0; j < k; j++) {
                float d = l2sq_dist(data + i * OUT_DIM, centroids + j * OUT_DIM);
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

    /* === ASM 4-partition flat IVF (2048 clusters per partition) ===
     * Tag = ((v[11]>0.5)<<1) | (v[5]>=0 ? 1 : 0)
     * Squared-L2 kmeans (fidelity with search/ASM) + per-cluster float bbox for now.
     */
    printf("Computing ASM tags and grouping records into 4 partitions...\n");
    int part_cnt[N_PARTITIONS] = {0};
    int *part_tags = (int *)malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) {
        float v5 = data[i * OUT_DIM + 5];
        float v11 = data[i * OUT_DIM + 11];
        int t = ((v11 > 0.5f) << 1) | ((v5 >= 0.0f) ? 1 : 0);
        part_tags[i] = t;
        part_cnt[t]++;
    }

    int *gi[N_PARTITIONS];
    int gpos[N_PARTITIONS] = {0};
    for (int t = 0; t < N_PARTITIONS; t++) {
        gi[t] = (part_cnt[t] > 0) ? (int *)malloc((size_t)part_cnt[t] * sizeof(int)) : NULL;
    }
    for (int i = 0; i < n; i++) {
        int t = part_tags[i];
        gi[t][gpos[t]++] = i;
    }
    free(part_tags);

    /* Per-partition flat 2048-cluster kmeans (ASM style) */
    mkdir(argv[2], 0755);

    for (int t = 0; t < N_PARTITIONS; t++) {
        int pn = part_cnt[t];
        if (pn == 0) {
            /* Write minimal empty structures */
            printf("Partition %d: 0 records, skipping\n", t);
            continue;
        }

        printf("Partition %d: extracting %d records...\n", t, pn);
        float *pd = (float *)malloc((size_t)pn * OUT_DIM * sizeof(float));
        for (int j = 0; j < pn; j++) {
            int orig = gi[t][j];
            memcpy(pd + (size_t)j * OUT_DIM, data + (size_t)orig * OUT_DIM, OUT_DIM * sizeof(float));
        }

        printf("Partition %d K-Means (2048 clusters, %d iters)...\n", t, N_ITERATIONS);
        float *cent = (float *)calloc((size_t)N_CLUSTERS * OUT_DIM, sizeof(float));
        int *ass = (int *)malloc((size_t)pn * sizeof(int));
        unsigned int ls = 42u + (unsigned int)t * 10007u;
        kmeans_run(pd, pn, cent, N_CLUSTERS, ass, &ls);

        /* Counting sort within partition */
        printf("  Counting sort for partition %d...\n", t);
        int *ccnt = (int *)calloc(N_CLUSTERS, sizeof(int));
        for (int j = 0; j < pn; j++) ccnt[ass[j]]++;

        uint32_t *poff = (uint32_t *)malloc(((size_t)N_CLUSTERS + 1) * sizeof(uint32_t));
        poff[0] = 0;
        for (int c = 0; c < N_CLUSTERS; c++)
            poff[c + 1] = poff[c] + (uint32_t)ccnt[c];

        float *psorted = (float *)malloc((size_t)pn * OUT_DIM * sizeof(float));
        uint32_t *cp = (uint32_t *)malloc((size_t)N_CLUSTERS * sizeof(uint32_t));
        memcpy(cp, poff, (size_t)N_CLUSTERS * sizeof(uint32_t));
        for (int j = 0; j < pn; j++) {
            int c = ass[j];
            uint32_t idx = cp[c]++;
            memcpy(psorted + (size_t)idx * OUT_DIM, pd + (size_t)j * OUT_DIM, OUT_DIM * sizeof(float));
        }

        /* Write per-partition files (centroids + offsets + dataset for now; bbox later) */
        char path[4096];
        FILE *fp;
        snprintf(path, sizeof(path), "%s/part%d_centroids.bin", argv[2], t);
        fp = fopen(path, "wb");
        fwrite(cent, sizeof(float), (size_t)N_CLUSTERS * OUT_DIM, fp);
        fclose(fp);

        snprintf(path, sizeof(path), "%s/part%d_offsets.bin", argv[2], t);
        fp = fopen(path, "wb");
        fwrite(poff, sizeof(uint32_t), (size_t)N_CLUSTERS + 1, fp);
        fclose(fp);

        snprintf(path, sizeof(path), "%s/part%d_dataset.bin", argv[2], t);
        fp = fopen(path, "wb");
        fwrite(psorted, sizeof(float), (size_t)pn * OUT_DIM, fp);
        fclose(fp);

        printf("Partition %d: %d records, 2048 clusters written\n", t, pn);

        /* Compute per-cluster axis-aligned bbox (float for now; next fidelity step will move to i16) */
        float *bmin = (float *)malloc((size_t)N_CLUSTERS * DIM * sizeof(float));
        float *bmax = (float *)malloc((size_t)N_CLUSTERS * DIM * sizeof(float));
        for (int c = 0; c < N_CLUSTERS; c++) {
            for (int d = 0; d < DIM; d++) {
                bmin[c*DIM + d] = 1e30f;
                bmax[c*DIM + d] = -1e30f;
            }
        }
        for (int c = 0; c < N_CLUSTERS; c++) {
            uint32_t start = poff[c];
            uint32_t end = poff[c+1];
            for (uint32_t i = start; i < end; i++) {
                const float *v = psorted + (size_t)i * OUT_DIM;
                for (int d = 0; d < DIM; d++) {
                    if (v[d] < bmin[c*DIM + d]) bmin[c*DIM + d] = v[d];
                    if (v[d] > bmax[c*DIM + d]) bmax[c*DIM + d] = v[d];
                }
            }
        }

        char bpath[4096];
        FILE *bfp;
        snprintf(bpath, sizeof(bpath), "%s/part%d_bbox_min.bin", argv[2], t);
        bfp = fopen(bpath, "wb");
        fwrite(bmin, sizeof(float), (size_t)N_CLUSTERS * DIM, bfp);
        fclose(bfp);

        snprintf(bpath, sizeof(bpath), "%s/part%d_bbox_max.bin", argv[2], t);
        bfp = fopen(bpath, "wb");
        fwrite(bmax, sizeof(float), (size_t)N_CLUSTERS * DIM, bfp);
        fclose(bfp);

        printf("  BBox written for partition %d\n", t);

        free(bmin);
        free(bmax);

        free(cp);
        free(psorted);
        free(poff);
        free(ccnt);
        free(ass);
        free(cent);
        free(pd);
    }

    /* cleanup */
    for (int t = 0; t < N_PARTITIONS; t++) if (gi[t]) free(gi[t]);
    free(data);

    printf("Done. ASM 4-partition index written to %s\n", argv[2]);
    return 0;
}

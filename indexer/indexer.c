#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DIM 14
#define OUT_DIM 16
#define N_L1 256
#define N_L2_PER_L1 256
#define N_TOTAL_L2 65536
#define N_ITERATIONS 20

static const float FEATURE_WEIGHTS[OUT_DIM] = {
    1.0038165f, 0.665417f, 0.8668326f, 0.5379362f,
    0.5f, 0.3f, 0.3701757f, 1.0f,
    1.2f, 1.2648705f, 0.81239825f, 1.051987f,
    0.8247206f, 2.0315619f, 0.0f, 0.0f
};

static void extract_features(const float *src, int id, int fraud, float *dst) {
    dst[0] = logf(1.0f + src[0] * 10000.0f) / logf(1.0f + 10000.0f);
    dst[1] = src[1];
    dst[2] = src[2];
    float hour = src[3] * 23.0f;
    dst[3] = sinf(hour * 2.0f * (float)M_PI / 24.0f);
    dst[4] = cosf(hour * 2.0f * (float)M_PI / 24.0f);
    float dow = src[4] * 6.0f;
    dst[5] = sinf(dow * 2.0f * (float)M_PI / 7.0f);
    dst[6] = cosf(dow * 2.0f * (float)M_PI / 7.0f);
    if (src[5] < 0.0f)
        dst[7] = -1.0f;
    else
        dst[7] = logf(1.0f + src[5] * 1440.0f) / logf(1.0f + 1440.0f);
    dst[8] = src[6];
    dst[9] = src[7];
    dst[10] = src[8];
    int packed = (src[9] >= 0.5f ? 1 : 0) + (src[10] >= 0.5f ? 2 : 0) + (src[11] >= 0.5f ? 4 : 0);
    dst[11] = (float)packed / 7.0f;
    dst[12] = src[12];
    dst[13] = src[13];
    dst[14] = 0.0f;
    uint32_t meta = ((uint32_t)id << 1) | (uint32_t)fraud;
    memcpy(&dst[15], &meta, sizeof(float));
}

static void apply_weights(float *v) {
    for (int i = 0; i < DIM; i++)
        v[i] *= FEATURE_WEIGHTS[i];
}

static float manhattan_dist(const float *a, const float *b) {
    float d = 0.0f;
    for (int i = 0; i < DIM; i++)
        d += fabsf(a[i] - b[i]);
    return d;
}

static void kmeans_pp_init(const float *data, int n, float *centroids, int k, unsigned int *seed) {
    int first = rand_r(seed) % n;
    memcpy(centroids, data + first * OUT_DIM, OUT_DIM * sizeof(float));
    float *dists = (float *)malloc(n * sizeof(float));
    for (int c = 1; c < k; c++) {
        float total = 0.0f;
        for (int i = 0; i < n; i++) {
            float mn = 1e30f;
            for (int j = 0; j < c; j++) {
                float d = manhattan_dist(data + i * OUT_DIM, centroids + j * OUT_DIM);
                if (d < mn) mn = d;
            }
            dists[i] = mn;
            total += mn;
        }
        float r = (float)rand_r(seed) / (float)RAND_MAX * total;
        float acc = 0.0f;
        int chosen = n - 1;
        for (int i = 0; i < n; i++) {
            acc += dists[i];
            if (acc >= r) {
                chosen = i;
                break;
            }
        }
        memcpy(centroids + c * OUT_DIM, data + chosen * OUT_DIM, OUT_DIM * sizeof(float));
    }
    free(dists);
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
    size_t cap = 4 << 20;
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

    size_t p = 0;
    while (p < len && buf[p] != '[') p++;
    if (p < len) p++;

    while (p < len) {
        while (p < len && buf[p] != '{' && buf[p] != ']') p++;
        if (p >= len || buf[p] == ']') break;
        p++;

        float vec[DIM] = {0};
        int lab = 0;

        while (p < len && buf[p] != '}') {
            while (p < len && buf[p] != '"' && buf[p] != '}') p++;
            if (p >= len || buf[p] == '}') break;
            p++;

            char key[64] = {0};
            int ki = 0;
            while (p < len && buf[p] != '"' && ki < 63)
                key[ki++] = buf[p++];
            if (p < len) p++;

            while (p < len && buf[p] != ':') p++;
            if (p < len) p++;
            while (p < len && (buf[p] == ' ' || buf[p] == '\t' || buf[p] == '\n' || buf[p] == '\r'))
                p++;

            if (strcmp(key, "vector") == 0) {
                while (p < len && buf[p] != '[') p++;
                if (p < len) p++;
                for (int d = 0; d < DIM; d++) {
                    while (p < len && (buf[p] == ' ' || buf[p] == '\t' || buf[p] == '\n' ||
                                       buf[p] == '\r' || buf[p] == ','))
                        p++;
                    char *end;
                    vec[d] = strtof(buf + p, &end);
                    p = (size_t)(end - buf);
                }
                while (p < len && buf[p] != ']') p++;
                if (p < len) p++;
            } else if (strcmp(key, "label") == 0) {
                while (p < len && buf[p] != '"') p++;
                if (p < len) p++;
                char lbl[32] = {0};
                int li = 0;
                while (p < len && buf[p] != '"' && li < 31)
                    lbl[li++] = buf[p++];
                if (p < len) p++;
                lab = strcmp(lbl, "fraud") == 0 ? 1 : 0;
            } else {
                if (p < len && buf[p] == '"') {
                    p++;
                    while (p < len && buf[p] != '"') {
                        if (buf[p] == '\\') p++;
                        p++;
                    }
                    if (p < len) p++;
                } else if (p < len && buf[p] == '[') {
                    int dep = 1;
                    p++;
                    while (p < len && dep > 0) {
                        if (buf[p] == '[') dep++;
                        else if (buf[p] == ']') dep--;
                        else if (buf[p] == '"') {
                            p++;
                            while (p < len && buf[p] != '"') {
                                if (buf[p] == '\\') p++;
                                p++;
                            }
                        }
                        p++;
                    }
                } else if (p < len && buf[p] == '{') {
                    int dep = 1;
                    p++;
                    while (p < len && dep > 0) {
                        if (buf[p] == '{') dep++;
                        else if (buf[p] == '}') dep--;
                        else if (buf[p] == '"') {
                            p++;
                            while (p < len && buf[p] != '"') {
                                if (buf[p] == '\\') p++;
                                p++;
                            }
                        }
                        p++;
                    }
                } else {
                    while (p < len && buf[p] != ',' && buf[p] != '}' && buf[p] != ']')
                        p++;
                }
            }
        }
        if (p < len && buf[p] == '}') p++;

        if (n >= na) {
            na *= 2;
            vecs = (float *)realloc(vecs, na * DIM * sizeof(float));
            labs = (int *)realloc(labs, na * sizeof(int));
        }
        memcpy(vecs + n * DIM, vec, DIM * sizeof(float));
        labs[n] = lab;
        n++;
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

    unsigned int seed = 42;
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

    printf("L1 K-Means (%d centroids, %d iterations)...\n", N_L1, N_ITERATIONS);
    float *l1c = (float *)calloc(N_L1 * OUT_DIM, sizeof(float));
    int *l1a = (int *)malloc(n * sizeof(int));
    kmeans_run(data, n, l1c, N_L1, l1a, &seed);

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
        unsigned int ls = 42 + (unsigned int)g * 1000u;
        kmeans_run(gd, gn, l2c + g * N_L2_PER_L1 * OUT_DIM, N_L2_PER_L1, ga, &ls);

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
        memcpy(sorted + idx * OUT_DIM, data + i * OUT_DIM, OUT_DIM * sizeof(float));
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

    printf("Done. %d records written to %s\n", n, argv[2]);

    free(sorted);
    free(l1c);
    free(l2c);
    free(off);
    return 0;
}

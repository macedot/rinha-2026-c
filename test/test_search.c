#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/vectorizer.h"
#include "src/knn.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: test <file>\n"); return 1; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *data = malloc(sz + 1); fread(data, 1, sz, f); data[sz] = 0; fclose(f);

    if (rinha_load_index("indexer/test_output") != 0) { fprintf(stderr, "failed to load index\n"); return 1; }

    char *p = data;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;

    for (int idx = 0; idx <= 19; idx++) {
        while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
        if (!*p || *p == ']') break;
        char *start = p; int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
        size_t len = p - start;
        float q[16];
        int ok = vectorizer_build(start, len, q);
        if (!ok) { printf("[%d] PARSE FAILED\n", idx); continue; }
        float fraud_score;
        int approved = rinha_search(q, &fraud_score);
        printf("[%d] approved=%d fraud_score=%.1f\n", idx, approved, fraud_score);
        while (*p && (*p == ' ' || *p == '\n' || *p == ',')) p++;
    }
    free(data);
    return 0;
}

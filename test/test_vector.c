#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/vectorizer.h"
#include "src/knn.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_vector <file>\n"); return 1; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(sz + 1);
    fread(data, 1, sz, f);
    data[sz] = 0;
    fclose(f);

    /* Parse JSON array manually - find each { } */
    char *p = data;
    /* skip [ */
    while (*p && *p != '[') p++;
    if (*p == '[') p++;

    for (int idx = 0; idx <= 5; idx++) {
        /* skip to next { */
        while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
        if (!*p || *p == ']') break;

        /* find matching } */
        char *start = p;
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
        size_t len = p - start;

        float q[14];
        int ok = vectorizer_build(start, len, q);
        if (!ok) { printf("[%d] PARSE FAILED\n", idx); continue; }

        printf("[%d] ", idx);
        for (int j = 0; j < 14; j++) {
            if (j > 0) printf(", ");
            printf("%.6f", q[j]);
        }
        printf("\n");

        /* skip comma */
        while (*p && (*p == ' ' || *p == '\n' || *p == ',')) p++;
    }
    free(data);
    return 0;
}

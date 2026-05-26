#ifndef VECTORIZER_H
#define VECTORIZER_H

#include <stddef.h>
#include <stdint.h>

#define VEC_DIM 16

int vectorizer_build(const char *body, size_t body_len, float out[VEC_DIM]);

#endif

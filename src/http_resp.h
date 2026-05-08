#ifndef HTTP_RESP_H
#define HTTP_RESP_H

#include <stddef.h>
#include <stdint.h>

extern const char *score_full[6];
extern const char *resp_ready;
extern const char *resp_not_found;
extern const char *resp_bad_req;
extern const char *resp_internal_err;

static inline const char *score_for(uint8_t fraud_count) {
    return score_full[fraud_count > 5 ? 0 : fraud_count];
}

#endif

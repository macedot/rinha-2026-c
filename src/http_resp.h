#ifndef HTTP_RESP_H
#define HTTP_RESP_H

#include <stddef.h>
#include <stdint.h>

extern const char *score_full[6];
extern const size_t score_len[6];
extern const char *resp_ready;
extern const size_t resp_ready_len;
extern const char *resp_not_found;
extern const size_t resp_not_found_len;
extern const char *resp_bad_req;
extern const size_t resp_bad_req_len;
extern const char *resp_internal_err;
extern const size_t resp_internal_err_len;

static inline const char *score_for(uint8_t fraud_count) {
    return score_full[fraud_count > 5 ? 0 : fraud_count];
}

#endif

#include "vectorizer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

static float round4(float x) {
    return roundf(x * 10000.0f) * 0.0001f;
}

static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static float mcc_risk_f(unsigned mcc) {
    switch (mcc) {
        case 5411: return 0.15f;
        case 5812: return 0.30f;
        case 5912: return 0.20f;
        case 5944: return 0.45f;
        case 7801: return 0.80f;
        case 7802: return 0.75f;
        case 7995: return 0.85f;
        case 4511: return 0.35f;
        case 5311: return 0.25f;
        case 5999: return 0.50f;
        default:   return 0.50f;
    }
}

/* --- Single-pass JSON parser (no backtracking) --- */

typedef struct {
    const char *s;
    size_t len;
    size_t pos;
} parser_t;

static inline void skip_ws(parser_t *p) {
    while (p->pos < p->len && (p->s[p->pos] == ' ' || p->s[p->pos] == '\n' ||
                               p->s[p->pos] == '\r' || p->s[p->pos] == '\t'))
        p->pos++;
}

static inline void expect_char(parser_t *p, char c) {
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == c) p->pos++;
}

static inline void skip_string(parser_t *p) {
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '"') {
        p->pos++;
        while (p->pos < p->len && p->s[p->pos] != '"') p->pos++;
        if (p->pos < p->len) p->pos++;
    }
}

static inline float parse_f32(parser_t *p) {
    skip_ws(p);
    int neg = 0;
    if (p->pos < p->len && p->s[p->pos] == '-') { neg = 1; p->pos++; }
    unsigned int_part = 0;
    while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
        int_part = int_part * 10 + (unsigned)(p->s[p->pos] - '0');
        p->pos++;
    }
    float v = (float)int_part;
    if (p->pos < p->len && p->s[p->pos] == '.') {
        p->pos++;
        unsigned frac = 0, frac_digits = 0;
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9' && frac_digits < 6) {
            frac = frac * 10 + (unsigned)(p->s[p->pos] - '0');
            frac_digits++;
            p->pos++;
        }
        if (frac_digits > 0) {
            static const float POW10_NEG[7] = {
                1.0f, 0.1f, 0.01f, 0.001f, 0.0001f, 0.00001f, 0.000001f
            };
            v += (float)frac * POW10_NEG[frac_digits];
        }
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    }
    return neg ? -v : v;
}

static inline int parse_bool(parser_t *p) {
    skip_ws(p);
    if (p->pos + 4 <= p->len && memcmp(p->s + p->pos, "true", 4) == 0) {
        p->pos += 4; return 1;
    }
    if (p->pos + 5 <= p->len && memcmp(p->s + p->pos, "false", 5) == 0) {
        p->pos += 5; return 0;
    }
    return 0;
}

static inline void parse_string_bounds(parser_t *p, const char **start, size_t *len) {
    skip_ws(p);
    *start = NULL; *len = 0;
    if (p->pos < p->len && p->s[p->pos] == '"') {
        p->pos++;
        *start = p->s + p->pos;
        while (p->pos < p->len && p->s[p->pos] != '"') p->pos++;
        *len = (size_t)(p->s + p->pos - *start);
        if (p->pos < p->len) p->pos++;
    }
}

static inline unsigned iso_hour(const char *s, size_t len) {
    if (len < 13) return 0;
    unsigned h = (unsigned)(s[11] - '0') * 10 + (unsigned)(s[12] - '0');
    return h > 23 ? 23 : h;
}

static inline void iso_ymdhm(const char *s, size_t len,
                             int *y, unsigned *m, unsigned *d,
                             unsigned *h, unsigned *mi) {
    *y = 0; *m = 1; *d = 1; *h = 0; *mi = 0;
    if (len < 16) return;
    *y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    *m = (unsigned)(s[5]-'0')*10 + (unsigned)(s[6]-'0');
    *d = (unsigned)(s[8]-'0')*10 + (unsigned)(s[9]-'0');
    *h = (unsigned)(s[11]-'0')*10 + (unsigned)(s[12]-'0');
    *mi = (unsigned)(s[14]-'0')*10 + (unsigned)(s[15]-'0');
}

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    if (m <= 2) y -= 1;
    int era = (y >= 0) ? y / 400 : (y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (m > 2)
        ? (153 * (m - 3) + 2) / 5 + d - 1
        : (153 * (m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static inline size_t weekday_from_iso(const char *s, size_t len) {
    int y; unsigned m, d, h, mi;
    iso_ymdhm(s, len, &y, &m, &d, &h, &mi);
    int64_t days = days_from_civil(y, m, d);
    int64_t w = ((days + 3) % 7 + 7) % 7;
    return (size_t)w;
}

static inline int64_t iso_to_epoch_minutes(const char *s, size_t len) {
    int y; unsigned m, d, h, mi;
    iso_ymdhm(s, len, &y, &m, &d, &h, &mi);
    int64_t days = days_from_civil(y, m, d);
    return days * 1440 + (int64_t)h * 60 + (int64_t)mi;
}

int vectorizer_build(const char *body, size_t body_len, float out[VEC_DIM]) {
    memset(out, 0, VEC_DIM * sizeof(float));

    parser_t p = { body, body_len, 0 };

    /* { "id": "...", ... } */
    expect_char(&p, '{');
    skip_string(&p); expect_char(&p, ':'); skip_string(&p); expect_char(&p, ',');

    /* "transaction": { amount, installments, requested_at } */
    skip_string(&p); expect_char(&p, ':'); expect_char(&p, '{');
    skip_string(&p); expect_char(&p, ':'); float amount = parse_f32(&p); expect_char(&p, ',');
    skip_string(&p); expect_char(&p, ':'); float installments = parse_f32(&p); expect_char(&p, ',');
    skip_string(&p); expect_char(&p, ':'); const char *req_ts; size_t req_ts_len; parse_string_bounds(&p, &req_ts, &req_ts_len); expect_char(&p, '}'); expect_char(&p, ',');

    /* "customer": { avg_amount, tx_count_24h, known_merchants } */
    skip_string(&p); expect_char(&p, ':'); expect_char(&p, '{');
    skip_string(&p); expect_char(&p, ':'); float customer_avg_amount = parse_f32(&p); expect_char(&p, ',');
    skip_string(&p); expect_char(&p, ':'); float tx_count_24h = parse_f32(&p); expect_char(&p, ',');
    skip_string(&p); expect_char(&p, ':');

    /* Parse known_merchants array into temp buffer */
    const char *merchants[32];
    size_t merchant_lens[32];
    int num_merchants = 0;
    expect_char(&p, '[');
    while (p.pos < p.len && p.s[p.pos] != ']') {
        const char *m_start; size_t m_len;
        size_t prev = p.pos;
        parse_string_bounds(&p, &m_start, &m_len);
        if (p.pos == prev) break; /* no progress — malformed input */
        if (num_merchants < 32) {
            merchants[num_merchants] = m_start;
            merchant_lens[num_merchants] = m_len;
            num_merchants++;
        }
        skip_ws(&p);
        if (p.pos < p.len && p.s[p.pos] == ',') p.pos++;
    }
    if (p.pos < p.len && p.s[p.pos] == ']') p.pos++;
    expect_char(&p, '}'); expect_char(&p, ',');

    /* "merchant": { id, mcc, avg_amount } */
    skip_string(&p); expect_char(&p, ':'); expect_char(&p, '{');
    skip_string(&p); expect_char(&p, ':'); const char *merchant_id; size_t merchant_id_len; parse_string_bounds(&p, &merchant_id, &merchant_id_len); expect_char(&p, ',');
    skip_string(&p); expect_char(&p, ':'); const char *mcc_str; size_t mcc_len; parse_string_bounds(&p, &mcc_str, &mcc_len); expect_char(&p, ',');
    skip_string(&p); expect_char(&p, ':'); float merchant_avg_amount = parse_f32(&p); expect_char(&p, '}'); expect_char(&p, ',');

    /* Check if merchant_id is in known_merchants */
    int known_merchant = 0;
    for (int i = 0; i < num_merchants; i++) {
        if (merchant_lens[i] == merchant_id_len && memcmp(merchants[i], merchant_id, merchant_id_len) == 0) {
            known_merchant = 1;
            break;
        }
    }

    /* "terminal": { is_online, card_present, km_from_home } */
    skip_string(&p); expect_char(&p, ':'); expect_char(&p, '{');
    skip_string(&p); expect_char(&p, ':'); int is_online = parse_bool(&p); expect_char(&p, ',');
    skip_string(&p); expect_char(&p, ':'); int card_present = parse_bool(&p); expect_char(&p, ',');
    skip_string(&p); expect_char(&p, ':'); float km_from_home = parse_f32(&p); expect_char(&p, '}'); expect_char(&p, ',');

    /* "last_transaction": null or { timestamp, km_from_current } */
    skip_string(&p); expect_char(&p, ':');
    float minutes_since_last_tx = -1.0f;
    float km_from_current = -1.0f;
    skip_ws(&p);
    if (p.pos < p.len && p.s[p.pos] == 'n') {
        p.pos += 4;
    } else {
        expect_char(&p, '{');
        skip_string(&p); expect_char(&p, ':'); const char *last_ts; size_t last_ts_len; parse_string_bounds(&p, &last_ts, &last_ts_len); expect_char(&p, ',');
        skip_string(&p); expect_char(&p, ':'); km_from_current = parse_f32(&p); expect_char(&p, '}');
        int64_t req_mins = iso_to_epoch_minutes(req_ts, req_ts_len);
        int64_t last_mins = iso_to_epoch_minutes(last_ts, last_ts_len);
        int64_t diff = req_mins - last_mins;
        if (diff < 0) diff = -diff;
        minutes_since_last_tx = clamp01((float)diff / 1440.0f);
        km_from_current = clamp01(km_from_current / 1000.0f);
    }

    /* Parse MCC */
    unsigned mcc = 0;
    for (size_t i = 0; i < mcc_len && i < 4; i++) {
        if (mcc_str[i] >= '0' && mcc_str[i] <= '9')
            mcc = mcc * 10 + (unsigned)(mcc_str[i] - '0');
    }

    float ratio = (customer_avg_amount > 0.0f)
        ? (amount / customer_avg_amount) / 10.0f
        : 1.0f;

    out[0]  = clamp01(amount / 10000.0f);
    out[1]  = clamp01(installments / 12.0f);
    out[2]  = clamp01(ratio);
    out[3]  = clamp01((float)iso_hour(req_ts, req_ts_len) / 23.0f);
    out[4]  = clamp01((float)weekday_from_iso(req_ts, req_ts_len) / 6.0f);
    out[5]  = minutes_since_last_tx;
    out[6]  = km_from_current;
    out[7]  = clamp01(km_from_home / 1000.0f);
    out[8]  = clamp01(tx_count_24h / 20.0f);
    out[9]  = is_online ? 1.0f : 0.0f;
    out[10] = card_present ? 1.0f : 0.0f;
    out[11] = known_merchant ? 0.0f : 1.0f;
    out[12] = mcc_risk_f(mcc);
    out[13] = clamp01(merchant_avg_amount / 10000.0f);

    return 1;
}

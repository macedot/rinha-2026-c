#include "vectorizer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

/* Normalization constants (match Rust normalization.json) */
static const float MAX_AMOUNT = 10000.0f;
static const float MAX_INSTALLMENTS = 12.0f;
static const float MAX_AVG_RATIO = 10.0f;
static const float MAX_MINUTES = 1440.0f;
static const float MAX_KM = 1000.0f;
static const float MAX_TX_COUNT = 20.0f;
static const float MAX_MERCHANT_AVG = 10000.0f;

/* MCC risk lookup table (65536 entries, initialized once) */
static float mcc_risks[65536];
static int mcc_initialized = 0;

static void init_mcc_risks(void) {
    if (mcc_initialized) return;
    for (int i = 0; i < 65536; i++) mcc_risks[i] = 0.5f;
    mcc_risks[5411] = 0.15f;
    mcc_risks[5812] = 0.30f;
    mcc_risks[5912] = 0.20f;
    mcc_risks[5944] = 0.45f;
    mcc_risks[7801] = 0.80f;
    mcc_risks[7802] = 0.75f;
    mcc_risks[7995] = 0.85f;
    mcc_risks[4511] = 0.35f;
    mcc_risks[5311] = 0.25f;
    mcc_risks[5999] = 0.50f;
    mcc_initialized = 1;
}

static inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* --- Timestamp parsing (match Rust logic exactly) --- */

static inline int parse_iso_hour(const char *s, size_t len) {
    if (len < 13) return 0;
    int h = (s[11] - '0') * 10 + (s[12] - '0');
    return h;
}

static inline int parse_iso_dow(const char *s, size_t len) {
    if (len < 10) return 0;
    int year = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int month = (s[5]-'0')*10 + (s[6]-'0');
    int day = (s[8]-'0')*10 + (s[9]-'0');

    if (month < 1 || month > 12) return 0;
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    if (month < 3) y -= 1;
    int dow = (y + y/4 - y/100 + y/400 + t[month-1] + day) % 7;
    // Convert Sunday=0 to Monday=0
    return (dow == 0) ? 6 : (dow - 1);
}

static inline int64_t iso_to_epoch_minutes(const char *s, size_t len) {
    if (len < 16) return 0;
    int year = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int month = (s[5]-'0')*10 + (s[6]-'0');
    int day = (s[8]-'0')*10 + (s[9]-'0');
    int hour = (s[11]-'0')*10 + (s[12]-'0');
    int min = (s[14]-'0')*10 + (s[15]-'0');

    // Simplified (matches Rust indexer)
    int64_t total_days = (int64_t)(year - 2000) * 365 + (int64_t)(year - 2000) / 4;
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int i = 0; i < month - 1; i++) total_days += month_days[i];
    if (month > 2 && year % 4 == 0) total_days += 1;
    total_days += day;
    return total_days * 1440 + hour * 60 + min;
}

/* --- Single-pass JSON parser (unchanged logic) --- */

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

int vectorizer_build(const char *body, size_t body_len, float out[VEC_DIM]) {
    init_mcc_risks();
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
        if (p.pos == prev) break;
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
    int has_last_tx = 0;
    const char *last_ts = NULL; size_t last_ts_len = 0;
    float km_from_current = -1.0f;
    skip_ws(&p);
    if (p.pos < p.len && p.s[p.pos] == 'n') {
        p.pos += 4;
    } else {
        has_last_tx = 1;
        expect_char(&p, '{');
        skip_string(&p); expect_char(&p, ':'); parse_string_bounds(&p, &last_ts, &last_ts_len); expect_char(&p, ',');
        skip_string(&p); expect_char(&p, ':'); km_from_current = parse_f32(&p); expect_char(&p, '}');
    }

    /* Parse MCC */
    unsigned mcc = 0;
    for (size_t i = 0; i < mcc_len && i < 4; i++) {
        if (mcc_str[i] >= '0' && mcc_str[i] <= '9')
            mcc = mcc * 10 + (unsigned)(mcc_str[i] - '0');
    }

    /* --- Compute new 16-dim feature vector (matches Rust) --- */

    // 0. ln(1 + amount) / ln(1 + max_amount)
    out[0] = logf(1.0f + amount) / logf(1.0f + MAX_AMOUNT);

    // 1. installments / max_installments
    out[1] = clamp01(installments / MAX_INSTALLMENTS);

    // 2. amount_vs_avg_ratio
    float ratio = (customer_avg_amount > 0.0f)
        ? (amount / customer_avg_amount) / MAX_AVG_RATIO
        : 1.0f;
    out[2] = clamp01(ratio);

    // 3. hour_sin, 4. hour_cos
    int hour = parse_iso_hour(req_ts, req_ts_len);
    int dow = parse_iso_dow(req_ts, req_ts_len);
    float hour_rad = hour * 2.0f * (float)M_PI / 24.0f;
    float day_rad = dow * 2.0f * (float)M_PI / 7.0f;
    out[3] = sinf(hour_rad);
    out[4] = cosf(hour_rad);

    // 5. day_sin, 6. day_cos
    out[5] = sinf(day_rad);
    out[6] = cosf(day_rad);

    // 7. ln(1 + minutes) / ln(1 + max_minutes)
    // 8. km_from_last_tx
    if (has_last_tx) {
        int64_t req_mins = iso_to_epoch_minutes(req_ts, req_ts_len);
        int64_t last_mins = iso_to_epoch_minutes(last_ts, last_ts_len);
        int64_t diff = req_mins - last_mins;
        if (diff < 0) diff = -diff;
        out[7] = logf(1.0f + (float)diff) / logf(1.0f + MAX_MINUTES);
        out[8] = clamp01(km_from_current / MAX_KM);
    } else {
        out[7] = -1.0f;
        out[8] = -1.0f;
    }

    // 9. km_from_home
    out[9] = clamp01(km_from_home / MAX_KM);

    // 10. tx_count_24h
    out[10] = clamp01(tx_count_24h / MAX_TX_COUNT);

    // 11. Packed binary: (online + 2*card + 4*unknown) / 7
    float packed = 0.0f;
    if (is_online) packed += 1.0f;
    if (card_present) packed += 2.0f;
    if (!known_merchant) packed += 4.0f;
    out[11] = packed / 7.0f;

    // 12. mcc_risk
    out[12] = (mcc < 65536) ? mcc_risks[mcc] : 0.5f;

    // 13. merchant_avg_amount
    out[13] = clamp01(merchant_avg_amount / MAX_MERCHANT_AVG);

    // 14, 15. padding / placeholder
    out[14] = 0.0f;
    out[15] = 0.0f;

    return 1;
}

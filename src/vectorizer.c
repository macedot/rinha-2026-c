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

/* --- JSON helpers (zero-allocation) --- */

static size_t skip_ws(const char *p, size_t len, size_t pos) {
    while (pos < len && (p[pos] == ' ' || p[pos] == '\n' || p[pos] == '\r' || p[pos] == '\t'))
        pos++;
    return pos;
}

static int find_str(const char *data, size_t data_len, size_t start,
                    const char *needle, size_t needle_len, size_t *out_pos) {
    for (size_t i = start; i + needle_len <= data_len; i++) {
        if (data[i] == needle[0] && memcmp(data + i, needle, needle_len) == 0) {
            if (out_pos) *out_pos = i;
            return 1;
        }
    }
    return 0;
}

static size_t matching_brace(const char *open, size_t len) {
    if (len == 0 || open[0] != '{') return 0;
    size_t depth = 0;
    int in_str = 0, esc = 0;
    for (size_t i = 0; i < len; i++) {
        if (in_str) {
            if (esc) { esc = 0; continue; }
            if (open[i] == '\\') { esc = 1; continue; }
            if (open[i] == '"') { in_str = 0; continue; }
            continue;
        }
        if (open[i] == '"') { in_str = 1; continue; }
        if (open[i] == '{') { depth++; continue; }
        if (open[i] == '}') {
            depth--;
            if (depth == 0) return i + 1;
        }
    }
    return 0;
}

static int object_range(const char *data, size_t data_len, const char *key,
                        size_t *out_start, size_t *out_len) {
    char pat[128];
    int pat_len = snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t pos;
    if (!find_str(data, data_len, 0, pat, (size_t)pat_len, &pos)) return 0;

    /* find colon */
    size_t ci = pos + (size_t)pat_len;
    while (ci < data_len && data[ci] != ':') ci++;
    if (ci >= data_len) return 0;
    ci++;

    size_t p = skip_ws(data, data_len, ci);
    if (p >= data_len || data[p] != '{') return 0;
    size_t close = matching_brace(data + p, data_len - p);
    if (close == 0) return 0;
    *out_start = p;
    *out_len = close;
    return 1;
}

static int json_number(const char *data, size_t data_len, const char *key, float *out) {
    char pat[128];
    int pat_len = snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t pos;
    if (!find_str(data, data_len, 0, pat, (size_t)pat_len, &pos)) return 0;

    size_t ci = pos + (size_t)pat_len;
    while (ci < data_len && data[ci] != ':') ci++;
    if (ci >= data_len) return 0;
    ci++;
    ci = skip_ws(data, data_len, ci);
    if (ci >= data_len) return 0;

    size_t end = ci;
    while (end < data_len && (isdigit((unsigned char)data[end]) || data[end] == '-' || data[end] == '+' || data[end] == '.'))
        end++;
    if (end == ci) return 0;

    char buf[64];
    size_t num_len = end - ci;
    if (num_len >= sizeof(buf)) return 0;
    memcpy(buf, data + ci, num_len);
    buf[num_len] = '\0';
    *out = strtof(buf, NULL);
    return 1;
}

static int json_bool(const char *data, size_t data_len, const char *key, int *out) {
    char pat[128];
    int pat_len = snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t pos;
    if (!find_str(data, data_len, 0, pat, (size_t)pat_len, &pos)) return 0;

    size_t ci = pos + (size_t)pat_len;
    while (ci < data_len && data[ci] != ':') ci++;
    if (ci >= data_len) return 0;
    ci++;
    ci = skip_ws(data, data_len, ci);
    if (ci + 4 <= data_len && memcmp(data + ci, "true", 4) == 0) { *out = 1; return 1; }
    if (ci + 5 <= data_len && memcmp(data + ci, "false", 5) == 0) { *out = 0; return 1; }
    return 0;
}

static int json_string(const char *data, size_t data_len, const char *key,
                       size_t *out_start, size_t *out_len) {
    char pat[128];
    int pat_len = snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t pos;
    if (!find_str(data, data_len, 0, pat, (size_t)pat_len, &pos)) return 0;

    size_t ci = pos + (size_t)pat_len;
    while (ci < data_len && data[ci] != ':') ci++;
    if (ci >= data_len) return 0;
    ci++;
    ci = skip_ws(data, data_len, ci);
    if (ci >= data_len || data[ci] != '"') return 0;
    ci++;
    size_t start = ci;
    while (ci < data_len && data[ci] != '"') ci++;
    if (ci >= data_len) return 0;
    *out_start = start;
    *out_len = ci - start;
    return 1;
}

static int array_contains_string(const char *data, size_t data_len, const char *key,
                                 const char *needle, size_t needle_len) {
    char pat[128];
    int pat_len = snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t pos;
    if (!find_str(data, data_len, 0, pat, (size_t)pat_len, &pos)) return 0;

    size_t ci = pos + (size_t)pat_len;
    while (ci < data_len && data[ci] != '[') ci++;
    if (ci >= data_len) return 0;
    ci++;

    while (ci < data_len) {
        ci = skip_ws(data, data_len, ci);
        if (ci >= data_len || data[ci] == ']') break;
        if (data[ci] == '"') {
            ci++;
            size_t s_start = ci;
            while (ci < data_len && data[ci] != '"') ci++;
            size_t s_len = ci - s_start;
            if (s_len == needle_len && memcmp(data + s_start, needle, needle_len) == 0)
                return 1;
            if (ci < data_len) ci++; /* skip closing quote */
        } else {
            while (ci < data_len && data[ci] != ',' && data[ci] != ']') ci++;
        }
        if (ci < data_len && data[ci] == ',') ci++;
    }
    return 0;
}

/* --- ISO 8601 date helpers --- */

static unsigned iso_hour_utc(const char *s, size_t len) {
    if (len < 14) return 0;
    unsigned h = (unsigned)(s[11] - '0') * 10 + (unsigned)(s[12] - '0');
    return h > 23 ? 23 : h;
}

static unsigned iso_minute(const char *s, size_t len) {
    if (len < 16) return 0;
    return (unsigned)(s[14] - '0') * 10 + (unsigned)(s[15] - '0');
}

static int iso_year(const char *s, size_t len) {
    if (len < 4) return 0;
    return (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
}

static unsigned iso_month(const char *s, size_t len) {
    if (len < 7) return 1;
    unsigned m = (unsigned)(s[5] - '0') * 10 + (unsigned)(s[6] - '0');
    return (m < 1 || m > 12) ? 1 : m;
}

static unsigned iso_day(const char *s, size_t len) {
    if (len < 10) return 1;
    return (unsigned)(s[8] - '0') * 10 + (unsigned)(s[9] - '0');
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

static size_t weekday_from_iso(const char *s, size_t len) {
    int64_t days = days_from_civil(iso_year(s, len), iso_month(s, len), iso_day(s, len));
    int64_t w = ((days + 3) % 7 + 7) % 7;
    return (size_t)w;
}

static int64_t iso_to_epoch_seconds(const char *s, size_t len) {
    int64_t days = days_from_civil(iso_year(s, len), iso_month(s, len), iso_day(s, len));
    return days * 86400 + (int64_t)iso_hour_utc(s, len) * 3600 + (int64_t)iso_minute(s, len) * 60;
}

static int64_t minutes_between_abs(const char *a, size_t a_len, const char *b, size_t b_len) {
    int64_t diff = iso_to_epoch_seconds(a, a_len) - iso_to_epoch_seconds(b, b_len);
    if (diff < 0) diff = -diff;
    return diff / 60;
}

int vectorizer_build(const char *body, size_t body_len, float out[VEC_DIM]) {
    memset(out, 0, VEC_DIM * sizeof(float));

    size_t t_start, t_len, c_start, c_len, m_start, m_len, tm_start, tm_len;
    if (!object_range(body, body_len, "transaction", &t_start, &t_len)) return 0;
    if (!object_range(body, body_len, "customer", &c_start, &c_len)) return 0;
    if (!object_range(body, body_len, "merchant", &m_start, &m_len)) return 0;
    if (!object_range(body, body_len, "terminal", &tm_start, &tm_len)) return 0;

    const char *tx = body + t_start; size_t txl = t_len;
    const char *cust = body + c_start; size_t custl = c_len;
    const char *merch = body + m_start; size_t merchl = m_len;
    const char *term = body + tm_start; size_t terml = tm_len;

    float amount, installments, customer_avg_amount, tx_count_24h;
    float merchant_avg_amount, km_from_home;
    if (!json_number(tx, txl, "amount", &amount)) return 0;
    if (!json_number(tx, txl, "installments", &installments)) return 0;
    if (!json_number(cust, custl, "avg_amount", &customer_avg_amount)) return 0;
    if (!json_number(cust, custl, "tx_count_24h", &tx_count_24h)) return 0;
    if (!json_number(merch, merchl, "avg_amount", &merchant_avg_amount)) return 0;
    if (!json_number(term, terml, "km_from_home", &km_from_home)) return 0;

    size_t ra_start, ra_len;
    if (!json_string(tx, txl, "requested_at", &ra_start, &ra_len)) return 0;
    const char *requested_at = tx + ra_start;
    size_t ra_len2 = ra_len;

    size_t mid_start, mid_len;
    if (!json_string(merch, merchl, "id", &mid_start, &mid_len)) return 0;
    const char *merchant_id = merch + mid_start;
    size_t mid_len2 = mid_len;

    size_t mcc_start, mcc_len;
    if (!json_string(merch, merchl, "mcc", &mcc_start, &mcc_len)) return 0;

    int is_online, card_present;
    if (!json_bool(term, terml, "is_online", &is_online)) return 0;
    if (!json_bool(term, terml, "card_present", &card_present)) return 0;

    float minutes_since_last_tx = -1.0f;
    float km_from_current = -1.0f;

    size_t lt_start, lt_len;
    if (object_range(body, body_len, "last_transaction", &lt_start, &lt_len)) {
        const char *lt = body + lt_start;
        size_t ltl = lt_len;
        size_t ts_start, ts_len;
        float km;
        if (json_string(lt, ltl, "timestamp", &ts_start, &ts_len) &&
            json_number(lt, ltl, "km_from_current", &km)) {
            const char *last_ts = lt + ts_start;
            int64_t mins = minutes_between_abs(requested_at, ra_len2, last_ts, ts_len);
            minutes_since_last_tx = clamp01((float)mins / 1440.0f);
            km_from_current = clamp01(km / 1000.0f);
        }
    }

    int known_merchant = array_contains_string(cust, custl, "known_merchants", merchant_id, mid_len2);
    int is_unknown_merchant = !known_merchant;

    /* parse MCC string to int */
    char mcc_buf[16];
    size_t mcc_num_len = mcc_len < 15 ? mcc_len : 15;
    memcpy(mcc_buf, merch + mcc_start, mcc_num_len);
    mcc_buf[mcc_num_len] = '\0';
    unsigned mcc = (unsigned)atoi(mcc_buf);

    float ratio = (customer_avg_amount > 0.0f)
        ? (amount / customer_avg_amount) / 10.0f
        : 1.0f;

    out[0]  = clamp01(amount / 10000.0f);
    out[1]  = clamp01(installments / 12.0f);
    out[2]  = clamp01(ratio);
    out[3]  = round4((float)iso_hour_utc(requested_at, ra_len2) / 23.0f);
    out[4]  = round4((float)weekday_from_iso(requested_at, ra_len2) / 6.0f);
    out[5]  = minutes_since_last_tx;
    out[6]  = km_from_current;
    out[7]  = clamp01(km_from_home / 1000.0f);
    out[8]  = clamp01(tx_count_24h / 20.0f);
    out[9]  = is_online ? 1.0f : 0.0f;
    out[10] = card_present ? 1.0f : 0.0f;
    out[11] = is_unknown_merchant ? 1.0f : 0.0f;
    out[12] = mcc_risk_f(mcc);
    out[13] = clamp01(merchant_avg_amount / 10000.0f);

    return 1;
}

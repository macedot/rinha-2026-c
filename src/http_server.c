#include "http_server.h"
#include "http_resp.h"
#include "vectorizer.h"
#include "bridge.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define REQ_BUF_SIZE 32768

static uint64_t g_time_read_total = 0;
static uint64_t g_time_vectorize_total = 0;
static uint64_t g_time_search_total = 0;
static uint64_t g_time_write_total = 0;
static uint64_t g_req_count = 0;

static inline uint64_t hr_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int write_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static size_t find_content_length(const char *headers, size_t headers_len) {
    const char *key = "Content-Length:";
    size_t key_len = 15;
    const char *p = headers;
    const char *end = headers + headers_len;

    while (p + key_len <= end) {
        const char *found = memchr(p, 'C', (size_t)(end - p));
        if (!found || found + key_len > end) return (size_t)-1;
        if (memcmp(found, key, key_len) == 0) {
            const char *s = found + key_len;
            while (s < end && (*s == ' ' || *s == '\t')) s++;
            size_t v = 0;
            while (s < end && *s >= '0' && *s <= '9') {
                v = v * 10 + (size_t)(*s - '0');
                s++;
            }
            return v;
        }
        p = found + 1;
    }
    return (size_t)-1;
}

static void handle_connection(int fd) {
    char req_buf[REQ_BUF_SIZE];
    size_t req_len = 0;

    while (1) {
        ssize_t n = read(fd, req_buf + req_len, REQ_BUF_SIZE - req_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return;
        }
        if (n == 0) return;
        req_len += (size_t)n;

        /* look for end of headers */
        char *hdr_end = memmem(req_buf, req_len, "\r\n\r\n", 4);
        if (!hdr_end) {
            if (req_len >= REQ_BUF_SIZE - 1) return;
            continue;
        }

        size_t header_len = (size_t)(hdr_end - req_buf);
        size_t body_start = header_len + 4;

        if (memcmp(req_buf, "GET /ready", 10) == 0) {
            write_all(fd, resp_ready, strlen(resp_ready));
            return;
        }

        if (memcmp(req_buf, "GET /inst", 9) == 0) {
            uint64_t inst[7];
            rinha_get_inst(inst);
            char ibuf[1024];
            int ilen = snprintf(ibuf, sizeof(ibuf),
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n"
                "=== Pipeline (avg ns/req, %llu reqs) ===\n"
                "read:       %llu\n"
                "vectorize:  %llu\n"
                "search:     %llu\n"
                "write:      %llu\n"
                "\n=== Search breakdown (avg ns/search, %llu searches) ===\n"
                "quantize:   %llu\n"
                "centroids:  %llu\n"
                "topn:       %llu\n"
                "reorder:    %llu\n"
                "fast_scan:  %llu\n"
                "full_scan:  %llu\n",
                (unsigned long long)g_req_count,
                (unsigned long long)(g_req_count ? g_time_read_total / g_req_count : 0),
                (unsigned long long)(g_req_count ? g_time_vectorize_total / g_req_count : 0),
                (unsigned long long)(g_req_count ? g_time_search_total / g_req_count : 0),
                (unsigned long long)(g_req_count ? g_time_write_total / g_req_count : 0),
                (unsigned long long)inst[6],
                (unsigned long long)(inst[6] ? inst[0] / inst[6] : 0),
                (unsigned long long)(inst[6] ? inst[1] / inst[6] : 0),
                (unsigned long long)(inst[6] ? inst[2] / inst[6] : 0),
                (unsigned long long)(inst[6] ? inst[3] / inst[6] : 0),
                (unsigned long long)(inst[6] ? inst[4] / inst[6] : 0),
                (unsigned long long)(inst[6] ? inst[5] / inst[6] : 0));
            write_all(fd, ibuf, (size_t)ilen);
            return;
        }

        if (memcmp(req_buf, "POST /fraud-score", 17) == 0) {
            size_t cl = find_content_length(req_buf, header_len);
            if (cl == (size_t)-1) {
                write_all(fd, resp_bad_req, strlen(resp_bad_req));
                return;
            }
            if (req_len < body_start + cl) {
                /* need more data */
                if (req_len + cl >= REQ_BUF_SIZE) return;
                continue;
            }

            g_req_count++;

            uint64_t t_vec = hr_nanos();
            float q[VEC_DIM];
            if (!vectorizer_build(req_buf + body_start, cl, q)) {
                write_all(fd, resp_bad_req, strlen(resp_bad_req));
                return;
            }
            g_time_vectorize_total += hr_nanos() - t_vec;

            uint64_t t_search = hr_nanos();
            int frauds = rinha_search(q);
            g_time_search_total += hr_nanos() - t_search;
            if (frauds > 5) {
                write_all(fd, resp_internal_err, strlen(resp_internal_err));
                return;
            }
            const char *resp = score_for((uint8_t)frauds);
            uint64_t t_write = hr_nanos();
            write_all(fd, resp, strlen(resp));
            g_time_write_total += hr_nanos() - t_write;
            return;
        }

        write_all(fd, resp_not_found, strlen(resp_not_found));
        return;
    }
}

static uint32_t octal_from_decimal(uint32_t mode) {
    uint32_t aa = mode / 100;
    uint32_t bb = (mode / 10) % 10;
    uint32_t cc = mode % 10;
    return (aa << 6) | (bb << 3) | cc;
}

static int create_tcp_socket(const config_t *cfg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (cfg->reuse_port)
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    if (cfg->tcp_nodelay)
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);
    if (strcmp(cfg->host, "0.0.0.0") == 0)
        addr.sin_addr.s_addr = INADDR_ANY;
    else
        inet_aton(cfg->host, &addr.sin_addr);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    if (listen(fd, 128) != 0) { close(fd); return -1; }

    fprintf(stderr, "listening TCP %s:%d\n", cfg->host, cfg->port);
    return fd;
}

static int create_uds_socket(const config_t *cfg) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    if (cfg->unlink_uds)
        unlink(cfg->uds_path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(cfg->uds_path);
    if (path_len >= sizeof(addr.sun_path)) path_len = sizeof(addr.sun_path) - 1;
    memcpy(addr.sun_path, cfg->uds_path, path_len);
    addr.sun_path[path_len] = '\0';

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    if (listen(fd, 128) != 0) { close(fd); return -1; }

    chmod(cfg->uds_path, octal_from_decimal(cfg->uds_mode));

    fprintf(stderr, "listening UDS %s mode=%u\n", cfg->uds_path, cfg->uds_mode);
    return fd;
}

int server_run(const config_t *cfg) {
    int server_fd;
    if (cfg->use_tcp)
        server_fd = create_tcp_socket(cfg);
    else
        server_fd = create_uds_socket(cfg);

    if (server_fd < 0) {
        fprintf(stderr, "failed to create socket\n");
        return -1;
    }

    while (1) {
        struct sockaddr client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, &client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "accept error: %d\n", errno);
            continue;
        }
        handle_connection(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}

#define _GNU_SOURCE
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
#include <sys/epoll.h>

#define REQ_BUF_SIZE 32768
#define MAX_EVENTS 128

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline int send_all(int fd, const char *data, size_t len) {
    ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
    return (n == (ssize_t)len) ? 0 : -1;
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

typedef struct {
    int fd;
    char req_buf[REQ_BUF_SIZE];
    size_t req_len;
    int active;
} conn_t;

static conn_t g_conns[MAX_EVENTS] __attribute__((aligned(64)));

static conn_t *conn_new(int fd) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (!g_conns[i].active) {
            g_conns[i].fd = fd;
            g_conns[i].req_len = 0;
            g_conns[i].active = 1;
            return &g_conns[i];
        }
    }
    return NULL;
}

static void conn_close(conn_t *c) {
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->active = 0;
    c->req_len = 0;
}

static int handle_request(conn_t *c) {
    char *req_buf = c->req_buf;
    size_t req_len = c->req_len;
    int fd = c->fd;

    char *hdr_end = memmem(req_buf, req_len, "\r\n\r\n", 4);
    if (!hdr_end) {
        if (req_len >= REQ_BUF_SIZE - 1) return -1;
        return 0;
    }

    size_t header_len = (size_t)(hdr_end - req_buf);
    size_t body_start = header_len + 4;

    if (memcmp(req_buf, "GET /ready", 10) == 0) {
        if (send_all(fd, resp_ready, resp_ready_len) != 0) return -1;
        c->req_len = 0;
        return 1;
    }

    if (memcmp(req_buf, "GET /inst", 9) == 0) {
        const char *msg = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\ninst disabled\n";
        send_all(fd, msg, 48);
        return -1;
    }

    if (memcmp(req_buf, "POST /fraud-score", 17) == 0) {
        size_t cl = find_content_length(req_buf, header_len);
        if (cl == (size_t)-1) {
            if (send_all(fd, resp_bad_req, resp_bad_req_len) != 0) return -1;
            c->req_len = 0;
            return 1;
        }
        if (req_len < body_start + cl) {
            if (req_len + cl >= REQ_BUF_SIZE) return -1;
            return 0;
        }

        float q[VEC_DIM];
        if (!vectorizer_build(req_buf + body_start, cl, q)) {
            if (send_all(fd, resp_bad_req, resp_bad_req_len) != 0) return -1;
            c->req_len = 0;
            return 1;
        }

        int frauds = rinha_search(q);
        if (frauds > 5) {
            if (send_all(fd, resp_internal_err, resp_internal_err_len) != 0) return -1;
            c->req_len = 0;
            return 1;
        }
        const char *resp = score_for((uint8_t)frauds);
        if (send_all(fd, resp, score_len[(uint8_t)frauds]) != 0) return -1;

        size_t consumed = body_start + cl;
        if (req_len > consumed) {
            memmove(req_buf, req_buf + consumed, req_len - consumed);
            c->req_len = req_len - consumed;
            return 2;
        }
        c->req_len = 0;
        return 1;
    }

    if (send_all(fd, resp_not_found, resp_not_found_len) != 0) return -1;
    c->req_len = 0;
    return 1;
}

static int process_conn(conn_t *c) {
    int fd = c->fd;
    char *req_buf = c->req_buf;
    size_t *req_len = &c->req_len;

    while (1) {
        ssize_t n = recv(fd, req_buf + *req_len, REQ_BUF_SIZE - *req_len, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (n == 0) return -1;
        *req_len += (size_t)n;

        int rc = handle_request(c);
        if (rc < 0) return -1;
        if (rc == 0) return 0;
        if (rc == 1) return 1;
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
    if (listen(fd, 1024) != 0) { close(fd); return -1; }

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
    if (listen(fd, 1024) != 0) { close(fd); return -1; }

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

    if (set_nonblocking(server_fd) < 0) {
        close(server_fd);
        return -1;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        close(server_fd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        close(epoll_fd);
        close(server_fd);
        return -1;
    }

    struct epoll_event events[MAX_EVENTS];

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.ptr == NULL) {
                while (1) {
                    struct sockaddr client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept4(server_fd, &client_addr, &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        fprintf(stderr, "accept error: %d\n", errno);
                        break;
                    }

                    conn_t *c = conn_new(client_fd);
                    if (!c) {
                        close(client_fd);
                        continue;
                    }

                    /* TCP_NODELAY for low-latency responses */
                    int opt = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

                    struct epoll_event cev;
                    cev.events = EPOLLIN | EPOLLET;
                    cev.data.ptr = c;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev) < 0) {
                        conn_close(c);
                        continue;
                    }
                }
            } else {
                conn_t *c = (conn_t *)events[i].data.ptr;
                int rc = process_conn(c);
                if (rc < 0) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
                    conn_close(c);
                }
            }
        }
    }

    /* Close any remaining connections */
    close(epoll_fd);
    close(server_fd);
    return 0;
}

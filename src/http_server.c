#define _GNU_SOURCE
#include "http_server.h"
#include "http_resp.h"
#include "vectorizer.h"
#include "knn.h"
#include "scm_rights.h"
#include "perf.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/poll.h>
#include <netinet/tcp.h>
#include <netinet/in.h>

#define REQ_BUF_SIZE 8192
#define MAX_EVENTS 128

static inline uint64_t perf_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline int send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                poll(&pfd, 1, 1000);
                continue;
            }
            return -1;
        }
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

static int handle_request(conn_t *c, perf_sample_t *ps) {
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

        uint64_t t_json_start = perf_now();
        float q[VEC_DIM];
        int vb = vectorizer_build(req_buf + body_start, cl, q);
        if (!vb) {
            if (send_all(fd, resp_bad_req, resp_bad_req_len) != 0) return -1;
            c->req_len = 0;
            return 1;
        }
        uint64_t t_json_end = perf_now();
        if (ps) ps->json_ns += t_json_end - t_json_start;

        uint64_t t_knn_start = perf_now();
        float fraud_score = 0.0f;
        int approved = rinha_search(q, &fraud_score);
        uint64_t t_knn_end = perf_now();
        if (ps) ps->knn_ns += t_knn_end - t_knn_start;

        uint64_t t_send_start = perf_now();
        char resp_buf[256];
        int resp_len = format_score_response(resp_buf, sizeof(resp_buf), approved, fraud_score);
        if (send_all(fd, resp_buf, (size_t)resp_len) != 0) return -1;
        uint64_t t_send_end = perf_now();
        if (ps) ps->send_ns += t_send_end - t_send_start;

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

    perf_sample_t ps = {0, 0, 0, 0, 0};
    int did_recv = 0;

    while (1) {
        if (*req_len == 0 || did_recv == 0) {
            uint64_t t_recv_start = perf_now();
            ssize_t n = recv(fd, req_buf + *req_len, REQ_BUF_SIZE - *req_len, 0);
            uint64_t t_recv_end = perf_now();
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (did_recv && ps.total_ns > 0) {
                        perf_record(&ps);
                    }
                    return 0;
                }
                if (did_recv && ps.total_ns > 0) {
                    perf_record(&ps);
                }
                return -1;
            }
            if (n == 0) {
                if (did_recv && ps.total_ns > 0) {
                    perf_record(&ps);
                }
                return -1;
            }
            *req_len += (size_t)n;
            ps.recv_ns += t_recv_end - t_recv_start;
            did_recv = 1;
        }

        uint64_t t_hdl_start = perf_now();
        int rc = handle_request(c, &ps);
        uint64_t t_hdl_end = perf_now();

        if (rc < 0) {
            if (ps.total_ns > 0) perf_record(&ps);
            return -1;
        }
        if (rc == 0) {
            if (ps.total_ns > 0) perf_record(&ps);
            return 0;
        }

        ps.total_ns += t_hdl_end - t_hdl_start;

        if (rc == 2) {
            did_recv = 0;
            continue;
        }

        perf_record(&ps);
        ps.recv_ns = 0;
        ps.json_ns = 0;
        ps.knn_ns = 0;
        ps.send_ns = 0;
        ps.total_ns = 0;
        did_recv = 0;
    }
}

static uint32_t octal_from_decimal(uint32_t mode) {
    uint32_t aa = mode / 100;
    uint32_t bb = (mode / 10) % 10;
    uint32_t cc = mode % 10;
    return (aa << 6) | (bb << 3) | cc;
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

/* Ctrl connection tracking for SCM_RIGHTS */
#define MAX_CTRL_CONNS 4
static int g_ctrl_conns[MAX_CTRL_CONNS];
static int g_ctrl_count = 0;

static int ctrl_conn_add(int fd) {
    if (g_ctrl_count >= MAX_CTRL_CONNS) return -1;
    g_ctrl_conns[g_ctrl_count] = fd;
    return g_ctrl_count++;
}

static void ctrl_conn_remove(int idx) {
    if (idx < 0 || idx >= g_ctrl_count) return;
    close(g_ctrl_conns[idx]);
    for (int i = idx; i < g_ctrl_count - 1; i++) {
        g_ctrl_conns[i] = g_ctrl_conns[i + 1];
    }
    g_ctrl_count--;
}

static int ctrl_conn_find(int fd) {
    for (int i = 0; i < g_ctrl_count; i++) {
        if (g_ctrl_conns[i] == fd) return i;
    }
    return -1;
}

int server_run(const config_t *cfg) {
    int server_fd = create_uds_socket(cfg);
    if (server_fd < 0) {
        fprintf(stderr, "failed to create socket\n");
        return -1;
    }

    if (set_nonblocking(server_fd) < 0) {
        close(server_fd);
        return -1;
    }

    /* Create .ctrl socket for SCM_RIGHTS fd passing from LB */
    int ctrl_listen_fd = ctrl_socket_create(cfg->uds_path);
    if (ctrl_listen_fd < 0) {
        fprintf(stderr, "failed to create ctrl socket\n");
        close(server_fd);
        return -1;
    }
    if (set_nonblocking(ctrl_listen_fd) < 0) {
        close(ctrl_listen_fd);
        close(server_fd);
        return -1;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        close(ctrl_listen_fd);
        close(server_fd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        close(epoll_fd);
        close(ctrl_listen_fd);
        close(server_fd);
        return -1;
    }

    ev.events = EPOLLIN;
    ev.data.fd = ctrl_listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctrl_listen_fd, &ev) < 0) {
        close(epoll_fd);
        close(ctrl_listen_fd);
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
            int fd = events[i].data.fd;

            if (fd == ctrl_listen_fd) {
                /* Accept new ctrl connections from LB */
                while (1) {
                    int conn_fd = ctrl_accept(ctrl_listen_fd);
                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }
                    int idx = ctrl_conn_add(conn_fd);
                    if (idx < 0) {
                        close(conn_fd);
                        continue;
                    }
                    struct epoll_event cev;
                    cev.events = EPOLLIN;
                    cev.data.fd = conn_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &cev) < 0) {
                        ctrl_conn_remove(idx);
                        continue;
                    }
                }
            } else if (fd == server_fd) {
                /* Main UDS socket: direct connections */
                while (1) {
                    struct sockaddr client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept4(server_fd, &client_addr, &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }

                    conn_t *c = conn_new(client_fd);
                    if (!c) {
                        close(client_fd);
                        continue;
                    }

                    struct epoll_event cev;
                    cev.events = EPOLLIN | EPOLLET;
                    cev.data.ptr = c;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev) < 0) {
                        conn_close(c);
                        continue;
                    }
                }
            } else if (ctrl_conn_find(fd) >= 0) {
                while (1) {
                    int client_fd = ctrl_recv_fd(fd);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        int idx = ctrl_conn_find(fd);
                        if (idx >= 0) {
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                            ctrl_conn_remove(idx);
                        }
                        break;
                    }

                    conn_t *c = conn_new(client_fd);
                    if (!c) {
                        close(client_fd);
                        continue;
                    }

                    if (set_nonblocking(client_fd) < 0) {
                        conn_close(c);
                        continue;
                    }

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

    close(epoll_fd);
    close(ctrl_listen_fd);
    close(server_fd);
    return 0;
}

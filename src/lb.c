/*
 * Minimal custom load balancer for Rinha 2026
 * Accepts TCP connections and forwards client sockets to backends
 * using SCM_RIGHTS over Unix domain control sockets.
 *
 * This replaces external LBs like so-no-forevis or HAProxy to achieve
 * true zero-copy fd passing without requiring security_opt.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_BACKENDS 2

static int ctrl_fds[MAX_BACKENDS];
static char ctrl_paths[MAX_BACKENDS][256];
static int num_backends = 0;

static int connect_ctrl(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    int attempts = 0;
    while (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno != ENOENT && errno != ECONNREFUSED) {
            fprintf(stderr, "[lb] connect %s failed: %s\n", path, strerror(errno));
            close(fd);
            return -1;
        }
        if (attempts++ % 25 == 0) {
            fprintf(stderr, "[lb] waiting for %s (attempt %d)...\n", path, attempts);
        }
        usleep(20000); // 20ms
    }
    return fd;
}

static int send_fd(int ctrl_fd, int client_fd) {
    char buf[1] = {0};
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &client_fd, sizeof(int));

    if (sendmsg(ctrl_fd, &msg, MSG_NOSIGNAL) <= 0) {
        perror("[lb] sendmsg SCM_RIGHTS");
        return -1;
    }
    return 0;
}

int main(void) {
    const char *port_str = getenv("LB_PORT");
    int port = port_str ? atoi(port_str) : 9999;

    const char *backends = getenv("LB_BACKENDS");
    if (!backends) {
        fprintf(stderr, "[lb] LB_BACKENDS not set (e.g. /run/sock/api1.sock.ctrl,/run/sock/api2.sock.ctrl)\n");
        return 1;
    }

    char tmp[1024];
    strncpy(tmp, backends, sizeof(tmp) - 1);
    tmp[sizeof(tmp)-1] = 0;

    char *tok = strtok(tmp, ",");
    while (tok && num_backends < MAX_BACKENDS) {
        strncpy(ctrl_paths[num_backends], tok, 255);
        ctrl_paths[num_backends][255] = 0;
        num_backends++;
        tok = strtok(NULL, ",");
    }

    if (num_backends == 0) {
        fprintf(stderr, "[lb] No backends configured\n");
        return 1;
    }

    for (int i = 0; i < num_backends; i++) {
        ctrl_fds[i] = connect_ctrl(ctrl_paths[i]);
        if (ctrl_fds[i] < 0) {
            fprintf(stderr, "[lb] Failed to connect to backend %d\n", i);
            return 1;
        }
        fprintf(stderr, "[lb] Connected to backend %d (%s)\n", i, ctrl_paths[i]);
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr, "[lb] Listening on port %d, forwarding to %d backends via SCM_RIGHTS\n", port, num_backends);

    int next = 0;
    while (1) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int client_fd = accept(listen_fd, (struct sockaddr *)&cli, &clilen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        // Best-effort TCP optimizations before handing off
        int one = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(client_fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));

        int idx = next;
        next = (next + 1) % num_backends;

        if (send_fd(ctrl_fds[idx], client_fd) < 0) {
            fprintf(stderr, "[lb] Failed to send fd to backend %d, attempting reconnect...\n", idx);
            close(ctrl_fds[idx]);
            ctrl_fds[idx] = connect_ctrl(ctrl_paths[idx]);
            if (ctrl_fds[idx] >= 0) {
                send_fd(ctrl_fds[idx], client_fd);
            }
        }

        close(client_fd); // We no longer need our copy
    }

    return 0;
}

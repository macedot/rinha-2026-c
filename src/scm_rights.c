/* SCM_RIGHTS fd passing — receive TCP sockets from SoNoForevis LB */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Create a .ctrl UDS socket for receiving FDs via SCM_RIGHTS */
int ctrl_socket_create(const char *base_path) {
    size_t blen = strlen(base_path);
    size_t clen = blen + 6; /* + ".ctrl\0" */
    char *ctrl_path = malloc(clen);
    if (!ctrl_path) return -1;
    memcpy(ctrl_path, base_path, blen);
    memcpy(ctrl_path + blen, ".ctrl", 6); /* includes '\0' */

    unlink(ctrl_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { free(ctrl_path); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(ctrl_path);
    if (plen >= sizeof(addr.sun_path)) plen = sizeof(addr.sun_path) - 1;
    memcpy(addr.sun_path, ctrl_path, plen);
    addr.sun_path[plen] = '\0';

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd); free(ctrl_path); return -1;
    }
    if (listen(fd, 1024) != 0) {
        close(fd); free(ctrl_path); return -1;
    }

    fprintf(stderr, "listening ctrl %s\n", ctrl_path);
    free(ctrl_path);
    return fd;
}

/* Accept a connection on ctrl socket and receive one FD via SCM_RIGHTS.
   Returns the received FD, or -1 on error. */
int ctrl_recv_fd(int ctrl_fd) {
    struct sockaddr_un addr;
    socklen_t addr_len = sizeof(addr);
    int conn_fd = accept4(ctrl_fd, (struct sockaddr *)&addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (conn_fd < 0) return -1;

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_u;

    char dummy[1];
    struct iovec iov = { .iov_base = dummy, .iov_len = sizeof(dummy) };
    struct msghdr msg = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_u.buf,
        .msg_controllen = sizeof(cmsg_u.buf),
    };

    memset(cmsg_u.buf, 0, sizeof(cmsg_u.buf));

    ssize_t n = recvmsg(conn_fd, &msg, 0);
    close(conn_fd);
    if (n < 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg) return -1;
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) return -1;

    int *fdptr = (int *)CMSG_DATA(cmsg);
    int received_fd = *fdptr;

    /* Set non-blocking */
    int flags = fcntl(received_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(received_fd, F_SETFL, flags | O_NONBLOCK);

    return received_fd;
}

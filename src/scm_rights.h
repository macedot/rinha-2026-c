#ifndef SCM_RIGHTS_H
#define SCM_RIGHTS_H

int ctrl_socket_create(const char *base_path);
int ctrl_recv_fd(int ctrl_fd);

#endif

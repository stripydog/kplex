/* tcp.h
 * This file is part of kplex
 * Copyright Keith Young 2013
 * For copying information see the file COPYING distributed with this software
 */

#define DEFTCPQSIZE 128

struct if_tcp {
    int fd;
    size_t qsize;
    struct if_tcp_shared *shared;
};

struct if_tcp_shared {
    time_t retry;
    socklen_t sa_len;
    struct sockaddr_storage sa;
    int donewith;
    int protocol;
    pthread_mutex_t t_mutex;
};

void cleanup_tcp(iface_t *ifa);
void write_tcp(struct iface *ifa);
ssize_t read_tcp(struct iface *ifa, char *buf);



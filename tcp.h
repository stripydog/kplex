/* tcp.h
 * This file is part of kplex
 * Copyright Keith Young 2013-2015
 * For copying information see the file COPYING distributed with this software
 */

#define DEFSNDTIMEO 30
#define DEFSNDBUF 1024
#define DEFKEEPIDLE 30
#define DEFKEEPINTVL 10
#define DEFKEEPCNT 3
#define MAXPREAMBLE 1024

struct tcp_preamble {
    unsigned char * string;
    size_t len;
};

struct if_tcp {
    int fd;
    struct if_tcp_shared *shared;
};

struct if_tcp_shared {
    char *host;
    char *port;
    time_t retry;
    socklen_t sa_len;
    struct sockaddr_storage sa;
    int donewith;
    int protocol;
    int keepalive;
    unsigned keepidle;
    unsigned keepintvl;
    unsigned keepcnt;
    unsigned sndbuf;
    int nodelay;
    int critical;
    int fixing;
    pthread_mutex_t t_mutex;
    pthread_cond_t fv;
    struct timeval tv;
    struct tcp_preamble *preamble;
};

void cleanup_tcp(iface_t *ifa);
void write_tcp(struct iface *ifa);
ssize_t read_tcp(void *ifa, char *buf);



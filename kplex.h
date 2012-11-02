/* kplex.h
 * This file is part of kplex
 * Copyright Keith Young 2012
 * For copying information see the file COPYING distributed with this software
 */
#include <sys/types.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <termios.h>
#include <errno.h>
#include <string.h>

#define DEFQUEUESZ 128
#define SERIALQUESIZE 128
#define BCASTQUEUESIZE 64
#define TCPQUEUESIZE 128

#define SENMAX 96
#define DEFBCASTPORT 10110
#define DEFTCPPORT "10110"

enum itype {
	FILEIO,
	SERIAL,
	BCAST,
	TCP,
    PTY,
	MCAST,
	ST
};

enum iotype {
	IN,
	OUT,
    BOTH
};

struct senblk {
	size_t len;
    struct iface *src;
	struct senblk *next;
	char data[SENMAX];
};
typedef struct senblk senblk_t;

struct ioqueue {
	pthread_mutex_t	q_mutex;
	pthread_cond_t	freshmeat;
	int active;
	senblk_t *free;
	senblk_t *qhead;
	senblk_t *qtail;
	senblk_t *base;
};
typedef struct ioqueue ioqueue_t;

struct iolists {
	pthread_mutex_t	io_mutex;
	pthread_mutex_t dead_mutex;
	pthread_cond_t	dead_cond;
	pthread_cond_t	init_cond;
	int	uninitialized;
	struct iface *outputs;
	struct iface *inputs;
	struct iface *dead;
};

struct iface {
	pthread_t tid;
    struct iface *pair;
	enum iotype direction;
	enum itype type;
	void *info;
	ioqueue_t *q;
	struct iface *next;
	struct iolists *lists;
	void (*cleanup)(struct iface *);
	struct iface *(*read)(struct iface *);
	struct iface *(*write)(struct iface *);
};

typedef struct iface iface_t;

struct engine_info {
	struct iolists *lists;
	ioqueue_t	*q;
};

iface_t *init_file(char *, iface_t *);
iface_t *init_serial(char *, iface_t *);
iface_t *init_bcast(char *, iface_t *);
iface_t *init_tcp(char *, iface_t *);
iface_t *init_pty(char *, iface_t *);
iface_t *init_seatalk(char *, iface_t *);

void *ifdup_serial(void *);
void *ifdup_file(void *);
void *ifdup_bcast(void *);
void *ifdup_tcp(void *);

ioqueue_t *init_q(size_t);

senblk_t *next_senblk(ioqueue_t *q);
void push_senblk(senblk_t *, ioqueue_t *);
void senblk_free(senblk_t *, ioqueue_t *);
int link_interface(iface_t *);
int unlink_interface(iface_t *);
void start_interface(void *ptr);
iface_t *ifdup(iface_t *);

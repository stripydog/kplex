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

#define KPLEXHOMECONF ".kplex.conf"
#define KPLEXGLOBALCONF "/etc/kplex.conf"
#define DEFQUEUESZ 128
#define SERIALQUESIZE 128
#define BCASTQUEUESIZE 64
#define TCPQUEUESIZE 128


#define SENMAX 96
#define DEFBCASTPORT 10110
#define DEFTCPPORT "10110"
#define IDMINORBITS 16
#define IDMINORMASK ((1<<IDMINORBITS)-1)
#define MAXINTERFACES 65535

#define BUFSIZE 1024

enum itype {
    GLOBAL,
	FILEIO,
	SERIAL,
	BCAST,
	TCP,
    PTY,
	MCAST,
	ST,
    END
};

enum filtertype {
    FILTER,
    FAILOVER
};

enum iotype {
    NONE,
	IN,
	OUT,
    BOTH
};

struct senblk {
	size_t len;
    unsigned int src;
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
	struct iface *initialized;
	struct iface *outputs;
	struct iface *inputs;
	struct iface *dead;
    struct iface *engine;
};

struct kopts {
    char *var;
    char *val;
    struct kopts *next;
};

struct srclist {
    union {
    unsigned int  id;
    char *        name;
    } src;
    time_t    failtime;
    time_t lasttime;
    struct srclist *next;
};

struct sfilter_rule {
    union { 
        char type;
        struct srclist *source;
    } info;
    char match[5];
    struct sfilter_rule *next;
};

typedef struct sfilter_rule sf_rule_t;

struct sfilter {
    enum filtertype type;
    pthread_mutex_t lock;
    unsigned int refcount;
    sf_rule_t *rules;
};

typedef struct sfilter sfilter_t;

struct iface {
	pthread_t tid;
    unsigned int id;
    char *name;
    struct iface *pair;
	enum iotype direction;
	enum itype type;
	void *info;
    struct kopts *options;
	ioqueue_t *q;
	struct iface *next;
	struct iolists *lists;
    int checksum;
    sfilter_t *ifilter;
    sfilter_t *ofilter;
	void (*cleanup)(struct iface *);
	struct iface *(*read)(struct iface *);
	struct iface *(*write)(struct iface *);
};

typedef struct iface iface_t;

struct iftypedef {
    enum itype  index;
    char *name;
    iface_t *(*init_func)(iface_t *);
    void *(*ifdup_func)(void *);
};

iface_t *init_file( iface_t *);
iface_t *init_serial(iface_t *);
iface_t *init_bcast(iface_t *);
iface_t *init_tcp(iface_t *);
iface_t *init_pty(iface_t *);
iface_t *init_seatalk(iface_t *);

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
int link_to_initialized(iface_t *);
void start_interface(void *ptr);
iface_t *ifdup(iface_t *);
void iface_thread_exit(int);
int next_config(FILE *,unsigned int *,char **,char **);

iface_t *parse_file(char *);
iface_t *parse_arg(char *);
iface_t *get_default_global(void);
void free_options(struct kopts *);
void free_filter(sfilter_t *);
void logerr(int,char *,...);
void logterm(int,char *,...);
void logtermall(int,char *,...);
void logwarn(char *,...);
void initlog(int);
sfilter_t *addfilter(sfilter_t *);
unsigned int namelookup(char *);
int insertname(char *, unsigned int);
void freenames(void);

extern struct iftypedef iftypes[];

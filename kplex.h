/* kplex.h
 * This file is part of kplex
 * Copyright Keith Young 2012-2020
 * For copying information see the file COPYING distributed with this software
 */
#ifndef KPLEX_H
#define KPLEX_H
#include <sys/types.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <termios.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <AvailabilityMacros.h>
#define KPLEXHOMECONFOSX "Library/Preferences/kplex.conf"
#define KPLEXOLDHOMECONFOSX "Library/Preferences/kplex.ini"
#endif

#define KPLEXHOMECONF ".kplex.conf"

#ifndef ACCESSPERMS
#define ACCESSPERMS (S_IRWXU|S_IRWXG|S_IRWXO)
#endif

#define KPLEXGLOBALCONF "/etc/kplex.conf"

#define DEFSRCNAME "kplex"

#define DEFQSIZE 16

#define SENMAX 80
/* This should be +2. Will be reduced in a future release */
#define SENBUFSZ (SENMAX + 4)
#define TAGMAX 80
#define DEFPORT 10110
#define DEFPORTSTRING "10110"
#define IDMINORBITS 16
#define IDMINORMASK ((1<<IDMINORBITS)-1)
#define MAXINTERFACES 65535

#define BUFSIZE 1024

/* Iinterface flags */
#define F_PERSIST 1
#define F_IPERSIST 2
#define F_LOOPBACK 4
#define F_OPTIONAL 8
#define F_NOCR 16

#define flag_test(a,b) (a->flags & b)
#define flag_set(a,b) (a->flags |= b)
#define flag_clear(a,b) (a->flags &= ~b)

/* TAG flags */
#define TAG_TS 1
#define TAG_MS 2
#define TAG_SRC 4
#define TAG_ISRC 8

#ifdef NOCATGETS
#define catgets(cat,x,y,msg) msg
#else
/* i8n catalogue */
#include <nl_types.h>
extern nl_catd cat;
#endif

extern int debuglevel;
#define DEBUG(level,...) if (debuglevel >= level) logdebug(0, __VA_ARGS__)
#define DEBUG2(level,...) if (debuglevel >= level) logdebug(errno, __VA_ARGS__)

/* parsing states */
enum sstate {
    SEN_NODATA,
    SEN_SENPROC,
    SEN_TAGPROC,
    SEN_TAGSEEN,
    SEN_CR,
    SEN_ERR
};

enum itype {
    GLOBAL,
    FILEIO,
    SERIAL,
    PTY,
    TCP,
    UDP,
    GOFREE,
    BCAST,
    MCAST,
    ST,
    END
};

enum filtertype {
    FILTER,
    FAILOVER
};

enum ruletype {
    DENY,
    ACCEPT,
    LIMIT
};

enum iotype {
    NONE,
    IN,
    OUT,
    BOTH
};

enum udptype {
    UDP_UNSPEC,
    UDP_UNICAST,
    UDP_BROADCAST,
    UDP_MULTICAST
};

enum evttype {
    EVT_HB
};

enum cksm {
    CKSM_NO = 0,
    CKSM_UNDEF,
    CKSM_STRICT,
    CKSM_LOOSE,
    CKSM_ADD,
    CKSM_ADDONLY
};

struct senblk {
    size_t len;
    unsigned long src;
    struct senblk *next;
    char data[SENBUFSZ];
};
typedef struct senblk senblk_t;

typedef struct iface iface_t;

struct ioqueue {
    iface_t *owner;
    pthread_mutex_t    q_mutex;
    pthread_cond_t    freshmeat;
    int active;
    int drops;
    senblk_t *free;
    senblk_t *qhead;
    senblk_t *qtail;
    senblk_t *base;
};
typedef struct ioqueue ioqueue_t;

struct iolists {
    pthread_mutex_t io_mutex;
    pthread_mutex_t init_mutex;
    pthread_cond_t  dead_cond;
    pthread_cond_t  init_cond;
    struct iface *initialized;
    struct iface *outputs;
    struct iface *inputs;
    struct iface *dead;
    struct iface *engine;
    struct evtmgr  *eventmgr;
};

struct evtmgr;

typedef struct evt {
    enum evttype type;
    void *info;
    int (*handle)(void *);
    time_t period;
    struct timespec when;
    struct evt *next;
    struct evtmgr *mgr;
} evt_t;

struct evtmgr {
    pthread_t tid;
    pthread_mutex_t evt_mutex;
    pthread_cond_t evt_cond;
    int active;
    evt_t *events;
};

struct kopts {
    char *var;
    char *val;
    struct kopts *next;
};

struct srclist {
    union {
    unsigned int  id;
    char *name;
    } src;
    time_t failtime;
    time_t lasttime;
    struct srclist *next;
};

struct ratelimit {
    time_t timeout;
    struct timeval last;
};

struct sfilter_rule {
    enum ruletype type;
    union { 
        struct ratelimit *limit;
        struct srclist *source;
    } info;
    union {
        unsigned int id;
        char *name;
    } src;
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
    unsigned long id;
    char *name;
    time_t heartbeat;
    struct iface *pair;
    enum iotype direction;
    enum itype type;
    void *info;
    struct kopts *options;
    ioqueue_t *q;
    struct iface *next;
    struct iolists *lists;
    int checksum;
    int strict;
    unsigned int flags;
    unsigned int tagflags;
    sfilter_t *ifilter;
    sfilter_t *ofilter;
    void (*cleanup)(struct iface *);
    void (*read)(struct iface *);
    void (*write)(struct iface *);
    ssize_t (*readbuf)(struct iface *,char *buf);
};

struct iftypedef {
    enum itype  index;
    char *name;
    iface_t *(*init_func)(iface_t *);
    void *(*ifdup_func)(void *);
};

/* Flags for engine status */

#define K_BACKGROUND 0x1
#define K_NOSTDIN 0x2
#define K_NOSTDOUT 0x4
#define K_NOSTDERR 0x8

struct if_engine {
    unsigned flags;
    int logto;
};

int mysleep(time_t);

iface_t *init_file( iface_t *);
iface_t *init_serial(iface_t *);
iface_t *init_pty(iface_t *);
iface_t *init_udp(iface_t *);
iface_t *init_tcp(iface_t *);
iface_t *init_gofree(iface_t *);
iface_t *init_bcast(iface_t *);
iface_t *init_mcast(iface_t *);
iface_t *init_seatalk(iface_t *);

void *ifdup_serial(void *);
void *ifdup_file(void *);
void *ifdup_udp(void *);
void *ifdup_tcp(void *);
void *ifdup_gofree(void *);
void *ifdup_bcast(void *);
void *ifdup_mcast(void *);
void *ifdup_seatalk(void *);

int init_q(iface_t *, size_t);

senblk_t *next_senblk(ioqueue_t *);
senblk_t *last_senblk(ioqueue_t *);
void push_senblk(senblk_t *, ioqueue_t *);
void senblk_free(senblk_t *, ioqueue_t *);
void flush_queue(ioqueue_t *);
int link_interface(iface_t *);
int unlink_interface(iface_t *);
int link_to_initialized(iface_t *);
void start_interface(void *);
iface_t *ifdup(iface_t *);
void iface_thread_exit(int);
int next_config(FILE *,unsigned int *,char **,char **);

int calcsum(const char *, size_t);
iface_t *parse_file(char *);
iface_t *parse_arg(char *);
iface_t *get_default_global(void);
void free_options(struct kopts *);
void free_filter(sfilter_t *);
void logerr(int,char *,...);
void logterm(int,char *,...);
void logtermall(int,char *,...);
void logdebug(int, char *,...);
void logwarn(char *,...);
void loginfo(char *,...);
void initlog(int);
int add_event(enum evttype,void *,time_t);
void stop_heartbeat(iface_t *);
struct evtmgr *init_evtmgr();
void proc_events(void *);
sfilter_t *addfilter(sfilter_t *);
int senfilter(senblk_t *,sfilter_t *);
int checkcksum(senblk_t *, enum cksm);
unsigned long namelookup(char *);
char *idlookup(unsigned long);
int insertname(char *, unsigned long);
void freenames(void);
int cmdlineopt(struct kopts **, char *);
void do_read(iface_t *);
size_t gettag(iface_t *, char *, senblk_t *);
int add_checksum(senblk_t *);

extern struct iftypedef iftypes[];

#endif /* KPLEX_H */

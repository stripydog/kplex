/* tcp.c
 * This file is part of kplex
 * Copyright Keith Young 2012-2014
 * For copying information see the file COPYING distributed with this software
 */

#include "kplex.h"
#include "tcp.h"
#include <netdb.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/uio.h>

/*
 * Duplicate struct if_tcp
 * Args: if_tcp to be duplicated
 * Returns: pointer to new if_tcp
 */
void *ifdup_tcp(void *ift)
{
    struct if_tcp *oldif,*newif;

    if ((newif = (struct if_tcp *) malloc(sizeof(struct if_tcp)))
        == (struct if_tcp *) NULL)
        return(NULL);
    oldif = (struct if_tcp *) ift;

    memcpy((void *)newif,(void *)oldif,sizeof(struct if_tcp));

    if (newif->shared)
        newif->shared->donewith=0;

    return ((void *) newif);
}

void cleanup_tcp(iface_t *ifa)
{
    struct if_tcp *ift = (struct if_tcp *)ifa->info;
    if (ift->shared) {
        /* io_mutex is held in cleanup routines to serialize this */
        /* unlock shared mutex in case we were interupted whilst holding it */
        (void)  pthread_mutex_unlock(&ift->shared->t_mutex);
        if (!(ift->shared->donewith)) {
            ift->shared->donewith++;
            return;
        }
        pthread_mutex_destroy(&ift->shared->t_mutex);
        free(ift->shared);
    }
    close(ift->fd);
}

int establish_keepalive(struct if_tcp *ift)
{
    int on=1;
    int err=0;

    if (ift->shared->keepalive)  {
        if (setsockopt(ift->fd,SOL_SOCKET,SO_KEEPALIVE,&on,sizeof(on)) < 0) {
            logerr(errno,"Could not enable keepalives on tcp socket");
            return(-1);
        }

        if (ift->shared->keepidle)
#ifdef __APPLE__
            if (setsockopt(ift->fd,IPPROTO_TCP,TCP_KEEPALIVE,
                    &ift->shared->keepidle, sizeof(unsigned)) < 0) {
                logerr(errno,"Could not set tcp keepidle");
                err=-1;
            }
#else
            if (setsockopt(ift->fd,IPPROTO_TCP,TCP_KEEPIDLE,
                    &ift->shared->keepidle,sizeof(unsigned)) < 0) {
                logerr(errno,"Could not set tcp keepidle");
                err=-1;
            }
#endif
#if !defined (__APPLE__) || __MAC_OS_X_VERSION_MAX_ALLOWED >= 1090
        if (ift->shared->keepintvl)
            if (setsockopt(ift->fd,IPPROTO_TCP,TCP_KEEPINTVL,
                    &ift->shared->keepintvl,sizeof(unsigned)) < 0) {
                logerr(errno,"Could not set tcp keepintvl");
                err=-1;
            }

        if (ift->shared->keepcnt)
            if (setsockopt(ift->fd,IPPROTO_TCP,TCP_KEEPCNT,
                    &ift->shared->keepcnt,sizeof(unsigned)) < 0) {
                logerr(errno,"Could not set tcp keepcnt");
                err=-1;
            }
#endif
    }
    if (ift->shared->tv.tv_sec) {
        if ((setsockopt(ift->fd,SOL_SOCKET,SO_SNDTIMEO,&ift->shared->tv,
                    sizeof(ift->shared->tv)) < 0) || (setsockopt(ift->fd,
                    SOL_SOCKET,SO_SNDBUF,&ift->shared->sndbuf,
                    sizeof(ift->shared->sndbuf)) <0))
            logerr(errno,"Could not set tcp send timeout");
    }
    return(err);
}

int reconnect(iface_t *ifa, int err)
{
    struct if_tcp *ift = (struct if_tcp *) ifa->info;
    int retval=0;
    int on=1;

    pthread_mutex_lock(&ift->shared->t_mutex);

    /* If the write timed out, we don't need to sleep before retrying */
    switch (err) {
    case EAGAIN:
        break;
    default:
        mysleep(ift->shared->retry);
    }

    /* Loop retrying until we reconnect or encounter an error which doesn't
     * look like one we are going to recover from */
    for(retval=0;retval == 0;) {
        /* For most re-connections, closing and re-opening the socket is 
         * unnecessary, but we do it here for consistency */
        close(ift->fd);
        if ((ift->fd=socket(ift->shared->sa.ss_family,SOCK_STREAM,
                ift->shared->protocol)) < 0) {
            logerr(errno,"Failed to create socket");
            retval=-1;
            break;
        }
        if (connect(ift->fd,(const struct sockaddr *)
                &ift->shared->sa,ift->shared->sa_len) == 0) {
            break;
        }

        switch (errno) {
        case ECONNREFUSED:
        case EHOSTUNREACH:
        case ENETDOWN:
        case ENETUNREACH:
            mysleep(ift->shared->retry);
        case ETIMEDOUT:
            continue;
        default:
            retval = -1;
        }
    }

    if (retval == 0) {
        if (setsockopt(ift->fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on)) < 0)
            logerr(errno,"Could not disable Nagle on new tcp connection");
        (void) establish_keepalive(ift);
    }

    flush_queue(ifa->q);

    pthread_mutex_unlock(&ift->shared->t_mutex);
    return(retval);
}

ssize_t reread(iface_t *ifa, char *buf, int bsize)
{
    struct if_tcp *ift = (struct if_tcp *) ifa->info;
    ssize_t nread;
    int fflags;
    int on=1;

    /* Make socket non-blocking so we don't hold the mutex longer
     * than necessary */
    if ((fflags=fcntl(ift->fd,F_GETFL)) < 0) {
        logerr(errno,"Failed to get socket flags");
        return(-1);
    }

    if (fcntl(ift->fd,F_SETFL,fflags | O_NONBLOCK) < 0) {
        logerr(errno,"Failed to make tcp socket non-blocking");
        return(-1);
    }

    pthread_mutex_lock(&ift->shared->t_mutex);
    if ((nread=read(ift->fd,buf,bsize)) <= 0) {
        if (nread == 0 || (errno != EWOULDBLOCK && errno != EAGAIN)) {
            /* An actual error as opposed to success but would block */
            for (nread=-1;nread!=0;) {
                close(ift->fd);
                if ((ift->fd=socket(ift->shared->sa.ss_family,SOCK_STREAM,
                        ift->shared->protocol)) < 0) {
                    logerr(errno,"Failed to create socket");
                    nread=-1;
                    break;
                }

                mysleep(ift->shared->retry);
                nread=connect(ift->fd,(const struct sockaddr *)&ift->shared->sa,
                        ift->shared->sa_len);
            }
        } else {
            nread=0;
        }
    }

    if (fcntl(ift->fd,F_SETFL,fflags) < 0) {
        logerr(errno,"Failed to make tcp socket blocking");
        nread=-1;
    }
    if (ifa->direction != IN)
        if (setsockopt(ift->fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on)) < 0)
            logerr(errno,"Could not disable Nagle on new tcp connection");

    (void) establish_keepalive(ift);
    pthread_mutex_unlock(&ift->shared->t_mutex);
    return(nread);
}

ssize_t read_tcp(struct iface *ifa, char *buf)
{
    struct if_tcp *ift = (struct if_tcp *) ifa->info;
    ssize_t nread;

    /* Man pages lie!  On FreeBSD, Linux and OS X, SIGPIPE is NOT delivered
     * to a process reading from socket which times out due to unreplied to
     * keepalives.  Instead the read exits with ETIMEDOUT
     */
    for(;;) {
        if ((nread=read(ift->fd,buf,BUFSIZ)) <=0) {
            if (!flag_test(ifa,F_PERSIST))
                break;
            if ((nread=reread(ifa,buf,BUFSIZ)) < 0) {
                logerr(errno,"failed to reconnect tcp connection");
                break;
            }
        } else {
            break;
        }
    }
    return nread;
}

void write_tcp(struct iface *ifa)
{
    struct if_tcp *ift = (struct if_tcp *) ifa->info;
    senblk_t *sptr;
    int status,err;
    int data=0;
    int cnt=1;
    struct iovec iov[2];

    if (ifa->tagflags) {
        if ((iov[0].iov_base=malloc(TAGMAX)) == NULL) {
                logerr(errno,"Disabing tag output on interface id %u (%s)",
                        ifa->id,(ifa->name)?ifa->name:"unlabelled");
                ifa->tagflags=0;
        } else {
            cnt=2;
            data=1;
        }
    }

    for(;;) {

        if ((sptr = next_senblk(ifa->q)) == NULL)
            break;

        if (senfilter(sptr,ifa->ofilter)) {
            senblk_free(sptr,ifa->q);
            continue;
        }

        if (ifa->tagflags)
            if ((iov[0].iov_len = gettag(ifa,iov[0].iov_base)) == 0) {
                logerr(errno,"Disabing tag output on interface id %u (%s)",
                        ifa->id,(ifa->name)?ifa->name:"unlabelled");
                ifa->tagflags=0;
                cnt=1;
                data=0;
                free(iov[0].iov_base);
            }

        /* SIGPIPE is blocked here so we can avoid using the (non-portable)
         * MSG_NOSIGNAL
         */
        iov[data].iov_base=sptr->data;
        iov[data].iov_len=sptr->len;

        if (writev(ift->fd,iov,cnt) <0) {
            err=errno;
            if (!flag_test(ifa,F_PERSIST))
                break;
            senblk_free(sptr,ifa->q);
            if ((status=reconnect(ifa,err)) == 0)
                continue;
            else {
                if (status < 0)
                    logerr(errno,"failed to reconnect tcp connection");
                break;
            }
        }
        senblk_free(sptr,ifa->q);
    }

    iface_thread_exit(errno);
}

iface_t *new_tcp_conn(int fd, iface_t *ifa)
{
    iface_t *newifa;
    struct if_tcp *oldift=(struct if_tcp *) ifa->info;
    struct if_tcp *newift=NULL;
    pthread_t tid;
    int on=1;
    sigset_t set,saved;

    if ((newifa = malloc(sizeof(iface_t))) == NULL)
        return(NULL);

    memset(newifa,0,sizeof(iface_t));
    if (((newift = (struct if_tcp *) malloc(sizeof(struct if_tcp))) == NULL) ||
            ((ifa->direction != IN) &&
            ((newifa->q=init_q(oldift->qsize)) == NULL))) {
        if (newifa && newifa->q)
            free(newifa->q);
        if (newift)
            free(newift);
        free(newifa);
        return(NULL);
    }
    newift->fd=fd;
    newift->shared=NULL;
    newifa->id=ifa->id+(fd&IDMINORMASK);
    newifa->direction=ifa->direction;
    newifa->type=TCP;
    newifa->name=NULL;
    newifa->info=newift;
    newifa->cleanup=cleanup_tcp;
    newifa->write=write_tcp;
    newifa->read=do_read;
    newifa->tagflags=ifa->tagflags;
    newifa->readbuf=read_tcp;
    newifa->lists=ifa->lists;
    newifa->ifilter=addfilter(ifa->ifilter);
    newifa->ofilter=addfilter(ifa->ofilter);
    newifa->checksum=ifa->checksum;
    if (ifa->direction == IN)
        newifa->q=ifa->lists->engine->q;
    else {
        if (setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on)) < 0)
            logerr(errno,"Could not disable Nagle on new tcp connection");

        if (ifa->direction == BOTH) {
            if ((newifa->next=ifdup(newifa)) == NULL) {
                logwarn("Interface duplication failed");
                free(newifa->q);
                free(newift);
                free(newifa);
                return(NULL);
            }
            newifa->direction=OUT;
            newifa->pair->direction=IN;
            newifa->pair->q=ifa->lists->engine->q;
            sigemptyset(&set);
            sigaddset(&set, SIGUSR1);
            pthread_sigmask(SIG_BLOCK, &set, &saved);
            link_to_initialized(newifa->pair);
            pthread_create(&tid,NULL,(void *)start_interface,(void *) newifa->pair);
            pthread_sigmask(SIG_SETMASK,&saved,NULL);
        }
    }
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, &saved);
    link_to_initialized(newifa);
    pthread_create(&tid,NULL,(void *)start_interface,(void *) newifa);
    pthread_sigmask(SIG_SETMASK,&saved,NULL);
    return(newifa);
}

void tcp_server(iface_t *ifa)
{
    struct if_tcp *ift=(struct if_tcp *)ifa->info;
    int afd;


    if (listen(ift->fd,5) == 0) {
        while(ifa->direction != NONE) {
         if ((afd = accept(ift->fd,NULL,NULL)) < 0)
             break;
    
         if (new_tcp_conn(afd,ifa) == NULL)
             close(afd);
        }
    }
    iface_thread_exit(errno);
}

iface_t *init_tcp(iface_t *ifa)
{
    struct if_tcp *ift;
    char *host,*port,*eptr=NULL;
    struct addrinfo hints,*aptr,*abase;
    struct servent *svent;
    int err;
    int on=1,off=0;
    char *conntype = "c";
    unsigned char *ptr;
    int i;
    struct kopts *opt;
    long retry=5;
    int keepalive=-1;
    unsigned keepidle=0;
    unsigned keepintvl=0;
    unsigned keepcnt=0;
    unsigned sndbuf=DEFSNDBUF;
    long timeout=-1;

    host=port=NULL;

    if ((ift = malloc(sizeof(struct if_tcp))) == NULL) {
        logerr(errno,"Could not allocate memory");
        return(NULL);
    }

    ift->qsize=DEFTCPQSIZE;
    ift->shared=NULL;

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"address"))
            host=opt->val;
        else if (!strcasecmp(opt->var,"mode")) {
            if (strcasecmp(opt->val,"client") && strcasecmp(opt->val,"server")){
                logerr(0,"Unknown tcp mode %s (must be \'client\' or \'server\')",opt->val);
                return(NULL);
            }
            conntype=opt->val;
        } else if (!strcasecmp(opt->var,"port")) {
            port=opt->val;
        } else if (!strcasecmp(opt->var,"retry")) {
            if (!flag_test(ifa,F_PERSIST)) {
                logerr(0,"retry valid only valid with persist option");
                return(NULL);
            }
            if ((retry=strtol(opt->val,&eptr,0)) == 0 && errno) {
                logerr(0,"retry value %s out of range",opt->val);
                return(NULL);
            }
            if (*eptr != '\0') {
                logerr(0,"Invalid retry value %s",opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"qsize")) {
            if (!(ift->qsize=atoi(opt->val))) {
                logerr(0,"Invalid queue size specified: %s",opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"keepalive")) {
            if (!flag_test(ifa,F_PERSIST)) {
                logerr(0,"keepalive valid only valid with persist option");
                return(NULL);
            }
            if (!strcasecmp(opt->val,"yes"))
                keepalive=1;
            else if (!strcasecmp(opt->val,"no"))
                keepalive=0;
            else {
                logerr(0,"keepalive must be \"yes\" or \"no\"");
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"keepcnt")) {
            if ((keepcnt=atoi(opt->val)) <= 0) {
                logerr(0,"Invalid keepcnt value specified: %s",opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"keepintvl")) {
            if ((keepintvl=atoi(opt->val)) <= 0) {
                logerr(0,"Invalid keepintvl value specified: %s",opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"keepidle")) {
            if ((keepidle=atoi(opt->val)) <= 0) {
                logerr(0,"Invalid keepidle value specified: %s",opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"timeout")) {
            if (!flag_test(ifa,F_PERSIST)) {
                logerr(0,"timeout valid only valid with persist option");
                return(NULL);
            }
            if (ifa->direction == IN) {
                logerr(0,"Timout option is for sending tcp data only (not receiving)");
                return(NULL);
            }
            if ((timeout=atoi(opt->val)) <= 0) {
                logerr(0,"Invalid timeout value specified: %s",opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"sndbuf")) {
            if (!flag_test(ifa,F_PERSIST)) {
                logerr(0,"sndbuf valid only valid with persist option");
                return(NULL);
            }
            if (ifa->direction == IN) {
                logerr(0,"sndbuf option is for sending tcp data only (not receiving)");
                return(NULL);
            }
            if ((sndbuf=atoi(opt->val)) <= 0) {
                logerr(0,"Invalid sndbuf size value specified: %s",opt->val);
                return(NULL);
            }
        } else  {
            logerr(0,"unknown interface option %s\n",opt->var);
            return(NULL);
        }
    }

    if (flag_test(ifa,F_PERSIST)){
        if (keepalive == -1) {
            keepalive=1;
            if (!keepidle)
                keepidle=DEFKEEPIDLE;
#if !defined (__APPLE__) || __MAC_OS_X_VERSION_MAX_ALLOWED >= 1090
            if (!keepintvl)
                keepintvl=DEFKEEPINTVL;
            if (!keepcnt)
                keepcnt=DEFKEEPCNT;
#endif
        }
        if (timeout == -1)
            timeout=DEFSNDTIMEO;
    }

    if (*conntype == 'c') {
        if (!host) {
            logerr(0,"Must specify address for tcp client mode\n");
            return(NULL);
        }
    } else if (flag_test(ifa,F_PERSIST)) {
        logerr(0,"persist option not valid for tcp servers");
        return(NULL);
    }

    if (!port) {
        if ((svent=getservbyname("nmea-0183","tcp")) != NULL)
            port=svent->s_name;
        else
            port = DEFPORTSTRING;
    }

    memset((void *)&hints,0,sizeof(hints));

    hints.ai_flags=(*conntype == 's')?AI_PASSIVE:0;
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_STREAM;

    if ((err=getaddrinfo(host,port,&hints,&abase))) {
        logerr(0,"Lookup failed for host %s/service %s: %s",host,port,gai_strerror(err));
        return(NULL);
    }

    aptr=abase;

    do {
        if ((ift->fd=socket(aptr->ai_family,aptr->ai_socktype,aptr->ai_protocol)) < 0)
            continue;
        if (*conntype == 'c') {
            if (connect(ift->fd,aptr->ai_addr,aptr->ai_addrlen) == 0)
                break;
        } else {
            setsockopt(ift->fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));
            if (aptr->ai_family == AF_INET6) {
                for (ptr=((struct sockaddr_in6 *)aptr->ai_addr)->sin6_addr.s6_addr,i=0;i<16;i++,ptr++)
                    if (*ptr)
                        break;
                if (i == sizeof(struct in6_addr)) {
                    if (setsockopt(ift->fd,IPPROTO_IPV6,IPV6_V6ONLY,
                            (void *)&off,sizeof(off)) <0) {
                        logerr(errno,"Failed to set ipv6 mapped ipv4 addresses on socket");
                    }
                }
            }
            if (bind(ift->fd,aptr->ai_addr,aptr->ai_addrlen) == 0)
                break;
            err=errno;
        }
        close(ift->fd);
     } while ((aptr = aptr->ai_next));

    if (aptr == NULL) {
        logerr(err,"Failed to open tcp %s for %s/%s",(*conntype == 's')?"server":"connection",host,port);
        return(NULL);
    }

    if (flag_test(ifa,F_PERSIST)) {
        if ((ift->shared = malloc(sizeof(struct if_tcp_shared))) == NULL) {
            logerr(errno,"Could not allocate memory");
            free(ift);
            return(NULL);
        }

        if (pthread_mutex_init(&ift->shared->t_mutex,NULL) != 0) {
            logerr(errno,"tcp mutex initialisation failed");
            return(NULL);
        }

        ift->shared->retry=retry;
        if (ift->shared->retry != retry) {
            logerr(0,"retry value out of range");
            return(NULL);
        }
        ift->shared->sa_len=aptr->ai_addrlen;
        (void) memcpy(&ift->shared->sa,aptr->ai_addr,aptr->ai_addrlen);
        ift->shared->donewith=1;
        ift->shared->protocol=aptr->ai_protocol;
        ift->shared->keepalive=keepalive;
        ift->shared->keepidle=keepidle;
        ift->shared->keepintvl=keepintvl;
        ift->shared->keepcnt=keepcnt;
        ift->shared->sndbuf=sndbuf;
        ift->shared->tv.tv_sec=timeout;
        ift->shared->tv.tv_usec=0;
    }

    freeaddrinfo(abase);

    if ((*conntype == 'c') && (ifa->direction != IN)) {
    /* This is an unusual but supported combination */
        if ((ifa->q =init_q(ift->qsize)) == NULL) {
            logerr(errno,"Interface duplication failed");
            return(NULL);
        }
        /* Disable Nagle. Not fatal if we fail for any reason */
        if (setsockopt(ift->fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on)) < 0)
            logerr(errno,"Could not disable Nagle algorithm for tcp socket");
    }

    if (flag_test(ifa,F_PERSIST)) {
        if (keepalive) {
            if (setsockopt(ift->fd,SOL_SOCKET,SO_KEEPALIVE,&on,sizeof(on)) < 0)
                logerr(errno,"Could not enable keepalives on tcp socket");

            if (keepidle)
#ifdef __APPLE__
                if (setsockopt(ift->fd,IPPROTO_TCP,TCP_KEEPALIVE,&keepidle,
                        sizeof(keepidle)) < 0)
                    logerr(errno,"Could not set tcp keepidle");
#else
                if (setsockopt(ift->fd,IPPROTO_TCP,TCP_KEEPIDLE,&keepidle,
                        sizeof(keepidle)) < 0)
                    logerr(errno,"Could not set tcp keepidle");
#endif
#if !defined (__APPLE__) || __MAC_OS_X_VERSION_MAX_ALLOWED >= 1090

            if (keepintvl)
                if (setsockopt(ift->fd,IPPROTO_TCP,TCP_KEEPINTVL,&keepintvl,
                        sizeof(keepintvl)) < 0)
                    logerr(errno,"Could not set tcp keepintvl");
            if (keepcnt)
                if (setsockopt(ift->fd,IPPROTO_TCP,TCP_KEEPCNT,&keepcnt,
                        sizeof(keepcnt)) < 0)
                    logerr(errno,"Could not set tcp keepcnt");
#endif
        }

        if (timeout) {
            if ((setsockopt(ift->fd,SOL_SOCKET,SO_SNDTIMEO,&ift->shared->tv,
                    sizeof(ift->shared->tv)) < 0) || (setsockopt(ift->fd,
                    SOL_SOCKET,SO_SNDBUF,&ift->shared->sndbuf,
                    sizeof(ift->shared->sndbuf)) <0))
                logerr(errno,"Could not set tcp send timeout");
        }
    }

    ifa->cleanup=cleanup_tcp;
    ifa->info = (void *) ift;
    if (*conntype == 'c') {
        ifa->read=do_read;
        ifa->readbuf=read_tcp;
        ifa->write=write_tcp;
        if (ifa->direction == BOTH) {
            if ((ifa->next=ifdup(ifa)) == NULL) {
                logerr(errno,"Interface duplication failed");
                return(NULL);
            }
            ifa->direction=OUT;
            ifa->pair->direction=IN;
        }
    } else {
        ifa->write=tcp_server;
        ifa->read=tcp_server;
    }
    free_options(ifa->options);
    return(ifa);
}

/* tcp.c
 * This file is part of kplex
 * Copyright Keith Young 2012-2016
 * For copying information see the file COPYING distributed with this software
 */

#include "kplex.h"
#include "tcp.h"
#include <netdb.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/uio.h>
#include <arpa/inet.h>

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

/*
 * Clean up a tcp interface on exit
 * Args: iface_t *
 * Returns: Nothing
 * Side effects: de-allocates memory and performs other cleanup tasks
 */
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
        if (ift->shared->port)
            free(ift->shared->port);
        if (ift->shared->host)
            free(ift->shared->host);
        if (ift->shared->preamble) {
            free((void *) ift->shared->preamble->string);
            free((void *) ift->shared->preamble);
        }
        pthread_mutex_destroy(&ift->shared->t_mutex);
        pthread_cond_destroy(&ift->shared->fv);
        free(ift->shared);
    }

    close(ift->fd);
}

/*
 * Send a preamble string (if defined)
 * Args: Pointer to an if_tcp structure, pointer to tcp_preamble structure
 * Returns: 0 on success, -1 on error
 * Side effects: Preamble string written to the interface's file descriptor
 */
int do_preamble(struct if_tcp *ift, struct tcp_preamble *preamble)
{
    size_t towrite;
    ssize_t n;

    if (preamble == NULL) {
        if (ift->shared == NULL || ift->shared->preamble == NULL)
            return (-1);
        preamble=ift->shared->preamble;
    }

    for (towrite=preamble->len;towrite;towrite-=n) {
        if ((n=write(ift->fd,preamble->string,towrite)) < 0)
            return(-1);
    }
    return(0);
}

/*
 * Set socket options to enable keepalives as required
 * Args: Pointer to an if_tcp structure
 * Returns: 0 on success, -1 if any errors occur
 * Side effects: Sets keepalive options on interface socket
 */
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
                err=-1;
    }
    return(err);
}

/*
 * Reconnect a lost connection in persist mode
 * Args: Pointer to interface and error raised by onnection failure
 * Returns: 0 on success, -1 in the case of an unrecoverable error
 * Side effects: Connection should be re-established on exit
 */
int reconnect(iface_t *ifa, int err)
{
    struct if_tcp *ift = (struct if_tcp *) ifa->info;
    struct if_tcp *iftp;
    int retval=0;
    int on=1;

    DEBUG(3,"%s: Reconnecting (write) interface",ifa->name);

    /* ift->shared_t_mutex shoudl be locked by the calling routine */

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
        DEBUG(6,"%s: Reconnecting...",ifa->name);
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
    DEBUG(3,"%s: Reconnected (write) interface",ifa->name);
    if (retval == 0) {
        if (ifa->pair) {
                iftp = (struct if_tcp *) ifa->pair->info;
                iftp->fd = ift->fd;
            }
        if (ift->shared->nodelay &&
                (setsockopt(ift->fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on))
                < 0))
            logerr(errno,"Could not disable Nagle on new tcp connection");
        (void) establish_keepalive(ift);
        if (ift->shared->preamble){
            do_preamble(ift,NULL);
        }
    }

    DEBUG(7,"Flushing queue interface %s",ifa->name);
    flush_queue(ifa->q);

    pthread_mutex_unlock(&ift->shared->t_mutex);
    return(retval);
}

/*
 * Reconnect a lost read connection in persist mode
 * Args: Pointer to interface
 *       Pointer to a buffer for newly read data
 *       Buffer size
 * Returns: Amount of data newly read on success, -1 on error
 * Side effects: Connection should be re-established on exit
 */
ssize_t reread(iface_t *ifa, char *buf, int bsize)
{
    struct if_tcp *ift = (struct if_tcp *) ifa->info;
    struct if_tcp *iftp;
    ssize_t nread;
    int fflags;
    int on=1;

    DEBUG(3,"%s: Reconnecting (read) interface",ifa->name);
    /* ift->shared->t_mutex should be held by the calling routine */
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
                DEBUG(7,"%s: Retrying connection...",ifa->name);
                if ((nread=connect(ift->fd,
                        (const struct sockaddr *)&ift->shared->sa,
                        ift->shared->sa_len)) == 0)
                    DEBUG(3,"%s: Reconnected (read) interface",ifa->name);

            }
        } else {
            nread=0;
        }
    }
    if (nread >= 0) {
        if (fcntl(ift->fd,F_SETFL,fflags) < 0) {
            logerr(errno,"Failed to make tcp socket blocking");
            nread=-1;
        }
    }
    if (nread == 0) {
        (void) establish_keepalive(ift);

        if (ifa->pair) {
            if (!(iftp = (struct if_tcp *) ifa->pair->info)) {
                logerr(errno,"No pair information found for bi-directional tcp connection!");
                nread=-1;
            } else {
                if (ift->shared->nodelay && (setsockopt(ift->fd,IPPROTO_TCP,
                        TCP_NODELAY,&on,sizeof(on)) < 0))
                    logerr(errno,
                            "Could not disable Nagle on new tcp connection");

                iftp->fd = ift->fd;
                if (iftp->shared->preamble)
                    do_preamble(iftp,NULL);
            }
        }
    }

    return(nread);
}

ssize_t read_tcp(struct iface *ifa, char *buf)
{
    struct if_tcp *ift = (struct if_tcp *) ifa->info;
    ssize_t nread;
    int done=0;

    /* Wow this is ugly and due a complete re-write.  In persist mode we first
       check if a pair thread (ie writer to this thread's reader on a bi-
       directional interface) has tried and failed to reconnect and if yes
       then we exit.  If not we increment a counter before entering blocking
       read. This is mainly for consistency with the write side.  If we don't
       read something (EOF or error) we exit if persist is not set but if it
       is set we check if there's another thread in the critical region
       (critical == 2).  If there is we do a shutdown on the socket and wait for
       the partner thread.  Once it catches up it waits, we fix the problem,
       then signal to the other thread to re-start.

       If there's no error on read we first check if another thread is waiting
       for this thread to fix a problem. If it is we tell it to go ahead and
       fix it because we're just leaving the critical region.
     */

    for(nread=0;nread<=0;) {
        if (flag_test(ifa,F_PERSIST)) {
            pthread_mutex_lock(&ift->shared->t_mutex);
            if (ift->fd == -1)
                done++;
            else
                ift->shared->critical++;
            pthread_mutex_unlock(&ift->shared->t_mutex);
            if (done) {
                nread=-1;
                break;
            }
        }
    
        /* Man pages lie!  On FreeBSD, Linux and OS X, SIGPIPE is NOT delivered
         * to a process reading from socket which times out due to unreplied to
         * keepalives.  Instead the read exits with ETIMEDOUT
         */
        nread=read(ift->fd,buf,BUFSIZ);
        if (nread <= 0) {
            if (nread) {
                DEBUG(3,"%s: %s",ifa->name,"Read Failed");
            } else {
                DEBUG(3,"%s: EOF",ifa->name);
            }

            if (!flag_test(ifa,F_PERSIST))
                break;
            pthread_mutex_lock(&ift->shared->t_mutex);
            if (ift->shared->fixing) {
                pthread_cond_signal(&ift->shared->fv);    
                pthread_cond_wait(&ift->shared->fv,&ift->shared->t_mutex);
            } else {
                if (ift->shared->critical == 2) {
                    ift->shared->fixing++;
                    (void) shutdown(ift->fd,SHUT_RDWR);
                    pthread_cond_wait(&ift->shared->fv,&ift->shared->t_mutex);
                }
                if ((nread=reread(ifa,buf,BUFSIZ)) < 0) {
                    if (ifa->pair)
                        ((struct if_tcp *)ifa->pair->info)->fd=-1;
                    logerr(errno,"failed to reconnect tcp connection");
                }
                if (ift->shared->fixing) {
                    ift->shared->fixing=0;
                    pthread_cond_signal(&ift->shared->fv);
                }
            }
            ift->shared->critical--;
            pthread_mutex_unlock(&ift->shared->t_mutex);
        } else if (flag_test(ifa,F_PERSIST)) {
            pthread_mutex_lock(&ift->shared->t_mutex);
            ift->shared->critical--;
            if (ift->shared->fixing)
                pthread_cond_signal(&ift->shared->fv);
            pthread_mutex_unlock(&ift->shared->t_mutex);
        }
    }
    return nread;
}

void write_tcp(struct iface *ifa)
{
    struct if_tcp *ift = (struct if_tcp *) ifa->info;
    senblk_t *sptr;
    int status=0;
    int err=0;
    int data=0;
    int cnt=1;
    int done = 0;
    struct iovec iov[2];

    if (ifa->tagflags) {
        if ((iov[0].iov_base=malloc(TAGMAX)) == NULL) {
                logerr(errno,"Disabing tag output on interface id %x (%s)",
                        ifa->id,ifa->name);
                ifa->tagflags=0;
        } else {
            cnt=2;
            data=1;
        }
    }

    for(;(!done);) {

        if ((sptr = next_senblk(ifa->q)) == NULL)
            break;

        if (senfilter(sptr,ifa->ofilter)) {
            senblk_free(sptr,ifa->q);
            continue;
        }

        if (ifa->tagflags)
            if ((iov[0].iov_len = gettag(ifa,iov[0].iov_base,sptr)) == 0) {
                logerr(errno,"Disabing tag output on interface id %x (%s)",
                        ifa->id,ifa->name);
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
        if (flag_test(ifa,F_PERSIST)) {
            pthread_mutex_lock(&ift->shared->t_mutex);
            if (ift->fd == -1)
                done++;
            else
                ift->shared->critical++;
            pthread_mutex_unlock(&ift->shared->t_mutex);
            if (done) {
                senblk_free(sptr,ifa->q);
                break;
            }
        }
        if (writev(ift->fd,iov,cnt) <0) {
            DEBUG2(3,"%s id %x: write failed",ifa->name,ifa->id);
            err=errno;
            if (!flag_test(ifa,F_PERSIST)) {
                senblk_free(sptr,ifa->q);
                break;
            }
            pthread_mutex_lock(&ift->shared->t_mutex);
            if (ift->shared->fixing) {
                pthread_cond_signal(&ift->shared->fv);
                pthread_cond_wait(&ift->shared->fv,&ift->shared->t_mutex);
            } else {
                if (ift->shared->critical == 2) {
                    ift->shared->fixing++;
                    (void) shutdown(ift->fd,SHUT_RDWR);
                    pthread_cond_wait(&ift->shared->fv,&ift->shared->t_mutex);
                }
                if ((status=reconnect(ifa,err)) <  0) {
                    if (ifa->pair)
                        ((struct if_tcp *) ifa->pair->info)->fd=-1;
                    logerr(errno,"failed to reconnect tcp connection");
                    done++;
                }
                if (ift->shared->fixing) {
                    ift->shared->fixing=0;
                    pthread_cond_signal(&ift->shared->fv);
                }
            }
            ift->shared->critical--;
            pthread_mutex_unlock(&ift->shared->t_mutex);
        } else if (flag_test(ifa,F_PERSIST)) {
            pthread_mutex_lock(&ift->shared->t_mutex);
            ift->shared->critical--;
            if (ift->shared->fixing)
                pthread_cond_signal(&ift->shared->fv);
            pthread_mutex_unlock(&ift->shared->t_mutex);
        }
        senblk_free(sptr,ifa->q);
    }

    if (cnt == 2)
        free(iov[0].iov_base);

    iface_thread_exit(errno);
}

void delayed_connect(iface_t *ifa)
{
    struct if_tcp *ift = (struct if_tcp *) ifa->info;
    struct if_tcp *iftp;
    struct addrinfo hints,*abase,*aptr;
    int err;
    int on=1;

    memset((void *)&hints,0,sizeof(hints));

    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_STREAM;

    pthread_mutex_lock(&ift->shared->t_mutex);

    while (ift->shared->host) {
        if ((err=getaddrinfo(ift->shared->host,ift->shared->port,&hints,&abase))) {
            if ((err != EAI_AGAIN && err != EAI_FAIL)) {
                logerr(0,"Lookup failed for host %s/service %s: %s",ift->shared->host,ift->shared->port,gai_strerror(err));
                iface_thread_exit(errno);
            }
            abase=NULL;
        }

        for (aptr=abase;aptr;aptr=aptr->ai_next) {
            if ((ift->fd=socket(aptr->ai_family,aptr->ai_socktype,aptr->ai_protocol)) < 0)
                continue;
            if (connect(ift->fd,aptr->ai_addr,aptr->ai_addrlen) == 0)
                break;
            close(ift->fd);
        }
        if (aptr) {
            ift->shared->sa_len=aptr->ai_addrlen;
            (void) memcpy(&ift->shared->sa,aptr->ai_addr,aptr->ai_addrlen);
            ift->shared->protocol=aptr->ai_protocol;
            freeaddrinfo(abase);
            free(ift->shared->host);
            free(ift->shared->port);
            ift->shared->host=ift->shared->port=NULL;
            if (ift->shared->nodelay &&
                    (setsockopt(ift->fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on))
                        < 0))
                logerr(errno,"Could not disable Nagle on new tcp connection");

            (void) establish_keepalive(ift);
            if (ifa->pair) {
                iftp = (struct if_tcp *) ifa->pair->info;
                iftp->fd = ift->fd;
            }
            /* do preamble */
            if (ift->shared->preamble)
                do_preamble(ift,NULL);

            DEBUG(3,"%s: Completed delayed connect",ifa->name);

        } else {
            DEBUG(4,"%s: Delayed connect failed (sleeping)",ifa->name);
            mysleep(ift->shared->retry);
        }
    }

    pthread_mutex_unlock(&ift->shared->t_mutex);

    if (ifa->direction == IN)
        do_read(ifa);
    else {
        write_tcp(ifa);
    }
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
            (init_q(newifa, oldift->qsize) < 0))) {
        if (newifa && newifa->q)
            free(newifa->q);
        if (newift)
            free(newift);
        free(newifa);
        return(NULL);
    }
    memset(newift,0,sizeof(struct if_tcp));

    newift->fd=fd;
    newift->shared=NULL;
    newifa->id=ifa->id+(fd&IDMINORMASK);
    newifa->direction=ifa->direction;
    newifa->type=TCP;
    newifa->name=ifa->name;
    newifa->info=newift;
    newifa->cleanup=cleanup_tcp;
    newifa->write=write_tcp;
    newifa->read=do_read;
    newifa->tagflags=ifa->tagflags;
    newifa->flags=ifa->flags;
    newifa->readbuf=read_tcp;
    newifa->lists=ifa->lists;
    newifa->ifilter=addfilter(ifa->ifilter);
    newifa->ofilter=addfilter(ifa->ofilter);
    newifa->checksum=ifa->checksum;
    newifa->strict=ifa->strict;
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
    int afd;
    socklen_t slen;
    struct if_tcp *ift=(struct if_tcp *)ifa->info;
    iface_t * newifa;
    struct sockaddr_storage sad;
    char addrs[INET6_ADDRSTRLEN];


    if (listen(ift->fd,5) == 0) {
        while(ifa->direction != NONE) {
            slen = sizeof(struct sockaddr_storage);
            if ((afd = accept(ift->fd,(struct sockaddr *) &sad,&slen)) < 0)
                break;
    
            if ((newifa = new_tcp_conn(afd,ifa)) == NULL) {
                close(afd);
                afd=-1;
            }
            DEBUG(3,"%s: New connection id %x %ssuccessfully received from %s",
                    ifa->name,newifa->id,(afd<0)?"un":"",
                    inet_ntop(sad.ss_family,(sad.ss_family == AF_INET)?
                    (const void *) &((struct sockaddr_in *)&sad)->sin_addr:
                    (const void *) &((struct sockaddr_in6 *)&sad)->sin6_addr,
                    addrs,INET6_ADDRSTRLEN));
        }
    }
    iface_thread_exit(errno);
}

struct tcp_preamble *parse_preamble(const char * val)
{
    const unsigned char *optr = (unsigned char *) val;
    unsigned char *ptr;
    unsigned char prebuf[MAXPREAMBLE];
    int count,tval,i;
    struct tcp_preamble *preamble;

    for (count=0,ptr=prebuf;*optr && count < MAXPREAMBLE;count++,optr++) {
        if (*optr == '\\') {
            switch (*(++optr)) {
            case 'a':
                *ptr++ = '\a';
                break;
            case 'b':
                *ptr++ = '\b';
                break;
            case 'f':
                *ptr++ = '\f';
                break;
            case 'n':
                *ptr++ = '\n';
                break;
            case 'r':
                *ptr++ = '\r';
                break;
            case 't':
                *ptr++ ='\t';
                break;
            case 'v':
                *ptr++ = '\v';
                break;
            case '\'':
                *ptr++ =  '\'';
                break;
            case '\"':
                *ptr++ =  '\"';
                break;
            case '?':
                *ptr++ =  '?';
                break;
            case 'x':
                for (i=0,tval=0;i<2;i++) {
                    if (*++optr == '\0')
                        return(NULL);
                    tval<<=4;
                    if (*optr >= '0' && *optr <= '9')
                        tval += *ptr - '0';
                    else if (*optr >= 'a' && *optr <= 'f')
                        tval += (*optr - 'a' + 10);
                    else if (*optr >= 'A' && *optr <= 'F')
                        tval += (*optr - 'A' + 10);
                    else
                        return(NULL);
                }
                *ptr++=(unsigned char) tval;
                break;
            case '\0':
                return(NULL);
            default:
                for (i=0,tval=0;i<3;i++) {
                    tval <<=3;
                    ++optr;
                    if (*optr >= '0' && *optr <= '7')
                        tval += (*optr - '0');
                    else if (i == 0) {
                        *ptr++ = *optr;
                        break;
                    } else
                        return(NULL);
                }
                if (i == 3) {
                    if (tval < 256)
                        *ptr++ = (unsigned char) tval;
                    else
                        return(NULL);
                } else
                    *ptr++ = *optr++;
                break;
            }
        } else
            *ptr++=*optr;
    }
    if (count == MAXPREAMBLE) {
        logerr(0,"Specified preamble is too long: Max %d chars",MAXPREAMBLE);
        return(0);
    }

    if ((preamble = (struct tcp_preamble *)
            malloc(sizeof(struct tcp_preamble))) == NULL) {
        logerr(errno,"Failed to allocate memory for preamble");
        return(NULL);
    }
    if ((preamble->string=(unsigned char *) malloc(count)) == NULL) {
        logerr(errno,"Failed to allocate memory for preamble string");
        if (preamble)
            free(preamble);
        return(NULL);
    }
    memcpy(preamble->string,prebuf,count);
    preamble->len=(size_t) count;
    return(preamble);
}

                

iface_t *init_tcp(iface_t *ifa)
{
    struct if_tcp *ift;
    char *host,*port,*eptr=NULL;
    struct addrinfo hints,*connection,*abase;
    struct servent *svent;
    struct tcp_preamble *preamble=NULL;
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
    int nodelay=1;
    long timeout=-1;
    int gpsd=0;

    host=port=NULL;

    if ((ift = malloc(sizeof(struct if_tcp))) == NULL) {
        logerr(errno,"Could not allocate memory");
        return(NULL);
    }

    ift->qsize=DEFTCPQSIZE;
    ift->shared=NULL;
    preamble=NULL;

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
        } else if (!strcasecmp(opt->var,"gpsd")) {
            if (!strcasecmp(opt->val,"yes")) {
                gpsd=1;
                if (!port)
                    port="2947";
            } else if (!strcasecmp(opt->val,"no")) {
                gpsd=0;
            } else {
                logerr(0,"Invalid option \"gpsd=%s\"",opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"preamble")) {
            if (preamble) {
                logerr(0,"Can only specify preamble once");
                return(NULL);
            }
            if ((preamble=parse_preamble(opt->val)) == NULL) {
                logerr(0,"Could not parse preamble %s",opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"nodelay")) {
            if (!strcasecmp(opt->val,"no")) {
                nodelay=0;
            } else if (!strcasecmp(opt->val,"yes")) {
                nodelay=1;
            } else {
                logerr(0,"Invalid option \"nodelay=%s\"",opt->val);
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
        if (gpsd) {
            if (preamble) {
                logerr(0,"Can't specify preamble with proto=gpsd");
                return(NULL);
            }
            preamble=parse_preamble("?WATCH={\"enable\":true,\"nmea\":true}");
        }
    } else {
        if (flag_test(ifa,F_PERSIST)) {
            logerr(0,"persist option not valid for tcp servers");
            return(NULL);
        }

        if (preamble) {
            logerr(0,"preamble option not valid for servers");
            free(preamble->string);
            free(preamble);
            return(NULL);
        }

        if (gpsd) {
            logerr(0,"proto=gpsd not valid for servers");
            return(NULL);
        }
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
        if (flag_test(ifa,F_IPERSIST) && (err == EAI_AGAIN || err == EAI_FAIL)){
            abase=NULL;
        } else {
            logerr(0,"Lookup failed for host %s/service %s: %s",host,port,gai_strerror(err));
            return(NULL);
        }
    }

    for (connection=abase;connection;connection=connection->ai_next) {
        if ((ift->fd=socket(connection->ai_family,connection->ai_socktype,connection->ai_protocol)) < 0)
            continue;
        if (*conntype == 'c') {
            if (connect(ift->fd,connection->ai_addr,connection->ai_addrlen) == 0)
                break;
        } else {
            setsockopt(ift->fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));
            if (connection->ai_family == AF_INET6) {
                for (ptr=((struct sockaddr_in6 *)connection->ai_addr)->sin6_addr.s6_addr,i=0;i<16;i++,ptr++)
                    if (*ptr)
                        break;
                if (i == sizeof(struct in6_addr)) {
                    if (setsockopt(ift->fd,IPPROTO_IPV6,IPV6_V6ONLY,
                            (void *)&off,sizeof(off)) <0) {
                        logerr(errno,"Failed to set ipv6 mapped ipv4 addresses on socket");
                    }
                }
            }
            if (bind(ift->fd,connection->ai_addr,connection->ai_addrlen) == 0)
                break;
            err=errno;
        }
        close(ift->fd);
     }

    if (connection == NULL && (!flag_test(ifa,F_IPERSIST))) {
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

        if (pthread_cond_init(&ift->shared->fv,NULL) != 0) {
            logerr(errno,"tcp condition variable initialisation failed");
            return(NULL);
        }

        ift->shared->retry=retry;
        if (ift->shared->retry != retry) {
            logerr(0,"retry value out of range");
            return(NULL);
        }
        if (connection) {
            ift->shared->sa_len=connection->ai_addrlen;
            (void) memcpy(&ift->shared->sa,connection->ai_addr,connection->ai_addrlen);
            ift->shared->protocol=connection->ai_protocol;
            ift->shared->host=ift->shared->port=NULL;
        } else {
            ift->shared->host=strdup(host);
            ift->shared->port=strdup(port);
            DEBUG(3,"%s: Initial connection to %s port %s failed",ifa->name,
                    host,port);
        }
        ift->shared->donewith=1;
        ift->shared->critical=0;
        ift->shared->fixing=0;
        ift->shared->keepalive=keepalive;
        ift->shared->keepidle=keepidle;
        ift->shared->keepintvl=keepintvl;
        ift->shared->keepcnt=keepcnt;
        ift->shared->sndbuf=sndbuf;
        ift->shared->tv.tv_sec=timeout;
        ift->shared->tv.tv_usec=0;
        ift->shared->nodelay=nodelay;
        ift->shared->preamble=preamble;
    }

    freeaddrinfo(abase);

    if (flag_test(ifa,F_PERSIST) && (connection)) {
        (void) establish_keepalive(ift);    
    }

    if ((*conntype == 'c') && (ifa->direction != IN)) {
    /* This is an unusual but supported combination */
        if (init_q(ifa, ift->qsize) < 0) {
            logerr(errno,"Interface duplication failed");
            return(NULL);
        }
        /* Disable Nagle. Not fatal if we fail for any reason */
        if (connection) {
            if (nodelay && (setsockopt(ift->fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on)) < 0))
                logerr(errno,"Could not disable Nagle algorithm for tcp socket");
        }
    }

    ifa->cleanup=cleanup_tcp;
    ifa->info = (void *) ift;
    if (*conntype == 'c') {
        if (connection) {
            if (preamble) {
                do_preamble(ift,preamble);
                if (ift->shared == NULL) {
                    free(preamble->string);
                    free(preamble);
                }
            }
            ifa->read=do_read;
            ifa->write=write_tcp;
        } else {
            ifa->read=delayed_connect;
            ifa->write=delayed_connect;
        }
        ifa->readbuf=read_tcp;
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
    DEBUG(3,"%s: initialised",ifa->name);
    return(ifa);
}

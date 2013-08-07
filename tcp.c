/* tcp.c
 * This file is part of kplex
 * Copyright Keith Young 2012-2013
 * For copying information see the file COPYING distributed with this software
 */

#include "kplex.h"
#include <netdb.h>
#include <fcntl.h>
#include <netinet/tcp.h>

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

int reconnect(iface_t *ifa)
{
    struct if_tcp *ift = (struct if_tcp *) ifa->info;
    senblk_t *sptr;
    int retval=0;
    int conres;

    pthread_mutex_lock(&ift->shared->t_mutex);
    for (;;) {
	    if ((sptr = last_senblk(ifa->q)) == NULL) {
            retval = 1;
	    	break;
        }

        if (senfilter(sptr,ifa->ofilter)) {
            senblk_free(sptr,ifa->q);
            continue;
        }

	    if ((send(ift->fd,sptr->data,sptr->len,0)) <0) {
            senblk_free(sptr,ifa->q);
	        switch(errno) {
	        case ECONNREFUSED:
	        case ENETUNREACH:
	        case ETIMEDOUT:
	        case EAGAIN:
                for(conres=-1;conres < 0;) {
	                close(ift->fd);
	                sleep(ift->shared->retry);
	                if ((ift->fd=socket(ift->shared->sa.ss_family,SOCK_STREAM,
                            ift->shared->protocol)) < 0) {
	                    logerr(errno,"Failed to create socket");
	                    retval=-1;
	                    break;
	                }
	                conres=connect(ift->fd,(const struct sockaddr *)
                            &ift->shared->sa,ift->shared->sa_len);
                }
	            break;
	        default:
	            logerr(errno,"Failed to reconnect socket");
	            retval=-1;
	        }
	        if (retval)
	            break;
	    }
    }
    pthread_mutex_unlock(&ift->shared->t_mutex);
    return(retval);
}
    
int reread(iface_t *ifa, char *buf, int bsize)
{
    struct if_tcp *ift = (struct if_tcp *) ifa->info;
    int nread,ret;
    int fflags;

    /* Make socket non-blocking so we don'hold the mutex longer
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
            for (ret=-1;ret!=0;) {
                close(ift->fd);
                if ((ift->fd=socket(ift->shared->sa.ss_family,SOCK_STREAM,
                        ift->shared->protocol)) < 0) {
                    logerr(errno,"Failed to create socket");
                    nread=-1;
                    break;
                }

                sleep(ift->shared->retry);
                ret=connect(ift->fd,(const struct sockaddr *)&ift->shared->sa,
                        ift->shared->sa_len);
            }
        }
        nread=0;
    }

    if (fcntl(ift->fd,F_SETFL,fflags) < 0) {
        logerr(errno,"Failed to make tcp socket blocking");
        nread=-1;
    }

    pthread_mutex_unlock(&ift->shared->t_mutex);
    return(nread);
}

void read_tcp(struct iface *ifa)
{
	char buf[BUFSIZ];
	char *bptr,*eptr=buf+BUFSIZ,*senptr;
	senblk_t sblk;
	struct if_tcp *ift = (struct if_tcp *) ifa->info;
	int nread,cr=0,count=0,overrun=0;

	senptr=sblk.data;
    sblk.src=ifa->id;

    while (ifa->direction != NONE) {
	    if ((nread=read(ift->fd,buf,BUFSIZ)) <=0) {
            if (!ifa->persist)
                break;
            if ((nread=reread(ifa,buf,BUFSIZ)) < 0) {
                logerr(errno,"failed to reconnect tcp connection");
                break;
            }
        }
		for(bptr=buf,eptr=buf+nread;bptr<eptr;bptr++) {
			if (count < SENMAX+2) {
				++count;
				*senptr++=*bptr;
			} else
				++overrun;

			if ((*bptr) == '\r') {
				++cr;
			} else {
				if (*bptr == '\n' && cr) {
					if (overrun) {
						overrun=0;
					} else {
						sblk.len=count;
                        if (!(ifa->checksum && checkcksum(&sblk)) &&
                                senfilter(&sblk,ifa->ifilter) == 0)
                            push_senblk(&sblk,ifa->q);
					}
					senptr=sblk.data;
					count=0;
				}
				cr=0;
			}
		}
	}
	iface_thread_exit(errno);
}

void write_tcp(struct iface *ifa)
{
	struct if_tcp *ift = (struct if_tcp *) ifa->info;
	senblk_t *sptr;
    int status;

	for(;;) {
		if ((sptr = next_senblk(ifa->q)) == NULL)
			break;

        if (senfilter(sptr,ifa->ofilter)) {
            senblk_free(sptr,ifa->q);
            continue;
        }

        /* SIGPIPE is blocked here so we can avoid using the (non-portable)
         * MSG_NOSIGNAL
         */
        if ((send(ift->fd,sptr->data,sptr->len,0)) <0) {
            if (!ifa->persist)
		        break;
		    senblk_free(sptr,ifa->q);
            if ((status=reconnect(ifa)) != 0) {
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

    if ((newifa = malloc(sizeof(iface_t))) == NULL)
        return(NULL);

    memset(newifa,0,sizeof(iface_t));
    if (((newift = (struct if_tcp *) malloc(sizeof(struct if_tcp))) == NULL) ||
            ((ifa->direction != IN) &&
            ((newifa->q=init_q(oldift->qsize)) == NULL))){
        if (newifa && newifa->q)
            free(newifa->q);
        if (newift->shared)
            free(newift->shared);
        if (newift)
            free(newift);
        free(newifa);
        return(NULL);
    }
    newift->fd=fd;
    newifa->id=ifa->id+(fd&IDMINORMASK);
    newifa->direction=ifa->direction;
    newifa->type=TCP;
    newifa->name=NULL;
    newifa->info=newift;
    newifa->cleanup=cleanup_tcp;
    newifa->write=write_tcp;
    newifa->read=read_tcp;
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
            link_to_initialized(newifa->pair);
            pthread_create(&tid,NULL,(void *)start_interface,(void *) newifa->pair);
        }
    }
    link_to_initialized(newifa);
    pthread_create(&tid,NULL,(void *)start_interface,(void *) newifa);
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

    host=port=NULL;

    if ((ift = malloc(sizeof(struct if_tcp))) == NULL) {
        logerr(errno,"Could not allocate memory");
        return(NULL);
    }

    ift->qsize=DEFTCPQSIZE;

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
            if (!ifa->persist) {
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
        }  else if (!strcasecmp(opt->var,"qsize")) {
            if (!(ift->qsize=atoi(opt->val))) {
                logerr(0,"Invalid queue size specified: %s",opt->val);
                return(NULL);
            }
        } else  {
            logerr(0,"unknown interface option %s\n",opt->var);
            return(NULL);
        }
    }

    if (*conntype == 'c') {
        if (!host) {
            logerr(0,"Must specify address for tcp client mode\n");
            return(NULL);
        }
    } else if (ifa->persist) {
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

    if (ifa->persist) {
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

    ifa->cleanup=cleanup_tcp;
    ifa->info = (void *) ift;
    if (*conntype == 'c') {
        ifa->read=read_tcp;
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

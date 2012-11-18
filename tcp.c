/* tcp.c
 * This file is part of kplex
 * Copyright Keith Young 2012
 * For copying information see the file COPYING distributed with this software
 */

#include "kplex.h"
#include <netdb.h>

#define DEFTCPQSIZE 128

struct if_tcp {
    int fd;
};

/*
 * Duplicate struct if_tcp
 * Args: if_tcp to be duplicated
 * Returns: pointer to new if_serial
 * Should we dup, copy or re-open the fd here?
 */
void *ifdup_tcp(void *ift)
{
    struct if_tcp *oldif,*newif;

    if ((newif = (struct if_tcp *) malloc(sizeof(struct if_tcp)))
        == (struct if_tcp *) NULL)
        return(NULL);
    oldif = (struct if_tcp *) ift;

    if ((newif->fd=dup(oldif->fd)) <0) {
        free(newif);
        return(NULL);
    }
    return ((void *) newif);
}

void cleanup_tcp(iface_t *ifa)
{
    struct if_tcp *ift = (struct if_tcp *)ifa->info;
    int how;

    switch(ifa->direction) {
    case IN:
        how=SHUT_RD;
        break;
    case OUT:
        how=SHUT_WR;
        break;
    case BOTH:
        how=SHUT_RDWR;
    }

    close(ift->fd);
}

struct iface * read_tcp(struct iface *ifa)
{
	char buf[BUFSIZ];
	char *bptr,*eptr=buf+BUFSIZ,*senptr;
	senblk_t sblk;
	struct if_tcp *ift = (struct if_tcp *) ifa->info;
	int nread,cr=0,count=0,overrun=0;
	int fd;

	senptr=sblk.data;
    sblk.src=ifa->id;
	fd=ift->fd;

	while ((ifa->direction != NONE) && (nread=read(fd,buf,BUFSIZ)) > 0) {
		for(bptr=buf,eptr=buf+nread;bptr<eptr;bptr++) {
			if (count < SENMAX) {
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

struct iface * write_tcp(struct iface *ifa)
{
	struct if_tcp *ift = (struct if_tcp *) ifa->info;
	senblk_t *sptr;
	int n;

#ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0
    n=1;
    setsockopt(ift->fd,SOL_SOCKET, SO_NOSIGPIPE, (void *)&n, sizeof(int));
#endif
	for(;;) {
		if ((sptr = next_senblk(ifa->q)) == NULL)
			break;

        if (senfilter(sptr,ifa->ofilter)) {
            senblk_free(sptr,ifa->q);
            continue;
        }

        if ((send(ift->fd,sptr->data,sptr->len,MSG_NOSIGNAL)) <0)
		    break;
		senblk_free(sptr,ifa->q);
	}

	iface_thread_exit(errno);
}

iface_t *new_tcp_conn(int fd, iface_t *ifa)
{
    iface_t *newifa;
    struct if_tcp *newift=NULL,*ift=( struct if_tcp *)ifa->info;
    pthread_t tid;

    if ((newifa = malloc(sizeof(iface_t))) == NULL)
        return(NULL);

    memset(newifa,0,sizeof(iface_t));
    if (((newift = malloc(sizeof(struct if_tcp))) == NULL) ||
        ((ifa->direction != IN) &&  ((newifa->q=init_q(DEFTCPQSIZE)) == NULL))){
            if (newifa && newifa->q)
                free(newifa->q);
            if (newift)
                free(newift);
            free(newifa);
            return(NULL);
    }
    newift->fd=fd;
    newifa->id=ifa->id+fd&(2^IDMINORBITS-1);
    newifa->direction=ifa->direction;
    newifa->type=TCP;
    newifa->id=ifa->id;
    newifa->name=NULL;
    newifa->info=newift;
    newifa->cleanup=cleanup_tcp;
    newifa->write=write_tcp;
    newifa->read=read_tcp;
    newifa->lists=ifa->lists;
    newifa->ifilter=addfilter(ifa->ifilter);
    newifa->ofilter=addfilter(ifa->ifilter);
    newifa->checksum=ifa->checksum;
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
        newifa->pair->q=ifa->q;
        link_to_initialized(newifa->pair);
        pthread_create(&tid,NULL,(void *)start_interface,(void *) newifa->pair);
    }

    link_to_initialized(newifa);
    pthread_create(&tid,NULL,(void *)start_interface,(void *) newifa);
    return(newifa);
}

iface_t *tcp_server(iface_t *ifa)
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
    char *host,*port;
    struct addrinfo hints,*aptr;
    struct servent *svent;
    int tport,err,on=1;
    char *conntype = "c";
    size_t qsize=DEFTCPQSIZE;
    struct kopts *opt;

    host=port=NULL;

    if ((ift = malloc(sizeof(struct if_tcp))) == NULL) {
        logtermall(errno,"Could not allocate memory");
    }

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"address"))
            host=opt->val;
        else if (!strcasecmp(opt->var,"mode")) {
            if (strcasecmp(opt->val,"client") && strcasecmp(opt->val,"server")){
                logtermall(0,"Unknown tcp mode %s (must be \'client\' or \'server\')",opt->val);
            }
            conntype=opt->val;
        } else if (!strcasecmp(opt->var,"port")) {
            port=opt->val;
        }  else if (!strcasecmp(opt->var,"qsize")) {
            if (!(qsize=atoi(opt->val))) {
                logtermall(0,"Invalid queue size specified: %s",opt->val);
            }
        } else  {
            logtermall(0,"unknown interface option %s\n",opt->var);
        }
    }

    if (*conntype == 'c' && !host) {
        logtermall(0,"Must specify address for tcp client mode\n");
    }

    if (!port) {
        if ((svent=getservbyname("nmea-0183","tcp")) != NULL)
            port=svent->s_name;
        else
            port = DEFTCPPORT;
    }

    memset((void *)&hints,0,sizeof(hints));

    hints.ai_flags=AI_CANONNAME|(*conntype == 's')?AI_PASSIVE:0;
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_STREAM;

    if (err=getaddrinfo(host,port,&hints,&aptr)) {
        logtermall(errno,"Lookup failed for host %s/service %s: %s",host,port,gai_strerror(err));
    }

    do {
        if ((ift->fd=socket(aptr->ai_family,aptr->ai_socktype,aptr->ai_protocol)) < 0)
            continue;
        if (*conntype == 'c') {
            if (connect(ift->fd,aptr->ai_addr,aptr->ai_addrlen) == 0)
                break;
        } else {
            setsockopt(ift->fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));
            if (bind(ift->fd,aptr->ai_addr,aptr->ai_addrlen) == 0)
                break;
        }
        close(ift->fd);
     } while (aptr = aptr->ai_next);

    if (aptr == NULL) {
        logtermall(errno,"Failed to open tcp %s for %s/%s",(*conntype == 's')?"server":"connection",host,port);
    }

    if ((*conntype == 'c') && (ifa->direction != IN)) {
    /* This is an unusual but supported combination */
        if ((ifa->q =init_q(DEFTCPQSIZE)) == NULL) {
            logtermall(errno,"Interface duplication failed");
        }
    }

    ifa->cleanup=cleanup_tcp;
    ifa->info = (void *) ift;
    if (*conntype == 'c') {
        ifa->read=read_tcp;
        ifa->write=write_tcp;
        if (ifa->direction == BOTH) {
            if ((ifa->next=ifdup(ifa)) == NULL) {
                logtermall(errno,"Interface duplication failed");
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

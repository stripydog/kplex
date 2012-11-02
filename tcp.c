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

    shutdown(ift->fd,how);
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
    sblk.src=ifa;
	fd=ift->fd;

	while ((nread=read(fd,buf,BUFSIZ)) > 0) {
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
						push_senblk(&sblk,ifa->q);
					}
					senptr=sblk.data;
					count=0;
				}
				cr=0;
			}
		}
	}
	iface_destroy(ifa,(void *) &errno);
}

struct iface * write_tcp(struct iface *ifa)
{
	struct if_tcp *ift = (struct if_tcp *) ifa->info;
	senblk_t *sptr;
	int n;

	for(;;) {
		if ((sptr = next_senblk(ifa->q)) == NULL)
			break;
        if ((send(ift->fd,sptr->data,sptr->len,0)) <0)
		    break;

		senblk_free(sptr,ifa->q);
	}
	iface_destroy(ifa,(void *) &errno);
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
    newifa->direction=ifa->direction;
    newifa->type=TCP;
    newifa->info=newift;
    newifa->cleanup=cleanup_tcp;
    newifa->write=write_tcp;
    newifa->read=read_tcp;
    newifa->lists=ifa->lists;
    if (ifa->direction == BOTH) {
        if ((newifa->pair=ifdup(newifa)) == NULL) {
            perror("Interface duplication failed");
            free(newifa->q);
            free(newift);
            free(newifa);
            return(NULL);
        }
        newifa->direction=OUT;
        newifa->pair->direction=IN;
        newifa->pair->q=ifa->q;
        pthread_create(&tid,NULL,(void *)start_interface,(void *) newifa->pair);
    }

    if (newifa->direction == OUT) {
        pthread_mutex_lock(&ifa->lists->io_mutex);
        ++ifa->lists->uninitialized;
        pthread_mutex_unlock(&ifa->lists->io_mutex);
    }

    pthread_create(&tid,NULL,(void *)start_interface,(void *) newifa);
    return(newifa);
}

iface_t *tcp_server(iface_t *ifa)
{
    struct if_tcp *ift=(struct if_tcp *)ifa->info;
    int afd;


    if (listen(ift->fd,5) == 0) {
        for(;;) {
         if ((afd = accept(ift->fd,NULL,NULL)) < 0)
             break;
    
         if (new_tcp_conn(afd,ifa) == NULL)
             close(afd);
     }
    }
    iface_destroy(ifa,(void *)&errno);
}

iface_t *init_tcp(char *str,iface_t *ifa)
{
    struct if_tcp *ift;
    char *host,*port=NULL;
    struct addrinfo hints,*aptr;
    struct servent *svent;
    int err,on=1;
    char *conntype;

    if ((ift = malloc(sizeof(struct if_tcp))) == NULL) {
        perror("Could not allocate memory");
        exit(1);
    }

    if (((conntype=strtok(str+4,",")) == NULL) || ((*conntype != 's')
                                        && (*conntype != 'c'))) {
        fprintf(stderr,"Invalid interface specification %s\n",str);
        exit(1);
    }
    if (host=strtok(NULL,",")) {
        if (!strcmp(host,"-")) {
            host=NULL;
        }
        port=strtok(NULL,",");
    }
    if (port == NULL) {
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
        fprintf(stderr,"Lookup failed for host %s/service %s: %s\n",host,port,gai_strerror(err));
        exit(1);
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
        fprintf(stderr,"Failed to open tcp %s for %s/%s\n",(*conntype == 's')?"server":"connection",host,port);
        exit(1);
    }

    if ((*conntype == 'c') && (ifa->direction != IN)) {
    /* This is an unusual but supported combination */
        if ((ifa->q =init_q(DEFTCPQSIZE)) == NULL) {
            perror("Interface duplication failed");
            exit(1);
        }
    }

    ifa->cleanup=cleanup_tcp;
    ifa->info = (void *) ift;
    if (*conntype == 'c') {
        ifa->read=read_tcp;
        ifa->write=write_tcp;
        if (ifa->direction == BOTH) {
            if ((ifa->pair=ifdup(ifa)) == NULL) {
                perror("Interface duplication failed");
                exit(1);
            }
            ifa->next=ifa->pair;
            ifa->direction=OUT;
            ifa->pair->direction=IN;
        }
    } else {
        ifa->write=tcp_server;
        ifa->read=tcp_server;
    }
    return(ifa);
}

/* bcast.c
 * This file is part of kplex
 * Copyright Keith Young 2012
 * For copying information see the file COPYING distributed with this software
 */

#include "kplex.h"
#include <netdb.h>

#define DEFBCASTQSIZE 64

struct if_bcast {
    int fd;
    struct sockaddr_in addr;
};

void cleanup_bcast(iface_t *ifa)
{
    struct if_bcast *ifb = (struct if_bcast *) ifa->info;

    close(ifb->fd);
}

iface_t * write_bcast(struct iface *ifa)
{
    struct if_bcast *ifb;
    senblk_t *sptr;
int n;

    ifb = (struct if_bcast *) ifa->info;

    for (;;) {
        if ((sptr = next_senblk(ifa->q)) == NULL)
            break;
        if ((n=sendto(ifb->fd,sptr->data,sptr->len,0,(struct sockaddr *)&ifb->addr,sizeof(struct sockaddr))) < 0)
            break;
        senblk_free(sptr,ifa->q);
    }
    iface_destroy(ifa,&errno);
}

iface_t *read_bcast(struct iface *ifa)
{
    struct if_bcast *ifb;
    senblk_t sblk;
    char buf[BUFSIZ];
    char *bptr,*eptr,*senptr;
    int nread,cr=0,count=0,overrun=0;

    ifb=(struct if_bcast *) ifa->info;

    senptr=sblk.data;

    while ((nread=recvfrom(ifb->fd,buf,BUFSIZ,0,NULL,0)) > 0) {
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

struct iface *init_bcast(char *str,struct iface *ifa)
{
    struct if_bcast *ifb;
    char *pptr,*host;
    short port=DEFBCASTPORT;
    struct servent *svent;
    const int on = 1;
    
    if ((ifb =malloc(sizeof(struct if_bcast))) == NULL) {
        perror("Could not allocate memory");
        exit(1);
    }

    if ((host=strtok(str+4,","))) {
        if (!strcmp(host,"-")) {
            host=NULL;
        }
        if (pptr=strtok(NULL,","))  {
            if ((port=atoi(pptr)) > 2^(sizeof(short) -1)) {
                fprintf(stderr,"port %s out of range\n",pptr);
                exit(1);
            }
        } else {
            if ((svent = getservbyname("nmea-0183","udp")) != NULL)
                port=svent->s_port;
        }
    }

    if ((host == NULL) && (ifa->direction == IN))
        ifb->addr.sin_addr.s_addr=htonl(INADDR_ANY);
    else {
        if (inet_pton(AF_INET,host?host:"255.255.255.255",&ifb->addr.sin_addr.s_addr) <= 0) {
            fprintf (stderr,"%s does not specify a valid broadcast address\n",str);
            exit(1);
        }
    }

    ifb->addr.sin_port=htons(port);

    if ((ifb->fd=socket(AF_INET,SOCK_DGRAM,0)) < 0) {
           perror("Could not create UDP socket");
               exit (1);
     }

    if (ifa->direction == OUT) {
        if (setsockopt(ifb->fd,SOL_SOCKET,SO_BROADCAST,&on,sizeof(on)) < 0) {
            perror("Setsockopt failed");
            exit(1);
        }
    } else {
        if (bind(ifb->fd,(const struct sockaddr *) &ifb->addr,sizeof(ifb->addr)) < 0) {
            perror("Bind failed");
            exit(1);
        }
    }

    if (ifa->direction == OUT)
        if ((ifa->q = init_q(DEFBCASTQSIZE)) == NULL) {
            perror("Could not create queue\n");
            exit(1);
        }
    ifa->write=write_bcast;
    ifa->read=read_bcast;
    ifa->cleanup=cleanup_bcast;
    ifa->info = (void *) ifb;
    return(ifa);
}

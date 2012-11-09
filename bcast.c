/* bcast.c
 * This file is part of kplex
 * Copyright Keith Young 2012
 * For copying information see the file COPYING distributed with this software
 *
 * This is hideous and proves what an abomination IPv4 broadcast is.
 * The simple case would be straight forward. Listen for broadcasts. Or
 * make them. But oh dear if you make *and* receive them in a multiplexer.
 * And even so, do you want to broadcast on all interfaces?
 * Code here has some protection.  Outgoing broadcast kplex interfaces must
 * specify a physical interface and potionally broadcast address to use.
 * This address is added to an ignore list and all packets received from 
 * addresses on that list are dropped. This may cause problems with dynamic
 * interfaces if the interface address changes. Workarrounds make this code
 * muckier than other interface types.
 */

#include "kplex.h"
#include <netdb.h>
#include <ifaddrs.h>

#define DEFBCASTQSIZE 64

/* structures for list of (local outbound) addresses to ignore when receiving */

static struct ignore_addr {
    struct sockaddr_in iaddr;
    struct ignore_addr *next;
} *ignore;

struct if_bcast {
    int fd;
    struct sockaddr_in addr;        /* Outbound address */
    struct sockaddr_in laddr;       /* local (bind) address */
};

/*
 * Duplicate broadcast specific info
 * Args: if_bcast to be duplicated (cast to void *)
 * Returns: pointer to new if_bcast (cast to void *)
 */
void *ifdup_bcast(void *ifb)
{
    struct if_bcast  *oldif,*newif;

    oldif = (struct if_bcast *)ifb;

    if ((newif = (struct if_bcast *) malloc(sizeof(struct if_bcast)))
        == (struct if_bcast *) NULL)
        return(NULL);

    /* Whole new socket so we can bind() it to a different address */
    if ((newif->fd = socket(AF_INET,SOCK_DGRAM,0)) < 0) {
        perror("Could not create duplicate socket");
        free(newif);
        return(NULL);
    }

    (void) memcpy(&newif->addr, &oldif->addr, sizeof(oldif->addr));

    /* unfortunately this will need changing and the new address binding. */
    (void) memcpy(&newif->laddr, &oldif->laddr, sizeof(oldif->laddr));

    return((void *) newif);
}

void cleanup_bcast(iface_t *ifa)
{
    struct if_bcast *ifb = (struct if_bcast *) ifa->info;

    close(ifb->fd);

    /* We could remove outgoing interfaces from the ignore list here, but
     * we'd have to check they weren't in use by some other interface */
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
    struct sockaddr_in src;
    struct ignore_addr *igp;
    socklen_t sz = (socklen_t) sizeof(src);
    ifb=(struct if_bcast *) ifa->info;

    senptr=sblk.data;
    sblk.src=ifa;

    while ((nread=recvfrom(ifb->fd,buf,BUFSIZ,0,(struct sockaddr *) &src,&sz))
                    > 0) {
                /* Probably superfluous check that we got the right size
                 * structure back */
                if (sz != (socklen_t) sizeof(src)) {
                    sz = (socklen_t) sizeof(src);
                    continue;
                }

                /* Compare the source address to the list of interfaces we're
                 * ignoringing */
                for (igp=ignore;igp;igp=igp->next)
                    if (igp->iaddr.sin_addr.s_addr == src.sin_addr.s_addr)
                        break;
                /* If igp points to anything, we broke out of the above loop
                 * on a match. Drop the packet and carry on */
                if (igp)
                    continue;

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

struct iface *init_bcast(struct iface *ifa)
{
    struct if_bcast *ifb;
    char *ifname,*bname;
    struct in_addr  baddr;
    int port=0;
    struct servent *svent;
    const int on = 1;
    static struct ifaddrs *ifap;
    struct ifaddrs *ifp=NULL;
    struct ignore_addr **igpp,*newig;
    size_t qsize = DEFBCASTQSIZE;
    struct kopts *opt;
    
    if ((ifb=malloc(sizeof(struct if_bcast))) == NULL) {
        perror("Could not allocate memory");
        exit(1);
    }
    memset(ifb,0,sizeof(struct if_bcast));

    ifname=bname=NULL;

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"device"))
            ifname=opt->val;
        else if (!strcasecmp(opt->var,"address"))
            bname=opt->val;
        else if (!strcasecmp(opt->var,"port")) {
            if (((port=atoi(opt->val)) > 0) && (port > 2^(sizeof(short) -1))) {
                fprintf(stderr,"port %s out of range\n",opt->val);
                exit(1);
            }
        }  else if (!strcasecmp(opt->var,"qsize")) {
            if (!(qsize=atoi(opt->val))) {
                fprintf(stderr,"Invalid queue size specified: %s",opt->val);
                exit(1);
            }
        } else  {
            fprintf(stderr,"unknown interface option %s\n",opt->var);
            exit(1);
        }
    }

    if (!port) {
        if ((svent = getservbyname("nmea-0183","udp")) != NULL)
            port=svent->s_port;
        else
            port=DEFBCASTPORT;
    }

    ifb->addr.sin_family = ifb->laddr.sin_family = AF_INET;

    if (ifname == NULL) {
        if (ifa->direction != IN) {
            fprintf(stderr,"Must specify interface for outgoing broadcasts\n");
            exit (1);
        }
        if (bname)
            ifb->laddr.sin_addr.s_addr = baddr.s_addr;
        else
            ifb->laddr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (ifap == NULL)
            if (getifaddrs(&ifap) < 0) {
                perror("Error getting interface info");
                exit(1);
        }
        for (ifp=ifap;ifp;ifp=ifp->ifa_next) {
            if ((!strcmp(ifname,ifp->ifa_name)) &&
                (ifp->ifa_addr->sa_family == AF_INET) &&
                ((bname == NULL) || (baddr.s_addr == 0xffffffff) ||
                (baddr.s_addr == ((struct sockaddr_in *) ifp->ifa_broadaddr)->sin_addr.s_addr)))
                break;
        }
        if (!ifp) {
            fprintf(stderr,"No IPv4 interface %s\n",ifname);
            exit(1);
        }
        ifb->addr.sin_addr.s_addr = bname?baddr.s_addr:((struct sockaddr_in *) ifp->ifa_broadaddr)->sin_addr.s_addr;
        ifb->laddr.sin_addr.s_addr=((struct sockaddr_in *)ifp->ifa_addr)->sin_addr.s_addr;
    }

    ifb->addr.sin_port=htons(port);
    if (ifa->direction != OUT)
        ifb->laddr.sin_port=htons(port);

    if ((ifb->fd=socket(AF_INET,SOCK_DGRAM,0)) < 0) {
           perror("Could not create UDP socket");
           exit (1);
     }

     if ((ifa->direction != IN) &&
        (setsockopt(ifb->fd,SOL_SOCKET,SO_BROADCAST,&on,sizeof(on)) < 0)) {
        perror("Setsockopt failed");
        exit(1);
    }

    if (setsockopt(ifb->fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on)) <0) {
        perror("setsockopt failed\n");
    }

    if (ifp)
        /* This won't work without root priviledges and may be system dependent
         * so let's silently ignore if it doesn't work */
        setsockopt(ifb->fd,SOL_SOCKET,SO_BINDTODEVICE,ifp->ifa_name,strlen(ifp->ifa_name));

    if (bind(ifb->fd,(const struct sockaddr *) &ifb->laddr,sizeof(ifb->laddr)) < 0) {
        perror("Bind failed");
        exit(1);
    }

    if (ifa->direction != IN) {
        /* Add the outgoing interface and broadcast port to the list that we
           need to ignore */
        if ((newig = (struct ignore_addr *) malloc(sizeof(struct ignore_addr)))
                == NULL) {
            perror("Could not allocate memory");
            exit(1);
        }
        newig->iaddr.sin_family = AF_INET;
        /* This is the *local* address and the *outgoing* port */
        newig->iaddr.sin_addr.s_addr = ifb->laddr.sin_addr.s_addr;
        newig->iaddr.sin_port = ifb->addr.sin_port;

        newig->next = NULL;

        /* find the end of the linked list, or a structure with the
         * ignore address already in it */
        for (igpp=&ignore;*igpp;igpp=&(*igpp)->next)
        /* DANGER! Only checks address, not port. Need to change this later
         * if port becomes significant */
            if ((*igpp)->iaddr.sin_addr.s_addr == 
                newig->iaddr.sin_addr.s_addr)
                break;

        /* Tack on new address if not a duplicate */
        if (*igpp == NULL)
            *igpp=newig;

        /* write queue initialization */
        if ((ifa->q = init_q(qsize)) == NULL) {
            perror("Could not create queue\n");
            exit(1);
        }
    }

    ifa->write=write_bcast;
    ifa->read=read_bcast;
    ifa->cleanup=cleanup_bcast;
    ifa->info = (void *) ifb;
    if (ifa->direction == BOTH) {
        if ((ifa->next=ifdup(ifa)) == NULL) {
            perror("Interface duplication failed");
            exit(1);
        }

        ifa->direction=OUT;
        ifa->pair->direction=IN;
        ifb = (struct if_bcast *) ifa->pair->info;
        ifb->laddr.sin_addr.s_addr=bname?baddr.s_addr:INADDR_ANY;
        ifb->laddr.sin_port=ifb->addr.sin_port;
        if (setsockopt(ifb->fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on)) <0) {
            perror("setsockopt failed 2\n");
        }
        /* As before, this may be system / privs dependent so let's not stress
         * if it doesn't work */
        setsockopt(ifb->fd,SOL_SOCKET,SO_BINDTODEVICE,ifp->ifa_name,strlen(ifp->ifa_name));

        if (bind(ifb->fd,(const struct sockaddr *) &ifb->laddr,sizeof(ifb->laddr)) < 0) {
            perror("Duplicate Bind failed");
            exit(1);
        }

    }
    free_options(ifa->options);
    return(ifa);
}

/* mcast.c
 * This file is part of kplex
 * Copyright Keith Young 2013
 * For copying information see the file COPYING distributed with this software
 *
 * Multicast interfaces
 */

#include "kplex.h"
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

#define DEFMCASTQSIZE 64

struct if_mcast {
    int fd;
    struct sockaddr_storage maddr;
    socklen_t asize;
    union {
        struct ip_mreqn ipmr;
        struct ipv6_mreq ip6mr;
    } mr;
};

/*
 * Duplicate multicast specific info
 * Args: if_mcast to be duplicated (cast to void *)
 * Returns: pointer to new if_mcast (cast to void *)
 */
void *ifdup_mcast(void *ifb)
{
    struct if_mcast  *oldif,*newif;

    oldif = (struct if_mcast *)ifb;

    if ((newif = (struct if_mcast *) malloc(sizeof(struct if_mcast)))
        == (struct if_mcast *) NULL)
        return(NULL);

    (void) memcpy(newif, oldif, sizeof(struct if_mcast));

    return((void *) newif);
}

void cleanup_mcast(iface_t *ifa)
{
    struct if_mcast *ifb = (struct if_mcast *) ifa->info;

    if (ifa->direction == IN) {
        if (ifb->maddr.ss_family == AF_INET) {
            if (setsockopt(ifb->fd,IPPROTO_IP,IP_DROP_MEMBERSHIP,
                    &ifb->mr.ipmr,sizeof(struct ip_mreqn)) < 0)
                logerr(errno,"IP_DROP_MEMBERSHIP failed");
        } else if (setsockopt(ifb->fd,IPPROTO_IPV6,IPV6_LEAVE_GROUP,
                    &ifb->mr.ip6mr,sizeof(struct ipv6_mreq)) < 0) {
                logerr(errno,"IPV6_LEAVE_GROUP failed");
        }
    }

    /* iomutex should be locked in the cleanup routine */
    if (!ifa->pair)
        close(ifb->fd);
}

void write_mcast(struct iface *ifa)
{
    struct if_mcast *ifb;
    senblk_t *sptr;
    int n;

    ifb = (struct if_mcast *) ifa->info;

    for (;;) {
        if ((sptr = next_senblk(ifa->q)) == NULL)
            break;

        if (senfilter(sptr,ifa->ofilter)) {
            senblk_free(sptr,ifa->q);
            continue;
        }

        if ((n=sendto(ifb->fd,sptr->data,sptr->len,0,
                (struct sockaddr *)&ifb->maddr,
                ifb->asize)) < 0)
            break;
        senblk_free(sptr,ifa->q);
    }
    iface_thread_exit(errno);
}

void read_mcast(struct iface *ifa)
{
    struct if_mcast *ifb;
    senblk_t sblk;
    char buf[BUFSIZ];
    char *bptr,*eptr,*senptr;
    int nread,cr=0,count=0,overrun=0,tagblock=0;
    struct sockaddr_storage src;
    socklen_t sz = (socklen_t) sizeof(src);
    ifb=(struct if_mcast *) ifa->info;

    senptr=sblk.data;
    sblk.src=ifa->id;

    while ((nread=recvfrom(ifb->fd,buf,BUFSIZ,0,(struct sockaddr *) &src,&sz))
                    > 0) {
        for(bptr=buf,eptr=buf+nread;bptr<eptr;bptr++) {
            if (tagblock) {
                if (*bptr == '\\') {
                    /* End of TAG block */
                    tagblock=0;
                }
                continue;
            }
            if (senptr == sblk.data) {
                /* first character */
                switch (*bptr) {
                case '\\':
                    tagblock=1;
                    continue;
                case '!':
                case '$':
                    break;
                default:
                    /* "overrun" now overloaded as an error flag */
                    overrun++;
                }
            }

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

/* Check whether an address is multicast
 * Args: pointer to struct sockaddr_storage
 * Returns: -1 if address family not INET or INET6
 *           0 if not a multicast address
 * 2 if an IPv6 link local multicast address
 * 3 if an IPv6 interface local multicast address
 * 1 otherwise
 */
int is_multicast(struct sockaddr *s)
{
    unsigned long addr;

    switch (s->sa_family) {
    case AF_INET:
        addr=ntohl(((struct sockaddr_in *) s)->sin_addr.s_addr);
        if ((addr & 0xff000000) == 0xe0000000)
            return(2);
        if ((addr & 0xf0000000) == 0xe0000000)
            return(1);
        return(0);
    case AF_INET6:
        if (((struct sockaddr_in6*) s)->sin6_addr.s6_addr[0] != 0xff)
            return(0);
        if ((((struct sockaddr_in6*) s)->sin6_addr.s6_addr[1] &  0x0f) == 2)
            return(2);
        if ((((struct sockaddr_in6*) s)->sin6_addr.s6_addr[1] &  0x0f) == 1)
            return(3);
        return(1);
    default:
        return(-1);
    }
}

struct iface *init_mcast(struct iface *ifa)
{
    struct if_mcast *ifm;
    char *ifname;
    struct addrinfo hints,*aptr,*abase;
    struct ifaddrs *ifap,*ifp;
    char *host,*service;
    struct servent *svent;
    size_t qsize = DEFMCASTQSIZE;
    struct kopts *opt;
    int ifindex,iffound=0;
    int linklocal=0;
    int on=1,off=0;
    int err;
    
    if ((ifm=malloc(sizeof(struct if_mcast))) == NULL) {
        logerr(errno,"Could not allocate memory");
        return(NULL);
    }
    memset(ifm,0,sizeof(struct if_mcast));

    ifname=host=service=NULL;

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"device"))
            ifname=opt->val;
        else if (!strcasecmp(opt->var,"group"))
            host=opt->val;
        else if (!strcasecmp(opt->var,"port"))
            service=opt->val;
        else if (!strcasecmp(opt->var,"qsize")) {
            if (!(qsize=atoi(opt->val))) {
                logerr(0,"Invalid queue size specified: %s",opt->val);
                return(NULL);
            }
        } else  {
            logerr(0,"Unknown interface option %s",opt->var);
            return(NULL);
        }
    }

    if (!host) {
        logerr(0,"Must specify multicast address for multicast interfaces");
        return(NULL);
    }

    if (!service) {
        if ((svent = getservbyname("nmea-0183","udp")) != NULL)
            service=svent->s_name;
        else
            service=DEFPORTSTRING;
    }

    memset((void *)&hints,0,sizeof(hints));

    hints.ai_flags=0;
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_DGRAM;
    hints.ai_protocol=IPPROTO_UDP;

    if ((err=getaddrinfo(host,service,&hints,&abase))) {
        logerr(0,"Lookup failed for address %s/service %s: %s",host,service,gai_strerror(err));
        return(NULL);
    }

    for (aptr=abase;aptr;aptr=aptr->ai_next)
        if (aptr->ai_family == AF_INET || aptr->ai_family  == AF_INET6)
            break;

    if (!aptr) {
        logerr(0,"No Suitable address found for %s/%s",host,service);
        freeaddrinfo(abase);
        return(NULL);
    }

    memcpy(&ifm->maddr,aptr->ai_addr,aptr->ai_addrlen);
    ifm->asize=aptr->ai_addrlen;

    freeaddrinfo(abase);

    if (ifm->maddr.ss_family == AF_INET) {
        memcpy(&ifm->mr.ipmr.imr_multiaddr,
                &((struct sockaddr_in*) &ifm->maddr)->sin_addr,
                sizeof(struct in_addr));
        ifm->mr.ipmr.imr_address.s_addr=INADDR_ANY;
    } else if (ifm->maddr.ss_family == AF_INET6) {
        memcpy(&ifm->mr.ip6mr.ipv6mr_multiaddr,
                &((struct sockaddr_in6 *)&ifm->maddr)->sin6_addr,
                sizeof(struct in6_addr));
    } else {
        logerr(0,"Unsupported address family %d\n",ifm->maddr.ss_family);
        return(NULL);
    }

    /* Now process local bind address */
    memset((void *)&hints,0,sizeof(hints));

    hints.ai_flags=AI_PASSIVE;
    hints.ai_family=ifm->maddr.ss_family;
    hints.ai_socktype=SOCK_DGRAM;
    hints.ai_protocol=IPPROTO_UDP;

    if ((err=getaddrinfo(NULL,service,&hints,&aptr))) {
        logerr(0,"Lookup failed for bind addresss: %s",gai_strerror(err));
        return(NULL);
    }

    for (aptr=abase;aptr;aptr=aptr->ai_next)
        if (aptr->ai_family == ifm->maddr.ss_family)
            break;

    if (!aptr) {
        logerr(0,"No suitable address found for %s/%s",host,service);
        freeaddrinfo(abase);
        return(NULL);
    }

    switch (is_multicast((struct sockaddr *)&ifm->maddr)) {
    case 0:
        logerr(0,"%s is not a multicast address",host);
        return(NULL);
    case 1:
        break;
    case 2:
    case 3:
        /* 3 is strictly speaking interface local... */
        linklocal++;
    }

    if ((ifm->fd=socket(ifm->maddr.ss_family,SOCK_DGRAM,IPPROTO_UDP)) < 0) {
        logerr(errno,"Could not create UDP socket");
        return(NULL);
     }

    if (ifname) {
        if (getifaddrs(&ifap) < 0) {
                logerr(errno,"Error getting interface info");
                return(NULL);
        }
        for (ifp=ifap;ifp;ifp=ifp->ifa_next) {
            if (ifname && strcmp(ifname,ifp->ifa_name))
                continue;
            iffound++;
            if (ifp->ifa_addr->sa_family == ifm->maddr.ss_family)
                break;
        }

        if (!ifp) {
            if (iffound)
                logerr(0,"Interface %s has no suitable local address",ifname);
            else if (ifname)
                logerr(0,"No interface %s found",ifname);
            return(NULL);
        }

        freeifaddrs(ifap);

        if ((ifindex=if_nametoindex(ifname)) == 0) {
            logerr(0,"Can't determine interface index for %s",ifname);
            return(NULL);
        }

        if (ifm->maddr.ss_family == AF_INET) {
            memcpy(&ifm->mr.ipmr.imr_address,
                    &((struct sockaddr_in *)aptr->ai_addr)->sin_addr,
                    sizeof(struct in_addr));
            ifm->mr.ipmr.imr_ifindex=ifindex;
        } else {
            ifm->mr.ip6mr.ipv6mr_interface=ifindex;
            if (linklocal)
               ((struct sockaddr_in6 *)&ifm->maddr)->sin6_scope_id=ifindex;
        }

        if (ifa->direction != IN) {
                if (ifm->maddr.ss_family==AF_INET) {
                    if (setsockopt(ifm->fd,IPPROTO_IP,IP_MULTICAST_IF,
                            &ifm->mr.ipmr,sizeof(struct ip_mreqn)) < 0) {
                        logerr(errno,"Failed to set multicast interface");
                        return(NULL);
                    }
                } else {
                    if (setsockopt(ifm->fd,IPPROTO_IPV6,IPV6_MULTICAST_IF,
                            &ifindex,sizeof(int)) < 0) {
                        logerr(errno,"Failed to set multicast interface");
                        return(NULL);
                    }
                }
        }
    } else {
        if (ifm->maddr.ss_family == AF_INET) {
            memcpy(&ifm->mr.ipmr.imr_address,
                    &((struct sockaddr_in *)aptr->ai_addr)->sin_addr,
                    sizeof(struct in_addr));
            ifm->mr.ipmr.imr_ifindex=0;
        } else {
            if (linklocal) {
                if (((struct sockaddr_in6 *)&ifm->maddr)->sin6_scope_id == 0) {
                    logerr(0,"Must specify a device with link local multicast addresses");
                    return(NULL);
                }
                ifm->mr.ip6mr.ipv6mr_interface = ((struct sockaddr_in6 *)
                        &ifm->maddr)->sin6_scope_id;
            } else {
                ifm->mr.ip6mr.ipv6mr_interface=0;
            }
        }
    }

    if (setsockopt(ifm->fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on)) < 0) {
        logerr(errno,"Failed to set SO_REUSEADDR");
        return(NULL);
    }

    if (ifa->direction != OUT) {
        if (ifm->maddr.ss_family==AF_INET) {
            if (setsockopt(ifm->fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&ifm->mr.ipmr,
                    sizeof(struct ip_mreqn)) < 0) {
                logerr(errno,"Failed to join multicast group %s",host);
                return(NULL);
            }
        } else {
            if (setsockopt(ifm->fd,IPPROTO_IPV6,IPV6_JOIN_GROUP,
                    &ifm->mr.ip6mr,sizeof(struct ipv6_mreq)) < 0) {
                logerr(errno,"Failed to join multicast group %s",host);
                return(NULL);
            }
        }
    }

    if (ifa->direction == IN) {
        if (bind(ifm->fd,aptr->ai_addr,aptr->ai_addrlen) < 0) {
            logerr(errno,"Bind failed");
            return(NULL);
        }
    }

    if (ifa->direction != IN) {
        /* write queue initialization */
        if ((ifa->q = init_q(qsize)) == NULL) {
            logerr(errno,"Could not create queue");
            return(NULL);
        }
    }

    ifa->write=write_mcast;
    ifa->read=read_mcast;
    ifa->cleanup=cleanup_mcast;
    ifa->info = (void *) ifm;
    if (ifa->direction == BOTH) {
        if (setsockopt(ifm->fd,
                (ifm->maddr.ss_family == AF_INET)?IPPROTO_IP:IPPROTO_IPV6,
                (ifm->maddr.ss_family == AF_INET)?
                IP_MULTICAST_LOOP:IPV6_MULTICAST_LOOP,&off,
                sizeof(off)) < 0) {
            logerr(errno,"Failed to disable multicast loopback\nDon't use bi-directional interfaces with loopback interface");
            return(NULL);
        }

        if ((ifa->next=ifdup(ifa)) == NULL) {
            logerr(0,"Interface duplication failed");
            return(NULL);
        }

        ifa->direction=OUT;
        ifa->pair->direction=IN;
        ifm = (struct if_mcast *) ifa->pair->info;
        if (bind(ifm->fd,aptr->ai_addr,aptr->ai_addrlen) < 0){
            logerr(errno,"Duplicate Bind failed");
            return(NULL);
        }

    }
    freeaddrinfo(abase);
    free_options(ifa->options);
    return(ifa);
}

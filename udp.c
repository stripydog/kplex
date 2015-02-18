/* udp.c
 * This file is part of kplex
 * Copyright Keith Young 2015
 * For copying information see the file COPYING distributed with this software
 *
 * UDP interfaces
 */

#include "kplex.h"
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

#define DEFUDPQSIZE 64

/* structures for list of (local outbound) addresses to ignore when receiving
 * this is our clunky way of ignoring our own broadcasts */

static struct ignore_addr {
    struct in_addr iaddr;
    unsigned short port;
    int refcnt;
    struct ignore_addr *next;
    struct ignore_addr *next_port;
} *ignore;

struct if_udp {
    int fd;
    enum udptype type;
    socklen_t asize;
    struct sockaddr_storage addr;
    union {
        struct ip_mreq ipmr;
        struct ipv6_mreq ip6mr;
    } mr;
    struct ignore_addr *ignore;
};

/*
 * Duplicate udp info
 * Args: if_udp to be duplicated (cast to void *)
 * Returns: pointer to new if_udp (cast to void *)
 */
void *ifdup_udp(void *ifa)
{
    struct if_udp  *oldif,*newif;

    oldif = (struct if_udp *)ifa;

    if ((newif = (struct if_udp *) malloc(sizeof(struct if_udp)))
        == (struct if_udp *) NULL)
        return(NULL);

    (void) memcpy(newif, oldif, sizeof(struct if_udp));

    /* Whole new file descriptor to bind() to.  Not an issue for Linux but
     * for some other platforms (e.g. OS X) we can't send with a multicast /
     * broadcast source address bound and can't receive with the interface
     * address bound */

     if ((newif->fd = socket(oldif->addr.ss_family,SOCK_DGRAM,IPPROTO_UDP)) <0){
        logwarn("Could not create duplicate socket: %s",strerror(errno));
        free(newif);
        return(NULL);
    }

    return((void *) newif);
}

void cleanup_udp(iface_t *ifa)
{
    struct if_udp *ifu = (struct if_udp *) ifa->info;

    if (ifu->type == UDP_MULTICAST && ifa->direction == IN) {
        if (ifu->addr.ss_family == AF_INET) {
            if (setsockopt(ifu->fd,IPPROTO_IP,IP_DROP_MEMBERSHIP,
                    &ifu->mr.ipmr,sizeof(struct ip_mreq)) < 0)
                logerr(errno,"IP_DROP_MEMBERSHIP failed");
        } else if (setsockopt(ifu->fd,IPPROTO_IPV6,IPV6_LEAVE_GROUP,
                    &ifu->mr.ip6mr,sizeof(struct ipv6_mreq)) < 0) {
                logerr(errno,"IPV6_LEAVE_GROUP failed");
        }
    }

    /* iomutex should be locked in the cleanup routine */
    close(ifu->fd);
}

void write_udp(struct iface *ifa)
{
    struct if_udp *ifu;
    senblk_t *sptr;
    int data=0;
    struct msghdr msgh;
    struct iovec iov[2];

    ifu = (struct if_udp *) ifa->info;
    msgh.msg_name=(void *)&ifu->addr;
    msgh.msg_namelen=ifu->asize;
    msgh.msg_control=NULL;
    msgh.msg_controllen=msgh.msg_flags=0;
    msgh.msg_iov=iov;
    msgh.msg_iovlen=1;

    if (ifa->tagflags) {
        if ((iov[0].iov_base=malloc(TAGMAX)) == NULL) {
                logerr(errno,"Disabing tag output on interface id %u (%s)",
                        ifa->id,(ifa->name)?ifa->name:"unlabelled");
                ifa->tagflags=0;
        } else {
            msgh.msg_iovlen=2;
            data=1;
        }
    }
    for (;;) {
        if ((sptr = next_senblk(ifa->q)) == NULL)
            break;

        if (senfilter(sptr,ifa->ofilter)) {
            senblk_free(sptr,ifa->q);
            continue;
        }

        if (ifa->tagflags)
            if ((iov[0].iov_len = gettag(ifa,iov[0].iov_base,sptr)) == 0) {
                logerr(errno,"Disabing tag output on interface id %u (%s)",
                        ifa->id,(ifa->name)?ifa->name:"unlabelled");
                ifa->tagflags=0;
                msgh.msg_iovlen=1;
                data=0;
                free(iov[0].iov_base);
            }

        iov[data].iov_base=sptr->data;
        iov[data].iov_len=sptr->len;

        if (sendmsg(ifu->fd,&msgh,0) < 0)
            break;
        senblk_free(sptr,ifa->q);
    }

    if (ifa->tagflags)
        free(iov[0].iov_base);

    iface_thread_exit(errno);
}

ssize_t read_udp(iface_t *ifa, char *buf)
{
    struct if_udp *ifu = (struct if_udp *) ifa->info;
    struct sockaddr_storage src;
    socklen_t sz = (socklen_t) sizeof(src);
    socklen_t insz = sizeof(struct sockaddr_in);
    ssize_t nread;
    struct ignore_addr *igp;

    do {
        nread = recvfrom(ifu->fd,(void *)buf,BUFSIZ,0,(struct sockaddr *) &src,&sz);
        if (ifu->ignore) {
            if (sz != insz) {
                sz=sizeof(src);
                continue;
            }
            for (igp=ifu->ignore;igp;igp=igp->next)
                if (igp->iaddr.s_addr == ((struct sockaddr_in *)&src)->sin_addr.s_addr)
                    break;
            if (igp)
                continue;
        }
        return (nread);
    } while(1);
}

/* Check whether an address is multicast
 * Args: pointer to struct sockaddr_storage
 * Returns: -1 if address family not INET or INET6
 *           0 if not a multicast address
 * 2 if an IPv6 link local multicast address
 * 3 if an IPv6 interface local multicast address
 * 1 otherwise
 */
static int is_multicast(struct sockaddr *s)
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

struct iface *init_udp(struct iface *ifa)
{
    struct if_udp *ifu;
    struct addrinfo hints,*aptr,*abase;
    struct ifaddrs *ifap=NULL,*ifp;
    char *address,*service,*ifname;
    struct servent *svent;
    size_t qsize = DEFUDPQSIZE;
    struct kopts *opt;
    int ifindex,iffound=0;
    int linklocal=0;
    int on=1,off=0;
    int err;
    struct sockaddr_storage laddr;
    struct sockaddr *sa;
    struct ignore_addr *igp,**igpp;
    
    if ((ifu=malloc(sizeof(struct if_udp))) == NULL) {
        logerr(errno,"Could not allocate memory");
        return(NULL);
    }
    memset(ifu,0,sizeof(struct if_udp));
    memset(&laddr,0,sizeof(struct sockaddr_storage));
    sa=(struct sockaddr *)&ifu->addr;

    ifname=address=service=NULL;

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"device"))
            ifname=opt->val;
        else if (!strcasecmp(opt->var,"address") ||
                !strcasecmp(opt->var,"group"))
            address=opt->val;
        else if (!strcasecmp(opt->var,"port"))
            service=opt->val;
        else if (!strcasecmp(opt->var,"qsize")) {
            if (!(qsize=atoi(opt->val))) {
                logerr(0,"Invalid queue size specified: %s",opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"mode")) {
            if (!strcasecmp(opt->val,"unicast"))
                ifu->type = UDP_UNICAST;
            else if (!strcasecmp(opt->val,"multicast"))
                ifu->type = UDP_MULTICAST;
            else if (!strcasecmp(opt->val,"broadcast"))
                ifu->type = UDP_BROADCAST;
            else {
                logerr(0,"Invalid UDP mode \'%s\'",opt->val);
                return(NULL);
            }
        } else  {
            logerr(0,"Unknown interface option %s",opt->var);
            return(NULL);
        }
    }

    if (!service) {
        if ((svent = getservbyname("nmea-0183","udp")) != NULL)
            service=svent->s_name;
        else
            service=DEFPORTSTRING;
    }
    memset((void *)&hints,0,sizeof(hints));

    hints.ai_flags=(ifa->direction==IN)?AI_PASSIVE:0;
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_DGRAM;
    hints.ai_protocol=IPPROTO_UDP;

    if ((err=getaddrinfo(address,service,&hints,&abase))) {
        logerr(0,"Lookup failed for address %s/service %s: %s",address,service,gai_strerror(err));
        return(NULL);
    }
    for (aptr=abase;aptr;aptr=aptr->ai_next) {
        if (aptr->ai_family == AF_INET || (aptr->ai_family  == AF_INET6 &&
                ifu->type != UDP_BROADCAST))
            break;
    }
    if (!aptr) {
        logerr(0,"No Suitable address found for %s/%s",address,service);
        freeaddrinfo(abase);
        return(NULL);
    }

    memcpy(sa,aptr->ai_addr,aptr->ai_addrlen);
    ifu->asize=aptr->ai_addrlen;
    freeaddrinfo(abase);

    if (address) {
        if (ifu->type == UDP_UNSPEC || ifu->type == UDP_MULTICAST) {
            switch (is_multicast(sa)) {
            case 0:
                if (ifu->type == UDP_MULTICAST) {
                    logerr(0,"%s is not a multicast address",address);
                    return(NULL);
                } else
                    break;
            case 2:
            case 3:
                /* 3 is strictly speaking interface local... */
                linklocal++;
            case 1:
                if (ifu->type == UDP_UNSPEC)
                    ifu->type = UDP_MULTICAST;
                if (ifu->addr.ss_family == AF_INET) {
                    memcpy(&ifu->mr.ipmr.imr_multiaddr,
                    &((struct sockaddr_in *)sa)->sin_addr,ifu->asize);
                } else {
                    memcpy(&ifu->mr.ip6mr.ipv6mr_multiaddr,
                    &((struct sockaddr_in6 *)sa)->sin6_addr,ifu->asize);
                }

                break;
            }
        }
        if (ifu->addr.ss_family == AF_INET &&
                (ifu->type == UDP_UNSPEC || ifu->type == UDP_BROADCAST )) {
            if (((struct sockaddr_in *)sa)->sin_addr.s_addr ==
                    INADDR_BROADCAST) {
                ifu->type = UDP_BROADCAST;
            } else {
                /* check if address belongs to a broadcast address */
                if (getifaddrs(&ifap) < 0) {
                    logerr(errno,"Error getting interface info");
                    return(NULL);
                }
                for (ifp=ifap;ifp;ifp=ifp->ifa_next)
                    if ((ifp->ifa_addr->sa_family == AF_INET) &&
                            (((struct sockaddr_in *)sa)->sin_addr.s_addr
                            == ((struct sockaddr_in *)(ifp->ifa_dstaddr))->sin_addr.s_addr))
                        break;
                if (ifp) {
                    if (ifname && strcmp(ifname,ifp->ifa_name)) {
                        logerr(0,"Broadcast address %s matches %s but %s specified",address,ifp->ifa_name,ifname);
                        freeifaddrs(ifap);
                        return(NULL);
                    }
                    memcpy(((struct sockaddr_in *)&laddr),ifp->ifa_addr,
                            sizeof(struct sockaddr_in));
                    if (ifp->ifa_flags & IFF_BROADCAST)
                        ifu->type = UDP_BROADCAST;
                    else
                        ifu->type = UDP_UNICAST;
                }
            }
        }
        if (ifu->type == UDP_UNSPEC)
            ifu->type = UDP_UNICAST;
    } else {
        /* No address specified */
        if (ifu->type == UDP_MULTICAST) {
            logerr(0,"Must specify an address for multicast interfaces");
            return(NULL);
        } else if (ifa->direction != IN && (ifname) && ifu->type != UDP_UNICAST)
            ifu->type = UDP_BROADCAST;
        else if (ifa->direction == IN) {
            if (!(ifu->type == UDP_UNSPEC || (ifname))) {
                logerr(0,"No address or interface name specified");
                return(NULL);
            }
        } else {
            logerr(0,"No address specified");
            return(NULL);
        }
    }

    if (ifname) {
        if ((!ifap) && (getifaddrs(&ifap) < 0)) {
                logerr(errno,"Error getting interface info");
                return(NULL);
        }
        for (ifp=ifap;ifp;ifp=ifp->ifa_next) {
            if (ifname && strcmp(ifname,ifp->ifa_name))
                continue;
            iffound++;
            if ((address == NULL && ((ifp->ifa_addr->sa_family == AF_INET) ||
                    (ifp->ifa_addr->sa_family == AF_INET6 &&
                    ifa->direction == IN))) ||
                    ifp->ifa_addr->sa_family == ifu->addr.ss_family)
                break;
        }

        if (!ifp) {
            if (iffound)
                logerr(0,"Interface %s has no suitable address",ifname);
            else if (ifname)
                logerr(0,"No interface %s found",ifname);
            freeifaddrs(ifap);
            return(NULL);
        }

        if (ifu->type == UDP_MULTICAST) {
            if (!(ifp->ifa_flags & IFF_MULTICAST)) {
                logerr(0,"Interface %s is not multicast capable",ifname);
                freeifaddrs(ifap);
                return(NULL);
            }

            if ((ifindex=if_nametoindex(ifname)) == 0) {
                logerr(0,"Can't determine interface index for %s",ifname);
                freeifaddrs(ifap);
                return(NULL);
            }
            if (ifa->direction != OUT) {
                if (ifu->addr.ss_family == AF_INET) {
                    memcpy(&ifu->mr.ipmr.imr_interface,
                            &((struct sockaddr_in *)ifp->ifa_addr)->sin_addr,
                            sizeof(struct in_addr));
                } else {
                    ifu->mr.ip6mr.ipv6mr_interface=ifindex;
                    if (linklocal)
                        ((struct sockaddr_in6 *)&ifu->addr)->sin6_scope_id=ifindex;
                }
            }
        } else if (ifu->type == UDP_BROADCAST) {
            if (!(ifp->ifa_flags & IFF_BROADCAST)) {
                if (ifp->ifa_dstaddr)
                    ifu->type = UDP_UNICAST;
                else if (!address) {
                    logerr(0,"Interface %s is not broadcast capable",ifname);
                    freeifaddrs(ifap);
                    return(NULL);
                }
            }
        }

        if (!address) {
            if (ifp->ifa_addr->sa_family == AF_INET)
                ifu->asize=sizeof(struct sockaddr_in);
            else
                ifu->asize=sizeof(struct sockaddr_in6);

            if (ifa->direction == IN && ifu->type != UDP_BROADCAST) {
                if ((sa->sa_family = ifp->ifa_addr->sa_family) == AF_INET)
                    ((struct sockaddr_in *)sa)->sin_addr.s_addr =
                            ((struct sockaddr_in *)ifp->ifa_addr)->sin_addr.s_addr;
                else
                    ((struct sockaddr_in6 *)sa)->sin6_addr =
                            ((struct sockaddr_in6 *)ifp->ifa_addr)->sin6_addr;
            } else {
                if (!(ifp->ifa_dstaddr)) {
                    logerr(0,"No output address specified for interface %s",
                            ifname);
                    freeifaddrs(ifap);
                    return(NULL);
                }
                if (ifu->type == UDP_UNSPEC)
                    ifu->type = (ifp->ifa_flags & IFF_BROADCAST)?
                            UDP_BROADCAST:UDP_UNICAST;
                else if (ifu->type == UDP_BROADCAST) {
                    if (!(ifp->ifa_flags & IFF_BROADCAST)) {
                        logerr(0,"Interface %s is not broadcast capable",
                                ifname);
                        freeifaddrs(ifap);
                        return(NULL);
                    }
                } else {
                    if (ifp->ifa_flags & IFF_BROADCAST) {
                        logerr(0,"Interface %s is not point to point and no address specified",ifname);
                        freeifaddrs(ifap);
                        return(NULL);
                    }
                }
                if ((sa->sa_family=ifp->ifa_dstaddr->sa_family) == AF_INET) {
                    ((struct sockaddr_in *)sa)->sin_addr.s_addr = 
                        ((struct sockaddr_in *)ifp->ifa_dstaddr)->sin_addr.s_addr;
                } else {
                    ((struct sockaddr_in6 *)sa)->sin6_addr = 
                        ((struct sockaddr_in6 *)ifp->ifa_dstaddr)->sin6_addr;
                }

                if (ifa->direction == BOTH && ifu->type == UDP_UNICAST) {
                    if ((laddr.ss_family=ifp->ifa_dstaddr->sa_family) ==
                            AF_INET) {
                        ((struct sockaddr_in *)&laddr)->sin_addr.s_addr = 
                            ((struct sockaddr_in *)ifp->ifa_dstaddr)->sin_addr.s_addr;
                    } else {
                        ((struct sockaddr_in6 *)&laddr)->sin6_addr = 
                            ((struct sockaddr_in6 *)ifp->ifa_dstaddr)->sin6_addr;
                    }
                } else if (ifu->type == UDP_BROADCAST && ifa->direction != IN) {
                    laddr.ss_family=ifp->ifa_dstaddr->sa_family;
                    ((struct sockaddr_in *)&laddr)->sin_addr.s_addr =
                        ((struct sockaddr_in *)ifp->ifa_addr)->sin_addr.s_addr;
                }
            }
        }

        freeifaddrs(ifap);
        ifap=NULL;

    } else {
        if (ifap)
            freeifaddrs(ifap);
        if (ifu->type == UDP_MULTICAST) {
            if (ifu->addr.ss_family == AF_INET) {
                ifu->mr.ipmr.imr_interface.s_addr=INADDR_ANY;
            } else if (ifu->addr.ss_family == AF_INET6) {
                if (linklocal) {
                    if (((struct sockaddr_in6 *)&ifu->addr)->sin6_scope_id == 0) {
                        logerr(0,"Must specify a device with link local multicast addresses");
                        return(NULL);
                    }
                    ifu->mr.ip6mr.ipv6mr_interface = ((struct sockaddr_in6 *)
                            &ifu->addr)->sin6_scope_id;
                } else {
                    ifu->mr.ip6mr.ipv6mr_interface=0;
                }
            }
        }
    }

    if (ifu->addr.ss_family == AF_UNSPEC) {
        logerr(0,"No address specified");
        return(NULL);
    }

    if ((ifu->fd=socket(ifu->addr.ss_family,SOCK_DGRAM,IPPROTO_UDP)) < 0) {
        logerr(errno,"Could not create UDP socket");
        if (ifap)
            freeifaddrs(ifap);
        return(NULL);
     }

    if (ifu->type == UDP_MULTICAST) {
        if (ifname && ifa->direction != IN) {
            if (ifu->addr.ss_family==AF_INET) {
                if (setsockopt(ifu->fd,IPPROTO_IP,IP_MULTICAST_IF,
                        &ifu->mr.ipmr.imr_interface,sizeof(ifu->mr.ipmr.imr_interface)) < 0) {
                    logerr(errno,"Failed to set multicast interface");
                    return(NULL);
                }
            } else if (ifu->addr.ss_family==AF_INET6) {
                if (setsockopt(ifu->fd,IPPROTO_IPV6,IPV6_MULTICAST_IF,
                        &ifindex,sizeof(int)) < 0) {
                    logerr(errno,"Failed to set multicast interface");
                    return(NULL);
                }
            }
        }
    }

    if (ifa->direction != IN) {
        if (ifu->type == UDP_BROADCAST) {
            if (setsockopt(ifu->fd,SOL_SOCKET,SO_BROADCAST,&on,sizeof(on)) < 0){
                logerr(errno,"Setsockopt failed");
                return(NULL);
            }
            for (igp=ignore,igpp=&ignore;igp;igpp=&igp->next_port,igp=igp->next_port)
                if (igp->port == ((struct sockaddr_in *)&ifu->addr)->sin_port)
                    break;

            if (igp) {
                ifu->ignore = igp;
                for (;igp;igpp=&igp->next,igp=igp->next)
                    if (igp->iaddr.s_addr ==
                            ((struct sockaddr_in *)&laddr)->sin_addr.s_addr)
                        break;
            }

            if (igp)
                igp->refcnt++;
            else {
                if ((*igpp=(struct ignore_addr *)
                        malloc(sizeof(struct ignore_addr))) == NULL) {
                    logerr(errno,"Could not allocate memory");
                    return(NULL);
                }
                (*igpp)->port=((struct sockaddr_in *)&ifu->addr)->sin_port;
                (*igpp)->iaddr.s_addr=
                        ((struct sockaddr_in *)&laddr)->sin_addr.s_addr;
                (*igpp)->next=NULL;
                (*igpp)->next_port=NULL;
                if (ifu->ignore == NULL)
                    ifu->ignore = *igpp;
            }
        }

        /* write queue initialization */
        if ((ifa->q = init_q(qsize)) == NULL) {
            logerr(errno,"Could not create queue");
            return(NULL);
        }
    }

    ifa->write=write_udp;
    ifa->read=do_read;
    ifa->readbuf=read_udp;
    ifa->cleanup=cleanup_udp;
    ifa->info = (void *) ifu;
    if (ifa->direction == BOTH) {
        if (ifu->type == UDP_MULTICAST) {
            if (setsockopt(ifu->fd,
                    (ifu->addr.ss_family == AF_INET)?IPPROTO_IP:IPPROTO_IPV6,
                    (ifu->addr.ss_family == AF_INET)?
                    IP_MULTICAST_LOOP:IPV6_MULTICAST_LOOP,&off,
                    sizeof(off)) < 0) {
                logerr(errno,"Failed to disable multicast loopback\nDon't use bi-directional interfaces with loopback interface");
                return(NULL);
            }
        }

        if ((ifa->next=ifdup(ifa)) == NULL) {
            logerr(0,"Interface duplication failed");
            return(NULL);
        }

        ifa->direction=OUT;
        ifa->pair->direction=IN;
        ifu = (struct if_udp *) ifa->pair->info;
        if (ifu->type == UDP_UNICAST) {
            if (laddr.ss_family == AF_UNSPEC) {
                laddr.ss_family=ifu->addr.ss_family;
                if (ifu->addr.ss_family == AF_INET) {
                    ((struct sockaddr_in *)&laddr)->sin_addr.s_addr
                            =INADDR_ANY;
                } else
                    ((struct sockaddr_in6 *)&laddr)->sin6_addr=in6addr_any;
            }
            sa=(struct sockaddr *)&laddr;
        }
    }

    if (ifa->direction == IN || (ifa->pair != NULL)) {
        if (setsockopt(ifu->fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on)) < 0) {
            logerr(errno,"Failed to set SO_REUSEADDR");
            return(NULL);
        }

#ifdef SO_REUSEPORT
        if (ifu->type != UDP_UNICAST)
            if (setsockopt(ifu->fd,SOL_SOCKET,SO_REUSEPORT,&on,sizeof(on)) < 0){
                logerr(errno,"Failed to set SO_REUSEPORT");
                return(NULL);
            }
#endif
        if (ifu->type == UDP_MULTICAST) {
            if (ifu->addr.ss_family==AF_INET) {
                if (setsockopt(ifu->fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,
                    &ifu->mr.ipmr, sizeof(struct ip_mreq)) < 0) {
                    logerr(errno,"Failed to join multicast group %s",address);
                    return(NULL);
                }
            } else {
                if (setsockopt(ifu->fd,IPPROTO_IPV6,IPV6_JOIN_GROUP,
                        &ifu->mr.ip6mr,sizeof(struct ipv6_mreq)) < 0) {
                    logerr(errno,"Failed to join multicast group %s",address);
                    return(NULL);
                }
            }
        }

        if (bind(ifu->fd,sa,ifu->asize) < 0) {
            logerr(errno,"Duplicate bind failed");
            return(NULL);
        }
    }

    free_options(ifa->options);
    return(ifa);
}

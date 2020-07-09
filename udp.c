/* udp.c
 * This file is part of kplex
 * Copyright Keith Young 2015 - 2019
 * For copying information see the file COPYING distributed with this software
 *
 * UDP interfaces
 */

#include "kplex.h"
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

#define CBUFSIZ 128

static struct ignore_addr {
    struct sockaddr_in iaddr;
    unsigned short port;
    int refcnt;
    int writers;
    struct ignore_addr *next;
} *ignore;

struct coalesce {
    size_t offset;
    size_t seqid;
    char buf[CBUFSIZ];
};

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
    struct coalesce *coalesce;
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

    /* In-bound connections don't need pointer to coalesce buffer */
    newif->coalesce = NULL;

    /* Whole new file descriptor to bind() to.  Not an issue for Linux but
     * for some other platforms (e.g. OS X) we can't send with a multicast /
     * broadcast source address bound and can't receive with the interface
     * address bound */

     if ((newif->fd = socket(oldif->addr.ss_family,SOCK_DGRAM,IPPROTO_UDP)) <0){
        logwarn(catgets(cat,11,1,"Could not create duplicate socket: %s"),
                strerror(errno));
        free(newif);
        return(NULL);
    }

    return((void *) newif);
}

void cleanup_udp(iface_t *ifa)
{
    struct if_udp *ifu = (struct if_udp *) ifa->info;
    struct ignore_addr *igp;

    if (ifu->type == UDP_MULTICAST && ifa->direction == IN) {
        if (ifu->addr.ss_family == AF_INET) {
            if (setsockopt(ifu->fd,IPPROTO_IP,IP_DROP_MEMBERSHIP,
                    &ifu->mr.ipmr,sizeof(struct ip_mreq)) < 0)
                logerr(errno,catgets(cat,11,2,"IP_DROP_MEMBERSHIP failed"));
        } else if (setsockopt(ifu->fd,IPPROTO_IPV6,IPV6_LEAVE_GROUP,
                    &ifu->mr.ip6mr,sizeof(struct ipv6_mreq)) < 0) {
                logerr(errno,catgets(cat,11,3,"IPV6_LEAVE_GROUP failed"));
        }
    } else if (ifu->ignore) {
        /* Broadcast Interface: OK to do this stuff with iomutex locked */
        if (--ifu->ignore->refcnt == 0) {
            if (ifu->ignore == ignore)
                ignore=ifu->ignore->next;
            else
                for (igp=ignore;igp;igp=igp->next)
                    if (igp == ifu->ignore)
                        if (igp->next == ifu->ignore)
                            igp->next = ifu->ignore->next;
            free(ifu->ignore);
        } else if (ifa->direction == OUT)
            ifu->ignore->writers--;
    }

    if (ifu->coalesce)
        free(ifu->coalesce);

    /* iomutex should be locked in the cleanup routine */
    close(ifu->fd);
}

int is_ais(char *sptr,size_t len,size_t *nfrag, size_t *frag, unsigned int *seq)
{
    int i;

    if (len < 13)
        return(0);

    if (!(sptr[3] == 'V' && sptr[4] == 'D' &&
            (sptr[5] == 'M' || sptr[5] == 'O')))
        return(0);
    sptr+=6;

    if ((*sptr++) != ',')
        return(0);

    for (i=7,*nfrag=0;i <= len && *sptr >= '0' && *sptr <= '9';sptr++,i++)
        *nfrag = *nfrag*10+*sptr-'0';

    if (*sptr++ != ',' || i > len)
        return(0);

    for (*frag=0;i <= len && *sptr >= '0' && *sptr <= '9';sptr++,i++)
        *frag = *frag*10+*sptr-'0';

    if (*sptr++ != ',' || i > len)
        return(0);

    for (*seq=0;i <= len && *sptr >= '0' && *sptr <= '9';sptr++,i++)
        *seq = *seq*10+*sptr-'0';

    if (*sptr != ',' || i > len)
        return(0);

    return(1);
}

int coalesce(struct if_udp *ifu, struct msghdr * mh)
{
    size_t nfrags,frag;
    unsigned int seqid;
    struct iovec *ioptr = mh->msg_iov;
    int data = mh->msg_iovlen-1;
    size_t len;
    int i;
    struct coalesce *cp = ifu->coalesce;

    if (!(is_ais(ioptr[data].iov_base,ioptr[data].iov_len,
            &nfrags,&frag,&seqid)))
        return(0);

    if (nfrags == 1 && cp->offset == 0)
        return(0);

    for (i=0;i<mh->msg_iovlen;i++)
        len=ioptr[i].iov_len;

    if ((cp->offset + len) > CBUFSIZ || ((cp->offset) && (cp->seqid != seqid) &&
            frag < nfrags)) {
        sendto(ifu->fd,cp->buf,cp->offset,0,
               (struct sockaddr *)&ifu->addr,ifu->asize);
        cp->offset=0;
    }

    if (data) {
        memcpy(cp->buf+cp->offset,mh->msg_iov->iov_base,mh->msg_iov->iov_len);
        cp->offset += mh->msg_iov->iov_len;
    }
    memcpy(ifu->coalesce->buf+ifu->coalesce->offset,
        mh->msg_iov[data].iov_base,mh->msg_iov[data].iov_len);
    cp->offset += mh->msg_iov[data].iov_len;

    if (frag == nfrags) {
        sendto(ifu->fd,cp->buf,cp->offset,0,
                (struct sockaddr *)&ifu->addr,ifu->asize);
        cp->offset=0;
    } else
        cp->seqid=seqid;

    return (1);;
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
                logerr(errno,catgets(cat,11,4,"%s: Disabing tag output"),
                        ifa->name);
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
                logerr(errno,catgets(cat,11,4,"%s: Disabing tag output"),
                        ifa->name);
                ifa->tagflags=0;
                msgh.msg_iovlen=1;
                data=0;
                free(iov[0].iov_base);
            }

        iov[data].iov_base=sptr->data;
        iov[data].iov_len=sptr->len;

        if (ifu->coalesce) {
            if (coalesce(ifu,&msgh)) {
                senblk_free(sptr,ifa->q);
                continue;
            }
        }

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
    ssize_t nread;
    struct iovec iov;
    struct msghdr mh;

    iov.iov_base = buf;
    iov.iov_len = BUFSIZ;

    mh.msg_name = &src;
    mh.msg_namelen = (socklen_t) sizeof(src);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = NULL;
    mh.msg_controllen = 0;
    mh.msg_flags = 0;

    do {
        nread = recvmsg(ifu->fd,&mh,0);

        if (ifu->ignore && ifu->ignore->writers) {
        /* Broadcast Interface: IPv4 */
            if (memcmp((void *)&src,(void *)&ifu->ignore->iaddr,(size_t) mh.msg_namelen)
                    == 0) 
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
    char *address,*service,*ifname,*eptr;
    struct servent *svent;
    size_t qsize = DEFQSIZE;
    struct kopts *opt;
    int coalesce=0;
    int ifindex,iffound=0;
    int linklocal=0;
    int on=1,off=0;
    int err;
    int port;
    struct sockaddr_storage laddr;
    struct sockaddr *sa;
    struct ignore_addr *igp;
    char debugbuf[INET6_ADDRSTRLEN];
    
    if ((ifu=malloc(sizeof(struct if_udp))) == NULL) {
        logerr(errno,catgets(cat,11,5,"Could not allocate memory"));
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
        else if (!strcasecmp(opt->var,"coalesce")) {
            if (!strcasecmp(opt->val,"ais") || !strcasecmp(opt->val,"yes"))
                coalesce=1;
            else if (!strcasecmp(opt->val,"no"))
                coalesce=0;
            else
                logerr(0,catgets(cat,11,6,
                        "Unrecognized value for coalesce: %s"),opt->val);
        } else if (!strcasecmp(opt->var,"qsize")) {
            if (!(qsize=atoi(opt->val))) {
                logerr(0,catgets(cat,11,7,"Invalid queue size specified: %s"),
                        opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"type")) {
            if (!strcasecmp(opt->val,"unicast"))
                ifu->type = UDP_UNICAST;
            else if (!strcasecmp(opt->val,"multicast"))
                ifu->type = UDP_MULTICAST;
            else if (!strcasecmp(opt->val,"broadcast"))
                ifu->type = UDP_BROADCAST;
            else {
                logerr(0,catgets(cat,11,8,"Invalid UDP mode \'%s\'"),opt->val);
                return(NULL);
            }
        } else  {
            logerr(0,catgets(cat,11,9,"Unknown interface option %s"),opt->var);
            return(NULL);
        }
    }

    if (!service) {
        if ((svent = getservbyname("nmea-0183","udp")) != NULL) {
            service=svent->s_name;
            port=svent->s_port;
        } else {
            service=DEFPORTSTRING;
            port=DEFPORT;
        }
    } else if (!address) {
        if ((port=strtol(service,&eptr,0)) <= 0 || port >= 65536 || *eptr != '\0') {
            if ((svent = getservbyname(service,"udp")) != NULL)
                port=svent->s_port;
            else
                port=DEFPORT;
        }
    }

    if (address || ifa->direction == IN) {
        memset((void *)&hints,0,sizeof(hints));

        hints.ai_flags=(ifa->direction==IN)?AI_PASSIVE:0;

        if (ifu->type == UDP_BROADCAST)
            hints.ai_family=AF_INET;
        else
            hints.ai_family=AF_UNSPEC;
        hints.ai_socktype=SOCK_DGRAM;
        hints.ai_protocol=IPPROTO_UDP;

        if ((err=getaddrinfo(address,service,&hints,&abase))) {
            logerr(0,catgets(cat,11,10,
                    "Lookup failed for address %s/service %s: %s"),address,
                    service,gai_strerror(err));
            return(NULL);
        }
        for (aptr=abase;aptr;aptr=aptr->ai_next) {
            if (aptr->ai_family == AF_INET || (aptr->ai_family  == AF_INET6 &&
                    ifu->type != UDP_BROADCAST))
                break;
        }

        if (!aptr) {
            logerr(0,catgets(cat,11,11,"No Suitable address found for %s/%s"),
                    address,service);
            freeaddrinfo(abase);
            return(NULL);
        }

        memcpy(sa,aptr->ai_addr,aptr->ai_addrlen);
        ifu->asize=aptr->ai_addrlen;
        freeaddrinfo(abase);
    }

    if (address) {

        if (ifu->type == UDP_UNSPEC || ifu->type == UDP_MULTICAST) {
            switch (is_multicast(sa)) {
            case 0:
                if (ifu->type == UDP_MULTICAST) {
                    logerr(0,catgets(cat,11,12,"%s is not a multicast address"),
                            address);
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
                laddr.ss_family=AF_INET;
                ((struct sockaddr_in *)&laddr)->sin_addr.s_addr = INADDR_ANY;
            } else {
                /* check if address belongs to a broadcast address */
                if (getifaddrs(&ifap) < 0) {
                    logerr(errno,"Error getting interface info");
                    return(NULL);
                }
                for (ifp=ifap;ifp;ifp=ifp->ifa_next)
                    /* Note that the definition of ifa_dstaddr varies by system
                       but the usage below should work on all target platforms */
                    if ((ifp->ifa_addr) && (ifp->ifa_addr->sa_family == AF_INET) &&
                            (ifp->ifa_dstaddr != NULL) &&
                            (((struct sockaddr_in *)sa)->sin_addr.s_addr
                            == ((struct sockaddr_in *)(ifp->ifa_dstaddr))->sin_addr.s_addr))
                        break;
                if (ifp) {
                    if (ifname && strcmp(ifname,ifp->ifa_name)) {
                        logerr(0,catgets(cat,11,14,
                                "Broadcast address %s matches %s but %s specified"),
                                address,ifp->ifa_name,ifname);
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
            logerr(0,catgets(cat,11,15,
                    "Must specify an address for multicast interfaces"));
            return(NULL);
        } else if (ifa->direction != IN && (ifname) && ifu->type != UDP_UNICAST)
            ifu->type = UDP_BROADCAST;
        else if (ifa->direction == IN) {
            if (!(ifu->type == UDP_UNSPEC || (ifname))) {
                logerr(0,catgets(cat,11,16,
                        "No address or interface name specified"));
                return(NULL);
            }
        } else {
            logerr(0,catgets(cat,11,17,"No address specified"));
            return(NULL);
        }
    }

    if (ifname) {
        if ((!ifap) && (getifaddrs(&ifap) < 0)) {
                logerr(errno,catgets(cat,11,18,"Error getting interface info"));
                return(NULL);
        }
        for (ifp=ifap;ifp;ifp=ifp->ifa_next) {
            if (ifname && strcmp(ifname,ifp->ifa_name))
                continue;
            iffound++;
            if (ifp->ifa_addr == NULL)
                continue;
            if (ifp->ifa_addr->sa_family != AF_INET &&
                    ifp->ifa_addr->sa_family != AF_INET6)
                continue;
            if ((address == NULL && (ifp->ifa_dstaddr != NULL)) ||
                    ((ifp->ifa_addr->sa_family == ifu->addr.ss_family)  &&
                    (ifa->direction == IN)) || ((address != NULL) &&
                    ifp->ifa_addr->sa_family == ifu->addr.ss_family))
                break;
        }

        if (!ifp) {
            if (iffound)
                logerr(0,catgets(cat,11,19,
                        "Interface %s has no suitable address"),ifname);
            else if (ifname)
                logerr(0,catgets(cat,11,20,"No interface %s found"),ifname);
            freeifaddrs(ifap);
            return(NULL);
        }

        if (ifu->type == UDP_MULTICAST) {
            if (!(ifp->ifa_flags & IFF_MULTICAST)) {
                logerr(0,catgets(cat,11,21,
                        "Interface %s is not multicast capable"),ifname);
                freeifaddrs(ifap);
                return(NULL);
            }

            if ((ifindex=if_nametoindex(ifname)) == 0) {
                logerr(0,catgets(cat,11,22,
                        "Can't determine interface index for %s"),ifname);
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
                    logerr(0,catgets(cat,11,23,
                            "Interface %s is not broadcast capable"),ifname);
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

            if (ifa->direction != IN) {
                if (!(ifp->ifa_dstaddr)) {
                    logerr(0,catgets(cat,11,24,
                            "No output address specified for interface %s"),
                            ifname);
                    freeifaddrs(ifap);
                    return(NULL);
                }
                if (ifu->type == UDP_UNSPEC)
                    ifu->type = (ifp->ifa_flags & IFF_BROADCAST)?
                            UDP_BROADCAST:UDP_UNICAST;
                else if (ifu->type == UDP_BROADCAST) {
                    if (!(ifp->ifa_flags & IFF_BROADCAST)) {
                        logerr(0,catgets(cat,11,23,
                                "Interface %s is not broadcast capable"),
                                ifname);
                        freeifaddrs(ifap);
                        return(NULL);
                    }
                } else {
                    if (ifp->ifa_flags & IFF_BROADCAST) {
                        logerr(0,catgets(cat,11,25,
                                "Interface %s is not point to point and no address specified"),
                                ifname);
                        freeifaddrs(ifap);
                        return(NULL);
                    }
                }
                if ((sa->sa_family=ifp->ifa_dstaddr->sa_family) == AF_INET) {
                    ((struct sockaddr_in *)sa)->sin_addr.s_addr = 
                        ((struct sockaddr_in *)ifp->ifa_dstaddr)->sin_addr.s_addr;
                    ((struct sockaddr_in *)sa)->sin_port = htons(port);
                } else {
                    ((struct sockaddr_in6 *)sa)->sin6_addr = 
                        ((struct sockaddr_in6 *)ifp->ifa_dstaddr)->sin6_addr;
                    ((struct sockaddr_in6 *)sa)->sin6_port = htons(port);
                }

                if (ifu->type == UDP_BROADCAST || ifa->direction == BOTH) {
                    if ((laddr.ss_family=ifp->ifa_dstaddr->sa_family) ==
                            AF_INET) {
                        ((struct sockaddr_in *)&laddr)->sin_addr.s_addr = 
                            ((struct sockaddr_in *)ifp->ifa_addr)->sin_addr.s_addr;
                    } else {
                        ((struct sockaddr_in6 *)&laddr)->sin6_addr = 
                            ((struct sockaddr_in6 *)ifp->ifa_addr)->sin6_addr;
                    }
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
                        logerr(0,catgets(cat,11,26,
                                "Must specify a device with link local multicast addresses"));
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
        logerr(0,catgets(cat,11,17,"No address specified"));
        return(NULL);
    }

    if ((ifu->fd=socket(ifu->addr.ss_family,SOCK_DGRAM,IPPROTO_UDP)) < 0) {
        logerr(errno,catgets(cat,11,27,"Could not create UDP socket"));
        if (ifap)
            freeifaddrs(ifap);
        return(NULL);
     }

    if (ifu->type == UDP_MULTICAST) {
        if (ifname && ifa->direction != IN) {
            if (ifu->addr.ss_family==AF_INET) {
                if (setsockopt(ifu->fd,IPPROTO_IP,IP_MULTICAST_IF,
                        &ifu->mr.ipmr.imr_interface,sizeof(ifu->mr.ipmr.imr_interface)) < 0) {
                    logerr(errno,catgets(cat,11,28,
                            "Failed to set multicast interface"));
                    return(NULL);
                }
            } else if (ifu->addr.ss_family==AF_INET6) {
                if (setsockopt(ifu->fd,IPPROTO_IPV6,IPV6_MULTICAST_IF,
                        &ifindex,sizeof(int)) < 0) {
                    logerr(errno,catgets(cat,11,28,
                            "Failed to set multicast interface"));
                    return(NULL);
                }
            }
        }
    } else if (ifu->type == UDP_BROADCAST) {
        for (igp=ignore;igp;igp=igp->next)
            if (igp->iaddr.sin_port == ((struct sockaddr_in *)sa)->sin_port &&
                    igp->iaddr.sin_addr.s_addr ==
                    ((struct sockaddr_in *)sa)->sin_addr.s_addr)
                break;

        if (igp == NULL) {
            if ((igp=(struct ignore_addr *)malloc(sizeof(struct ignore_addr)))
                    < 0) {
                logerr(errno,catgets(cat,11,5,"Could not allocate memory"));
                return(NULL);
            }
            igp->iaddr.sin_port = ((struct sockaddr_in *)sa)->sin_port;
            igp->iaddr.sin_addr.s_addr =
                    ((struct sockaddr_in *)sa)->sin_addr.s_addr;
            igp->refcnt=igp->writers=0;
            igp->next=(ignore)?ignore->next:NULL;

            ignore=igp;
        }

        if ((igp->refcnt+=(ifa->direction == BOTH)?2:1)<0) {
            logerr(0,catgets(cat,11,29,"Max broadcast interfaces exceeded"));
            return(NULL);
        }

        if (ifa->direction != IN)
            igp->writers++;

        ifu->ignore=igp;
    }

    if (ifa->direction != IN) {
        if (ifu->type == UDP_BROADCAST) {
            if (setsockopt(ifu->fd,SOL_SOCKET,SO_BROADCAST,&on,sizeof(on)) < 0){
                logerr(errno,catgets(cat,11,30,"Setsockopt failed"));
                return(NULL);
            }
        }

        /* write queue initialization */
        if (init_q(ifa, qsize) < 0) {
            logerr(errno,catgets(cat,11,31,"Could not create queue"));
            return(NULL);
        }
        if (coalesce) {
            if ((ifu->coalesce=
                    (struct coalesce *)malloc(sizeof(struct coalesce))) == NULL) {
                logerr(errno,catgets(cat,11,5,"Could not allocate memory"));
                return(NULL);
            }
            ifu->coalesce->offset=ifu->coalesce->seqid=0;
        }
    }

    /* Set interface.  This is platform specific and is generally a privileged
     * operation so we'll silently ignore failures and if it works bonus!
     */
#ifdef SO_BINDTODEVICE
    /* Linux: requires root privileges */
    if (ifname) {
        /* Is it a struct ifreq?  Is it a string? is it strlen + 1?  Seems
        the length parameter tends to be ignored so if it's null terminated
        and starts at the address pointed to we're fine */
        if (setsockopt(ifu->fd,SOL_SOCKET,SO_BINDTODEVICE,ifname,
                strlen(ifname)) == 0) {
            DEBUG2(3,catgets(cat,11,32,"%s: BINDTODEVICE failed on device %s"),
                   ifa->name,ifname);
        } else {
            DEBUG(3,catgets(cat,11,33,
                    "%s: BINDTODEVICE succeeded on device %s"),ifa->name,
                    ifname);
        }
    }
#endif

    if (ifa->direction != IN) {
        DEBUG(3,catgets(cat,11,34,"%s: output address %s, port %d"),
                ifa->name,inet_ntop(ifu->addr.ss_family,
                ((ifu->addr.ss_family == AF_INET)?
                (void*)&((struct sockaddr_in *)&ifu->addr)->sin_addr:
                (void*)&((struct sockaddr_in6*)&ifu->addr)->sin6_addr),debugbuf,
                INET6_ADDRSTRLEN),ntohs((ifu->addr.ss_family == AF_INET)?
                ((struct sockaddr_in*)&ifu->addr)->sin_port:
                ((struct sockaddr_in6*)&ifu->addr)->sin6_port));
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
                logerr(errno,catgets(cat,11,35,
                        "Failed to disable multicast loopback\nDon't use bi-directional interfaces with loopback interface"));
                return(NULL);
            }
        }

        if ((ifa->next=ifdup(ifa)) == NULL) {
            logerr(0,catgets(cat,11,36,"Interface duplication failed"));
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
        /* Platform-specific interface binding for read side */
#ifdef SO_BINDTODEVICE
        /* Linux */
        if (ifname) {
            if (setsockopt(ifu->fd,SOL_SOCKET,SO_BINDTODEVICE,ifname,
                    strlen(ifname)) == 0) {
                DEBUG2(3,catgets(cat,11,37,
                        "%s: BINDTODEVICE failed (read) to device %s"),
                        ifa->name,ifname);
            } else {
                DEBUG(3,catgets(cat,11,38,
                    "%s: BINDTODEVICE succeeded (read) to device %s"),
                    ifa->name,ifname);
            }
        }
#endif

    }

    if (ifa->direction == IN || (ifa->pair != NULL)) {
        if (setsockopt(ifu->fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on)) < 0) {
            logerr(errno,catgets(cat,11,39,"Failed to set SO_REUSEADDR"));
            return(NULL);
        }

#ifdef SO_REUSEPORT
        if (ifu->type != UDP_UNICAST)
            if (setsockopt(ifu->fd,SOL_SOCKET,SO_REUSEPORT,&on,sizeof(on)) < 0){
                logerr(errno,catgets(cat,11,40,"Failed to set SO_REUSEPORT"));
                return(NULL);
            }
#endif
        if (ifu->type == UDP_MULTICAST) {
            if (ifu->addr.ss_family==AF_INET) {
                if (setsockopt(ifu->fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,
                    &ifu->mr.ipmr, sizeof(struct ip_mreq)) < 0) {
                    logerr(errno,catgets(cat,11,41,
                            "Failed to join multicast group %s"),address);
                    return(NULL);
                }
            } else {
                if (setsockopt(ifu->fd,IPPROTO_IPV6,IPV6_JOIN_GROUP,
                        &ifu->mr.ip6mr,sizeof(struct ipv6_mreq)) < 0) {
                    logerr(errno,catgets(cat,11,41,
                            "Failed to join multicast group %s"),address);
                    return(NULL);
                }
            }
        }
        if (bind(ifu->fd,sa,ifu->asize) < 0) {
            logerr(errno,catgets(cat,11,42,"bind failed for udp interface %s"),
                    ifa->name);
            return(NULL);
        }
        DEBUG(3,catgets(cat,11,43,"udp interface %s listening on %s, port %d"),
                ifa->name,inet_ntop(sa->sa_family,((sa->sa_family == AF_INET)?
                (void*)&((struct sockaddr_in*)sa)->sin_addr:
                (void*)&((struct sockaddr_in6*)sa)->sin6_addr),debugbuf,
                INET6_ADDRSTRLEN),
                ntohs((sa->sa_family==AF_INET)?
                ((struct sockaddr_in*)sa)->sin_port:
                ((struct sockaddr_in6*)sa)->sin6_port));
    }

    free_options(ifa->options);
    return(ifa);
}

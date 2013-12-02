#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <signal.h>
#include <net/if.h>

#include "kplex.h"
#include "tcp.h"

#define GOFREE_PORT 2052
#define GOFREE_GROUP "239.2.1.1"
#define RECVBUFSZ 1480

struct if_gofree {
    int fd;
    size_t qsize;
    struct ip_mreqn ipmr;
};

struct gofree_mfd {
    char *name;
    struct sockaddr_in addr;
    time_t lastseen;
};

    
void cleanup_gofree(iface_t *ifa)
{
    struct if_gofree *ifg=(struct if_gofree *)ifa->info;
    close(ifg->fd);
}

iface_t *new_gofree_conn(pthread_t *tid, struct gofree_mfd *mfd, iface_t *ifa)
{
    iface_t *newifa;
    struct if_tcp *newift;
    struct if_gofree *ifg = (struct if_gofree *) ifa->info;
    int err;
    sigset_t set,saved;

    if ((newifa = malloc(sizeof(iface_t))) == NULL)
        return(NULL);
    memset(newifa,0,sizeof(iface_t));

    if (((newift = (struct if_tcp *) malloc(sizeof(struct if_tcp))) == NULL) ||
            ((ifa->direction != IN) &&
            ((newifa->q=init_q(ifg->qsize)) == NULL))) {
        if (newifa && newifa->q)
            free(newifa->q);
        if (newift)
            free(newift);
        free(newifa);
        return(NULL);
    }

    if (((newift->fd=socket(PF_INET,SOCK_STREAM,0)) < 0) || \
            (connect(newift->fd,(struct sockaddr *)&mfd->addr,sizeof(struct sockaddr)) != 0)) {
        err=errno;
        free(newifa->q);
        free(newift);
        free(newifa);
        errno=err;
        return(NULL);
    }
    newift->shared=NULL;
    newifa->id=ifa->id+(newift->fd&IDMINORMASK);
    newifa->direction=IN;
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
    newifa->q=ifa->lists->engine->q;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, &saved);
    link_to_initialized(newifa);
    pthread_create(tid,NULL,(void *)start_interface,(void *) newifa);
    pthread_sigmask(SIG_SETMASK,&saved,NULL);

    return(newifa);
}

void *ifdup_gofree(void *ifa)
{
    return NULL;
}

char *next_json_val(char **pptr)
{
    char *ptr;
    char *val;
    int comma=0;
    char delim=0;

    for (ptr=(*pptr);*ptr;ptr++) {
        switch (*ptr) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            continue;
        case '"':
            delim='"';
            break;
        case '[':
            delim=']';
            break;
        case '{':
            delim='}';
            break;
        default:
            break;
        }
        break;
    }
    if (*ptr == '\0' || *ptr==','){
        if (*ptr) {
            ++ptr;
        }
        *pptr=ptr;
        return(NULL);
    }
    val=ptr++;
    if (delim) {
        for (;*ptr;ptr++) {
            if (*ptr == delim) {
                break;
            }
        }
        if (*ptr) {
            *ptr++='\0';
        } else
            return(NULL);
    }
        
    else {
        for(;*ptr;ptr++){
            switch (*ptr){
            case ',':
                comma++;
            case ' ':
            case '\t':
            case '\n':
                *ptr++='\0';
                break;
            default:
                continue;
            }
            break;
        }
    }

    if (!comma)
        while (*ptr)
            if (*ptr++ == ',')
                break;
    *pptr=ptr;
    return(val);
}

char *next_json_key(char **pptr)
{
    char *ptr;
    char *key;

    for (ptr=(*pptr);*ptr;ptr++) {
        switch (*ptr) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            continue;
        case '"':
            break;
        default:
            return NULL;
        }
        break;
    }

    key=++ptr;
    for (;*ptr;ptr++) {
        if (*ptr == '"') {
            *ptr++='\0';
            break;
        }
    }

    while (*ptr)
        if (*ptr++ == ':')
            break;

    if (*ptr) {
        *pptr=ptr;
        return key;
    }
    return(NULL);
}

char *get_next_json_elem(char **pptr)
{
    char *ptr=*pptr;
    char *elem;
    while (*ptr)
        if (*ptr++ == '{')
            break;
    if (*ptr == '\0')
        return(NULL);

    for (elem=ptr;*ptr;ptr++)
        if (*ptr == '}') {
            *ptr++ = '\0';
            *pptr=ptr;
            return(elem);
        }
    return(NULL);
}

int parse_json(struct gofree_mfd *mfd, char *buf, size_t len)
{
    char *ptr,*key,*val,*elem;
    int gotaddr=0,gotport=0,thisservice;

    buf[len-1]='\0';

    for (ptr=buf;*ptr;) {
        if (*ptr++ == '{') {
            break;
        }
    }

    if (*ptr == '\0')
        return(-1);

    mfd->lastseen=time(NULL);

    while (!(gotaddr && gotport)) {
        if ((key = next_json_key(&ptr)) == NULL)
            return(-1);
        if ((val = next_json_val(&ptr)) == NULL)
            return(-1);
        if (strcmp("IP",key) == 0) {
            if (inet_pton(AF_INET,++val,(void *) &mfd->addr.sin_addr) != 1)
                return(-1);
            else
                gotaddr++;
        } else if (strcmp("Services",key) == 0) {
            if (*val++ != '[')
                return(-1);
            while ((elem = get_next_json_elem(&val)) != NULL) {
                thisservice=0;
                for (;;) {
                    if ((key = next_json_key(&elem)) == NULL)
                        break;
                    if ((val = next_json_val(&elem)) == NULL)
                        return(-1);
                    if (strcmp(key,"Service") == 0) {
                        if (strcmp(val,"\"nmea-0183") == 0)
                            thisservice=1;
                    } else if (strcmp(key,"Port") == 0) {
                        mfd->addr.sin_port=htons(atoi(val));
                    }
                }
                if (thisservice) {
                    if (mfd->addr.sin_port == 0) {
                        return(-1);
                    }
                    gotport++;
                    break;
                }
            }
        }
    }

    return(0);
}

void gofree_server (iface_t *ifa)
{
    struct if_gofree *ifg=(struct if_gofree *)ifa->info;
    char msgbuf[RECVBUFSZ];
    struct gofree_mfd currmfd,newmfd;
    pthread_t tid;
    int isConnected=0;
    int newconn;
    size_t len;
    struct sockaddr sa;
    socklen_t sl;

    currmfd.lastseen = 0;
    currmfd.addr.sin_addr.s_addr=INADDR_ANY;

    while (ifa->direction != NONE) {
        sl=sizeof(struct sockaddr);
        if ((len=recvfrom(ifg->fd,msgbuf,RECVBUFSZ,0,&sa,&sl)) < 0) {
            logerr(errno,"Receive failed");
            break;
        }
        if (parse_json(&newmfd,msgbuf,len) != 0)
            continue;

        newconn=0;

        if (newmfd.addr.sin_addr.s_addr == currmfd.addr.sin_addr.s_addr) {
            if (newmfd.addr.sin_port != currmfd.addr.sin_port) {
                newconn=1;
                currmfd.addr.sin_port=newmfd.addr.sin_port;
            }
            currmfd.lastseen=newmfd.lastseen;
        } else if ((newmfd.lastseen - currmfd.lastseen)  > 2) {
            newconn = 1;
            memcpy(&currmfd,&newmfd,sizeof(struct gofree_mfd));
        }
        if (!newconn) {
            if ((isConnected) && (pthread_kill(tid,0) == 0))
                continue;
        } else if (isConnected) {
            pthread_kill(tid,SIGUSR1);
            pthread_join(tid,NULL);
        }
        /* create new tcp connection */
        isConnected = 0;
        newmfd.addr.sin_family=AF_INET;
        if (new_gofree_conn(&tid,&newmfd,ifa) != NULL)
            isConnected++;
    }
}

iface_t *init_gofree(iface_t *ifa)
{
    struct if_gofree *ifg;
    char *ifname=NULL;
    struct ifaddrs *ifap,*ifp;
    struct sockaddr_in maddr;
    int ifindex,iffound=0;
    int on=1;
    struct kopts *opt;


    if ((ifg = malloc(sizeof(struct if_gofree))) == NULL) {
        logerr(errno,"Could not allocate memory");
        return(NULL);
    }

    ifg->qsize=DEFTCPQSIZE;

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"device"))
            ifname=opt->val;
        else if (!strcasecmp(opt->var,"qsize")) {
            if (!(ifg->qsize=atoi(opt->val))) {
                logerr(0,"Invalid queue size specified: %s",opt->val);
                return(NULL);
            }
        } else  {
            logerr(0,"unknown interface option %s\n",opt->var);
            return(NULL);
        }
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
            if (ifp->ifa_addr->sa_family == AF_INET)
                break;
        }

        if (!ifp) {
            if (iffound)
                logerr(0,"Interface %s has no suitable local address",ifname);
            else if (ifname)
                logerr(0,"No interface %s found",ifname);
            return(NULL);
        }

        if ((ifindex=if_nametoindex(ifname)) == 0) {
            logerr(0,"Can't determine interface index for %s",ifname);
            return(NULL);
        }

        memcpy(&ifg->ipmr.imr_address,
                &((struct sockaddr_in *)ifp->ifa_addr)->sin_addr,
                sizeof(struct in_addr));
        ifg->ipmr.imr_ifindex=ifindex;

        freeifaddrs(ifap);
    } else {
        ifg->ipmr.imr_address.s_addr=INADDR_ANY;
        ifg->ipmr.imr_ifindex=0;
    }

    if ((ifg->fd = socket(PF_INET,SOCK_DGRAM,0)) < 0) {
        logerr(errno,"Could not create UDP socket");
        return(NULL);
     }
        
    if (setsockopt(ifg->fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on)) < 0) {
        logerr(errno,"Failed to set SO_REUSEADDR");
        return(NULL);
    }

    maddr.sin_family=AF_INET;
    maddr.sin_port=htons(GOFREE_PORT);
    inet_pton(AF_INET,GOFREE_GROUP,&maddr.sin_addr);

    memcpy(&ifg->ipmr.imr_multiaddr,&maddr.sin_addr,sizeof(struct in_addr));

    if (setsockopt(ifg->fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&ifg->ipmr,
            sizeof(struct ip_mreqn)) < 0) {
        logerr(errno,"Failed to join multicast group %s",GOFREE_GROUP);
        return(NULL);
    }

    if (bind(ifg->fd,(struct sockaddr *)&maddr,sizeof(struct sockaddr_in)) < 0) {
        logerr(errno,"Bind failed");
        return(NULL);
    }

    ifa->cleanup=cleanup_gofree;
    ifa->info=(void *) ifg;
    ifa->write=gofree_server;
    ifa->read=gofree_server;
    free_options(ifa->options);
    return(ifa);
}

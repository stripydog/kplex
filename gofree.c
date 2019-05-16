/* gofree.c
 * This file is part of kplex
 * Copyright Keith Young 2013-2019
 *  For copying information see the file COPYING distributed with this softwar`
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <signal.h>
#include <net/if.h>

#include "kplex.h"
#include "tcp.h"    /* Included for spawned tcp interfaces */

#define GOFREE_PORT 2052
#define GOFREE_GROUP "239.2.1.1"
/* Asked Navico about the max size of a discovery packet.  Initially they
 * told me 64kB.  However, not only would this involve fragmentation without
 * jumbo frames support it would break their own C# example. Below is a waste
 * of space but would capture max size of a UDP packet over IPv4 with MTU 1500
 */
#define RECVBUFSZ 1472

/* the ip_mreq is needed to drop group membership when the interface exits */
struct if_gofree {
    int fd;
    struct ip_mreq ipmr;
};

/* We don't really have a use for name except for debugging. In this release
 * it is not referenced at all
 */
struct gofree_mfd {
    char *name;
    struct sockaddr_in addr;
    time_t lastseen;
};

void cleanup_gofree(iface_t *ifa)
{
    struct if_gofree *ifg=(struct if_gofree *)ifa->info;

    /* Drop group membership from the interface, closed fd and exit */

    if (setsockopt(ifg->fd,IPPROTO_IP,IP_DROP_MEMBERSHIP,&ifg->ipmr,
            sizeof(struct ip_mreq)) < 0)
        logerr(errno,catgets(cat,5,1,"IP_DROP_MEMBERSHIP failed"));

    close(ifg->fd);
}

/* Create a new TCP connection to a gofree MFD and a thread to handle it
 * Args: pointer to thread id (to be filled in), pointer to mfd structure and
 *     pointer to the gofree interface spawning this connection
 * Returns: pointer to the new tcp interface structure on success, NULL on
 *     failure
 * Side Effects: pthread_t pointed to by tid is filled in with new thread's id
 */
iface_t *new_gofree_conn(pthread_t *tid, struct gofree_mfd *mfd, iface_t *ifa)
{
    iface_t *newifa;
    struct if_tcp *newift;
    int err;
    sigset_t set,saved;
    char addrbuf[INET_ADDRSTRLEN];   /* for debug info */

    if ((newifa = malloc(sizeof(iface_t))) == NULL)
        return(NULL);
    memset(newifa,0,sizeof(iface_t));

    if ((newift = (struct if_tcp *) malloc(sizeof(struct if_tcp))) == NULL) {
        free(newifa);
        return(NULL);
    }

    if (((newift->fd=socket(PF_INET,SOCK_STREAM,0)) < 0) || \
            (connect(newift->fd,(struct sockaddr *)&mfd->addr,sizeof(struct sockaddr)) != 0)) {
        /* Save errno so it isn't set to 0 by free */
        err=errno;
        free(newift);
        free(newifa);
        errno=err;
        return(NULL);
    }
    newift->shared=NULL;
    newifa->id=ifa->id+(newift->fd&IDMINORMASK);
    newifa->direction=IN;
    newifa->type=TCP;
    newifa->name=ifa->name;
    newifa->info=newift;
    newifa->cleanup=cleanup_tcp;
    newifa->write=write_tcp;
    newifa->read=do_read;
    newifa->tagflags=ifa->tagflags;
    newifa->readbuf=read_tcp;
    newifa->lists=ifa->lists;
    newifa->ifilter=addfilter(ifa->ifilter);
    /* Copying ofilter is unnecessary as gofree is input only */
    newifa->checksum=ifa->checksum;
    newifa->q=ifa->lists->engine->q;
    /* disable SIGUSR1 before launching new thread to avoid it being killed
     * while holding a mutex */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, &saved);
    link_to_initialized(newifa);
    pthread_create(tid,NULL,(void *)start_interface,(void *) newifa);

    /* reset sig mask and re-enable SIGUSR1 */
    pthread_sigmask(SIG_SETMASK,&saved,NULL);
    DEBUG(3,catgets(cat,5,2,"%s: connected to MFD %s at %s port %s"),ifa->name,
            mfd->name,inet_ntop(AF_INET,(const void *)&mfd->addr.sin_addr,
            addrbuf,INET_ADDRSTRLEN),ntohs(mfd->addr.sin_port));

    return(newifa);
}

/* Actually we don't do any dup-ing: gofree is one-way */
void *ifdup_gofree(void *ifa)
{
    return NULL;
}

/* Return pointer to next value in a JSON key : val pair
 * Args: pointer to pointer to buffer pointing to JSON string just after a
 *     divider for key : val
 * Returns: Pointer to value. NULL on error
 * Side Effects: \0 inserted after value, pointer pointed at is advanced to
 *     before the next key
 */
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

/* Retrieve next key from json key : value pair
 * Args:  pointer to pointer pointing at just before the next json key
 * Returns: pointer to beginning of next JSON key
 * Side effects: Inserts \0 after next json key and advances pointer pointer
 * to point to the subsequent character
 */
char *next_json_key(char **pptr)
{
    char *ptr;
    char *key;

    for (ptr=(*pptr);;ptr++) {
        switch (*ptr) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            continue;
        case '"':
            break;
        default:
            *pptr=ptr;
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

/* Get the next JSON element bounded by "{" and "}"
 * Args: Pointer to pointer to before element
 * Returns: Pointer to just after next '{', NULL on error (no "{" found or
 *    no closing '}' found)
 * Side Effects: closing '}' is replaced by \0, pointer pointed to by arg
        advanced to point at character beyond
 */
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

/* Take MFD location information from a GoFree tier 1 announcement
 * Args: pointer to MFD structure, pointer to character buffer containing JSON
 *    string, size of string
 * Returns: 0 on success, -1 if no information extracted from JSON string
 * Side effects: mfd structure is filled in with IP and lastseen time.  JSON
 * string is chopped up with NULLs so no longer usable
 */
int parse_json(struct gofree_mfd *mfd, char *buf, size_t len)
{
    char *ptr,*key,*val,*eval,*elem;
    int gotaddr=0,gotport=0,thisservice;

    /* This *should* be the terminating '}' or subsequent whitespace */
    buf[len-1]='\0';

    /* Seek to opening '{' */
    for (ptr=buf;*ptr;) {
        if (*ptr++ == '{') {
            break;
        }
    }

    if (*ptr == '\0')
        return(-1);

    mfd->lastseen=time(NULL);

    /* Loop through keys and values until we find IP and port info */
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
                    if ((eval = next_json_val(&elem)) == NULL)
                        return(-1);
                    if (strcmp(key,"Service") == 0) {
                        if (strcmp(eval,"\"nmea-0183") == 0)
                            thisservice=1;
                    } else if (strcmp(key,"Port") == 0) {
                        mfd->addr.sin_port=htons(atoi(eval));
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

/* Main gofree server routine: listens for multicast service announcements
 * launches and kills TCP connections accordingly
 */
void gofree_server (iface_t *ifa)
{
    struct if_gofree *ifg=(struct if_gofree *)ifa->info;
    char msgbuf[RECVBUFSZ];
    struct gofree_mfd currmfd,newmfd;
    pthread_t tid;
    int isConnected=0;
    int newconn;
    ssize_t len;
    struct sockaddr sa;
    socklen_t sl;

    currmfd.lastseen = 0;
    currmfd.addr.sin_addr.s_addr=INADDR_ANY;

    /* Here's how this works.  Each time we receive a JSON string we try and
     * parse it for nmea-0183 service information. If we find it we compare
     * the address with that of any already connected MFD */
    while (ifa->direction != NONE) {
        sl=sizeof(struct sockaddr);
        if ((len=recvfrom(ifg->fd,msgbuf,RECVBUFSZ,0,&sa,&sl)) < 0) {
            logerr(errno,catgets(cat,5,3,"Receive failed"));
            break;
        }
        if (parse_json(&newmfd,msgbuf,len) != 0)
            continue;

        if (isConnected != 0) {
            newconn=0;

            if ((newmfd.addr.sin_addr.s_addr != currmfd.addr.sin_addr.s_addr) ||
                    (newmfd.addr.sin_port != currmfd.addr.sin_port)) {
                if (newmfd.lastseen - currmfd.lastseen >2) {
                    newconn=1;
                }
            }
            if (!newconn) {
                if (pthread_kill(tid,0) == 0)
                    /* Connection thread is still running */
                    continue;
            } else {
                /* Connected but new connection required */
                pthread_kill(tid,SIGUSR1);
                pthread_join(tid,NULL);
            }
        }
        /* create new tcp connection */
        newmfd.addr.sin_family=AF_INET;
        isConnected = (new_gofree_conn(&tid,&newmfd,ifa) == NULL)?0:1;
        memcpy(&currmfd,&newmfd,sizeof(struct gofree_mfd));
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

    if (ifa->direction == OUT) {
        logerr(0,catgets(cat,5,4,
                "gofree interfaces must be \"in\" (the default) only"));
        return(NULL);
    }

    if (ifa->direction == BOTH)
        ifa->direction=IN;

    if ((ifg = malloc(sizeof(struct if_gofree))) == NULL) {
        logerr(errno,catgets(cat,5,5,"Could not allocate memory"));
        return(NULL);
    }

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"device")) {
            ifname=opt->val;
        } else  {
            logerr(0,catgets(cat,5,6,"unknown interface option %s\n"),opt->var);
            return(NULL);
        }
    }

    if (ifname) {
        if (getifaddrs(&ifap) < 0) {
                logerr(errno,catgets(cat,5,7,"Error getting interface info"));
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
                logerr(0,catgets(cat,5,8,
                        "Interface %s has no suitable local address"),ifname);
            else if (ifname)
                logerr(0,catgets(cat,5,9,"No interface %s found"),ifname);
            return(NULL);
        }

        if ((ifindex=if_nametoindex(ifname)) == 0) {
            logerr(0,catgets(cat,5,10,"Can't determine interface index for %s"),
                    ifname);
            return(NULL);
        }

        memcpy(&ifg->ipmr.imr_interface,
                &((struct sockaddr_in *)ifp->ifa_addr)->sin_addr,
                sizeof(struct in_addr));

        freeifaddrs(ifap);
    } else {
        ifg->ipmr.imr_interface.s_addr=INADDR_ANY;
    }

    if ((ifg->fd = socket(PF_INET,SOCK_DGRAM,0)) < 0) {
        logerr(errno,catgets(cat,5,11,"Could not create UDP socket"));
        return(NULL);
     }
        
    if (setsockopt(ifg->fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on)) < 0) {
        logerr(errno,catgets(cat,5,12,"Failed to set SO_REUSEADDR"));
        return(NULL);
    }

    maddr.sin_family=AF_INET;
    maddr.sin_port=htons(GOFREE_PORT);
    inet_pton(AF_INET,GOFREE_GROUP,&maddr.sin_addr);

    memcpy(&ifg->ipmr.imr_multiaddr,&maddr.sin_addr,sizeof(struct in_addr));

    if (setsockopt(ifg->fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&ifg->ipmr,
            sizeof(struct ip_mreq)) < 0) {
        logerr(errno,catgets(cat,5,13,"Failed to join multicast group %s"),
                GOFREE_GROUP);
        return(NULL);
    }

    if (bind(ifg->fd,(struct sockaddr *)&maddr,sizeof(struct sockaddr_in)) < 0) {
        logerr(errno,catgets(cat,5,14,"Bind failed"));
        return(NULL);
    }

    DEBUG(3,catgets(cat,5,15,"%s listening on %s for gofree to %s port %d"),
            ifa->name,(ifname)?ifname:catgets(cat,5,16,"default"),GOFREE_GROUP,
            GOFREE_PORT);

    ifa->cleanup=cleanup_gofree;
    ifa->info=(void *) ifg;
    ifa->write=gofree_server;
    ifa->read=gofree_server;
    free_options(ifa->options);
    return(ifa);
}

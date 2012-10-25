#include "kplex.h"
#include <netdb.h>

#define DEFTCPQSIZE 128

struct if_tcp {
    int fd;
};

void cleanup_tcp(iface_t *ifa)
{
    struct if_tcp *ift = (struct if_tcp *)ifa->info;

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
	fd=ift->fd;

	while ((nread=read(fd,buf,BUFSIZ)) > 0) {
fprintf(stderr,"DEBUG: read %d chars\n",nread); fflush(stderr);
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

    if (((newifa = malloc(sizeof(iface_t))) == NULL) ||
        ((newift = malloc(sizeof(struct if_tcp))) == NULL) ||
        ((newifa->q=init_q(DEFTCPQSIZE)) == NULL)) {
            if (newifa && newifa->q)
                free(newifa->q);
            if (newift)
                free(newift);
            free(newifa);
            return(NULL);
    }

    newift->fd=fd;
    newifa->direction=OUT;
    newifa->type=TCP;
    newifa->info=newift;
    newifa->cleanup=cleanup_tcp;
    newifa->write=write_tcp;
    newifa->lists=ifa->lists;
    link_interface(newifa);

    pthread_create(&tid,NULL,(void *)do_output,(void *) newifa);
    return(newifa);
}

iface_t *tcp_server(iface_t *ifa)
{
    struct if_tcp *ift=(struct if_tcp *)ifa->info;
    int afd;

    ifa->tid = pthread_self();
    
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

    if ((ift = malloc(sizeof(struct if_tcp))) == NULL) {
        perror("Could not allocate memory");
        exit(1);
    }

    if (host=strtok(str+4,":")) {
        if (!strcmp(host,"-")) {
            host=NULL;
        }
        port=strtok(NULL,":");
    }

    if (port == NULL) {
        if ((svent=getservbyname("nmea-0183","tcp")) != NULL)
            port=svent->s_name;
        else
            port = DEFTCPPORT;
    }

    memset((void *)&hints,0,sizeof(hints));

    hints.ai_flags=AI_CANONNAME|(ifa->direction==OUT)?AI_PASSIVE:0;
    hints.ai_family=AF_UNSPEC;
    hints.ai_socktype=SOCK_STREAM;

    if (err=getaddrinfo(host,port,&hints,&aptr)) {
        fprintf(stderr,"Lookup failed for host %s/service %s: %s\n",host,port,gai_strerror(err));
        exit(1);
    }

    do {
        if ((ift->fd=socket(aptr->ai_family,aptr->ai_socktype,aptr->ai_protocol)) < 0)
            continue;
        if (ifa->direction == IN) {
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
        fprintf(stderr,"Failed to open %s tcp port for %s/%s\n",(ifa->direction == OUT)?"outbound":"inbound",host,port);
        exit(1);
    }

    ifa->read=read_tcp;
    ifa->write=tcp_server;
    ifa->cleanup=cleanup_tcp;
    ifa->info = (void *) ift;
    ifa->q=NULL;
    return(ifa);
}

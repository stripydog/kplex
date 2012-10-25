#include "kplex.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <pty.h>
#include <limits.h>

#define DEFSERIALQSIZE 128

struct if_serial {
    int fd;
    struct termios otermios;
};

void cleanup_serial(iface_t *ifa)
{
    struct if_serial *ifs = (struct if_serial *)ifa->info;

	if (tcsetattr(ifs->fd,TCSAFLUSH,&ifs->otermios) < 0) {
		perror("Warning: Failed to restore serial line");
	}
}

int ttyopen(char *device, enum iotype direction)
{
	int dev;
	struct stat sbuf;

	if (stat(device,&sbuf) < 0) {
		fprintf(stderr,"Could not stat %s: %s\n",device,strerror(errno));
		return(-1);
	}

	if (!S_ISCHR(sbuf.st_mode)){
		fprintf(stderr,"%s is not a character device\n",device);
		return(-1);
	}

	if ((dev=open(device,O_RDWR|O_NOCTTY)) < 0) {
		fprintf(stderr,"failed to open %s: %s\n",device,strerror(errno));
		return(-1);
	}

	return(dev);
}

int ttysetup(int dev,struct termios *otermios_p, tcflag_t cflag)
{
	struct termios ttermios,ntermios;

	if (tcgetattr(dev,otermios_p) < 0) {
		perror("failed to get terminal attributes");
		return (-1);
	}

	memset(&ntermios,0,sizeof(struct termios));

	ntermios.c_cflag=cflag;
	ntermios.c_iflag=IGNBRK|INPCK;
	ntermios.c_cc[VMIN]=1;
	ntermios.c_cc[VTIME]=0;

	if (tcflush(dev,TCIOFLUSH) < 0)
		perror("Warning: Failed to flush serial device");

	if (tcsetattr(dev,TCSAFLUSH,&ntermios) < 0) {
		perror("Failed to set up serial line!");
		return(-1);
	}

	if (tcgetattr(dev,&ttermios) < 0) {
		perror("Failed to re-read serial line attributes");
		return(-1);
	}

	if ((ttermios.c_cflag != ntermios.c_cflag) ||
		(ttermios.c_iflag != ntermios.c_iflag)) {
fprintf(stderr,"cflag:actual  = %u, requested = %u\niflag:actual  = %u, requested = %u\n",ttermios.c_cflag,ntermios.c_cflag,ttermios.c_iflag,ntermios.c_iflag);
		fprintf(stderr,"Failed to correctly set up serial line");
		return(-1);
	}

	return(0);
}

struct iface * read_serial(struct iface *ifa)
{
	char buf[BUFSIZ];
	char *bptr,*eptr=buf+BUFSIZ,*senptr;
	senblk_t sblk;
	struct if_serial *ifs = (struct if_serial *) ifa->info;
	int nread,cr=0,count=0,overrun=0;
	int fd;

	senptr=sblk.data;
	fd=ifs->fd;

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

struct iface * write_serial(struct iface *ifa)
{
	struct if_serial *ifs = (struct if_serial *) ifa->info;
	senblk_t *senblk_p;
	int fd=ifs->fd;
	int n=0;
	char *ptr;

	while(n >= 0) {
		if ((senblk_p = next_senblk(ifa->q)) == NULL)
			break;
		ptr=senblk_p->data;
		while(senblk_p->len) {
			if ((n=write(fd,ptr,senblk_p->len)) < 0)
				break;
			senblk_p->len -=n;
			ptr+=n;
		}
		senblk_free(senblk_p,ifa->q);
	}
	iface_destroy(ifa,(void *) &errno);
}

struct iface *init_serial (char *str, struct iface *ifa)
{
	char *devname,*option;
	struct if_serial *ifs;
	int baud=B4800;
	tcflag_t cflag;

	if ((devname=strtok(str+4,":")) == NULL) {
		fprintf(stderr,"Bad specification for serial device: \'%s\'\n",
			str);
		exit(1);
	}

	if ((option = strtok(NULL,":")) != NULL) {
		if (!strcmp(option,"384000"))
			baud=B38400;
		else if (!strcmp(option,"9600"))
			baud=B9600;
		else if (!strcmp(option,"4800"))
			baud=B4800;
		else {
			fprintf(stderr,"Unsupported baud rate \'%s\' in interface specification '\%s\'\n",option,devname);
			exit(1);
		}
	}

	cflag=baud|CS8|CLOCAL|PARENB|((ifa->direction == OUT)?0:CREAD);

	if ((ifs = malloc(sizeof(struct if_serial))) == NULL) {
		perror("Could not allocate memory\n");
		exit(1);
	}

	if ((ifs->fd=ttyopen(devname,ifa->direction)) < 0) {
		exit (1);
	}

	if (ttysetup(ifs->fd,&ifs->otermios,cflag) < 0)
		exit(1);


    if (ifa->direction == OUT)
        if ((ifa->q =init_q(DEFSERIALQSIZE)) == NULL) {
            perror("Could not create queue");
            exit(1);
        }
    ifa->read=read_serial;
    ifa->write=write_serial;
    ifa->cleanup=cleanup_serial;
	ifa->info=(void *)ifs;
	return(ifa);
}

struct iface *init_pty (char *str, struct iface *ifa)
{
	char *devname,*option;
	struct if_serial *ifs;
	int baud=B4800,slavefd;
	tcflag_t cflag;
    char slave[PATH_MAX];
    
	if ((devname=strtok(str+4,":")) == NULL) {
		fprintf(stderr,"Bad specification for serial device: \'%s\'\n",
			str);
		exit(1);
	}

	if ((option = strtok(NULL,":")) != NULL) {
		if (!strcmp(option,"384000"))
			baud=B38400;
		else if (!strcmp(option,"9600"))
			baud=B9600;
		else if (!strcmp(option,"4800"))
			baud=B4800;
		else {
			fprintf(stderr,"Unsupported baud rate \'%s\' in interface specification '\%s\'\n",option,devname);
			exit(1);
		}
	}

	cflag=baud|CS8|CLOCAL|CREAD;

	if ((ifs = malloc(sizeof(struct if_serial))) == NULL) {
		perror("Could not allocate memory\n");
		exit(1);
	}

    if (ifa->direction == OUT) {
	    if (openpty(&ifs->fd,&slavefd,slave,NULL,NULL) < 0) {
            perror("error opening pty");
		    exit (1);
    	}

        printf("Slave pty for output at %s baud is %s\n",(baud==B4800)?"4800":
                                                        (baud==B9600)?"9600":
                                                        "38.4k",slave);

        if ((ifa->q =init_q(DEFSERIALQSIZE)) == NULL) {
            perror("Could not create queue");
            exit(1);
        }
    } else
	    if ((ifs->fd=ttyopen(devname,ifa->direction)) < 0) {
	    	exit (1);
    	}

	if (ttysetup(ifs->fd,&ifs->otermios,cflag) < 0)
		exit(1);

    ifa->read=read_serial;
    ifa->write=write_serial;
    ifa->cleanup=cleanup_serial;
	ifa->info=(void *)ifs;
	return(ifa);
}

/* seatalk.c
 * This file is part of kplex
 * Copyright Keith Young 2012
 * For copying information see the file COPYING distributed with this software
 *
 * This file efines the seatalk interface to kplex.  This is based on
 * reverse engineering of the seatalk 1 protocol by Thomas Knauf and others
 * documented at http://www.thomasknauf.de/seatalk.htm
 * This code is experimental and likely to be very unreliable
 * Writing seatalk data is not currently supported and may be non-trivial due
 * to the decoupling of reading and writing in kplex but requirement for reading
 * what has been written as part of seatalk collision avoidance
 * "Seatalk" is a registered trademark of Raymarine and is used without
 * permission
 */

#include "kplex.h"

#define DEFSEATALKQSIZE 128
#define STBUFSIZE 128
#define MAXMSGLEN 18

struct if_seatalk {
    int fd;
    int saved;                  /* Are stored terminal settins valid? */
    struct termios otermios;    /* To restore previous interface settings
                                 *  on exit */
    struct termios ctermios;    /* current terminal settings */
};

int ttyopen(char *, enum iotype);
int ttysetup(int,struct termios *, tcflag_t, int);

void cleanup_seatalk(iface_t *ifa)
{
    struct if_seatalk *ifs = (struct if_seatalk *)ifa->info;

    if (!ifa->pair && ifs->saved) {
        if (tcsetattr(ifs->fd,TCSAFLUSH,&ifs->otermios) < 0) {
            if (ifa->type != PTY || errno != EIO)
                logwarn("Failed to restore serial line: %s",strerror(errno));
        }
    }
    close(ifs->fd);
}

void *ifdup_seatalk(void *ifs)
{
    struct if_seatalk *oldif,*newif;

    if ((newif = (struct if_seatalk *) malloc(sizeof(struct if_seatalk)))
        == (struct if_seatalk *) NULL)
        return(NULL);

    oldif = (struct if_seatalk *) ifs;

    if ((newif->fd=dup(oldif->fd)) <0) {
        free(newif);
        return(NULL);
    }

    newif->saved=oldif->saved;
    memcpy(&newif->otermios,&oldif->otermios,sizeof(struct termios));
    memcpy(&newif->ctermios,&oldif->ctermios,sizeof(struct termios));
    return((void *)newif);
}

/*
 * NMEA checksum routine for use when translating seatalk to NMEA
 * Args: Pointer to nmea sentence
 * Returns: CRC32 checksum of input */
int chksum(char*s)
{
    int c = 0;

    while (*s)
        c ^=*s++;
    return(c);
}

/*
 * Convert seatalk input to NMEA sentences
 * See README file. This is dodgy and incomplete
 * Args: pointer to seatalk command buffer and pointer to senblk_t which will
 * contain the output nmea sentence
 * Returns:s	0 on success
 * 		1 is sentence is not translatable
 * 		-1 on error
 */
int st2nmea(unsigned char *st, char *nmea)
{
    unsigned char *cmd=st;
    unsigned char *att=st+1;
    int val=0;

#ifdef DEBUG
    int i;
    fprintf(stderr,"ST: %02X %02X %02X",*cmd,*att,st[2]);
    for (i=0;i<*att;i++)
        fprintf(stderr," %02X",st[i+3]);
    fprintf(stderr,"\n");fflush(stderr);
#endif
    /* Only water temperature defined at this point. Probably wrongly */
    switch (*cmd) {
    case 0x00:
	    val=(st[4]<<8)+st[3];
	    sprintf(nmea,"$IIDBT,%.1f,f,%.1f,m,%.1f,F",val/10.0,val*0.3048,
			    val*0.6);
	    break;
    case 0x23:
        if (st[2]&0x40)
	/* Transducer not functional */
            return(1);
        sprintf(nmea,"$IIMTW,%d,C",(char) st[3]);
        break;

    default:
        return(1);
    }
    sprintf(nmea+strlen(nmea),"*%2X\r\n",chksum(nmea+1));
    return (0);
}

int set_parity(int fd, int ptype, struct termios *termset)
{
    if ((termset->c_cflag & PARODD) == ptype)
        return(0);

    termset->c_cflag=(termset->c_cflag &~PARODD)|ptype;
    if (tcsetattr(fd,TCSADRAIN,termset) < 0)
        return(-1);

    return(0);
}

/*
 * Write Seatalk data
 * This is not currently functional
 * Args: pointer to interface
 * Returns: pointer to interface
 */
iface_t * write_seatalk(iface_t *ifa)
{
    /* not currently supported */
    iface_thread_exit(-1);
}

/*
 * Read Seatalk data
 * Args: pointer to interface structure
 * Returns: Pointer to interface structure
 */
iface_t * read_seatalk(struct iface *ifa)
{
    struct if_seatalk *ifs=(struct if_seatalk *) ifa;
    int n,toread,perr=0,nocomm=1;
    char buf[STBUFSIZE];
    char stdata[MAXMSGLEN];
    char *cmdp=stdata;
    char *attr=stdata+1;
    char *bufp;
    senblk_t sblk;

    sblk.src=ifa->id;
    /* Here's what happens here. With PARMRK set, parity errors are signalled
     * by 0xff00 in the byte stream.  We have space parity set. A command bit
     * will will generate a parity error, so if we see 0xff followed by 0x00
     * we know the next byte is a command byte
     * Note that some USB to serial interfaces don't support MARK/SPACE parity.
     * Some (keyspan springs to mind) are just bad at reporting parity errors.
     */
    while ((n=read(ifs->fd,&buf,BUFSIZE)) > 0) {
        for (bufp=buf;n;n--,bufp++) {
            if (*bufp == 0xff) {
                if (perr) {
                    perr=0;
                    continue;
                } else
                    perr=1;
            }
            else  {
                if (*bufp == 0) {
                    if (perr) {
                        cmdp=stdata;
                        nocomm=0;
                        toread=3;
                        perr=0;
                        continue;
                    }
                }
                perr=0;
            }
            if (nocomm)
                continue;
            *cmdp = *bufp;
            if (--toread == 0) {
                if (st2nmea(stdata,sblk.data) == 0) {
                    sblk.len=strlen(sblk.data);
                    if (!senfilter(&sblk,ifa->ifilter))
                        push_senblk(&sblk,ifa->q);
                }
                nocomm=1;
                continue;
            }

            if (cmdp++ == attr)
                toread=((*attr) & 0xf) + 1;
        }
    }
    iface_thread_exit(errno);
}

/* Initialise a seatalk interface
 * Args: Pointer to incomplete interface structure
 * Returns: More complete interface structure
 * Seatalk interface not tested or really supported yet. Consider this a
 * placeholder
 */
struct iface *init_seatalk (struct iface *ifa)
{
    char *devname=NULL;
    struct if_seatalk *ifs;
    int baud=B4800;		/* This is the only supported baud rate */
    tcflag_t cflag;
    int st=1;
    int ret;
    struct kopts *opt;
    size_t qsize=0;

    /* temporary hack: writing not yet supported */
    if (ifa->direction != IN) {
        logerr(0,"Only inbound seatalk connections supported at present");
        return(NULL);
    }

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"filename"))
            devname=opt->val;
        else if (!strcasecmp(opt->var,"qsize")) {
            if (!(qsize=atoi(opt->val))) {
                logerr(0,"Invalid queue size specified: %s",opt->val);
                return(NULL);
            }
        } else  {
            logerr(0,"unknown interface option %s",opt->var);
            return(NULL);
        }
    }

    cflag=baud|CS8|CLOCAL|IGNBRK|PARENB|((ifa->direction == OUT)?0:CREAD);

    if ((ifs = malloc(sizeof(struct if_seatalk))) == NULL) {
        logerr(errno,"Could not allocate memory");
        return(NULL);
    }

    ifs->saved=0;

    if ((ifs->fd=ttyopen(devname,ifa->direction)) < 0) {
        return(NULL);
    }

    free_options(ifa->options);

    if (((ret=ttysetup(ifs->fd,&ifs->otermios,cflag,st)) < 0) ||
        (tcgetattr(ifs->fd,&ifs->ctermios) < 0)){
        if (ret == -1) {
            if (tcsetattr(ifs->fd,TCSANOW,&ifs->otermios) < 0) {
                logerr(errno,"Failed to reset serial line");
            }
        }
        close(ifs->fd);
        return(NULL);
    }
    ifs->saved=1;

    ifa->read=read_seatalk;
    ifa->write=write_seatalk;
    ifa->cleanup=cleanup_seatalk;

    if (ifa->direction != IN)
        if ((ifa->q =init_q(DEFSEATALKQSIZE)) == NULL) {
            logerr(0,"Could not create queue");
            cleanup_serial(ifa);
            return(NULL);
        }

    ifa->info=(void *)ifs;

    if (ifa->direction == BOTH) {
        if ((ifa->next=ifdup(ifa)) == NULL) {
            logerr(0,"Interface duplication failed");
            cleanup_serial(ifa);
            return(NULL);
        }
        ifa->direction=OUT;
        ifa->pair->direction=IN;
    }
    return(ifa);
}

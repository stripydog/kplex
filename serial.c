/* serial.c
 * This file is part of kplex
 * Copyright Keith Young 2012
 * For copying information see the file COPYING distributed with this software
 *
 * This file contains code for serial-like interfaces. This currently
 * comprises:
 *     nmea 0183 serial interfaces
 *     pseudo ttys
 *     seatalk serial interfaces
 * Note that nmea 0183 will normally need  converting from rs422 to something 
 * a serial interface can handle.
 * It is assumed that seatalk has been appropriately converted to serial
 * input.  A further restriction for seatalk interfaces is that they must
 * support MARK and SPACE parity.  This will preclude a number of keyspan
 * and prolific serial to USB devices being used.
 * The seatalk code here is experimental, incomplete and probably untested
 * The rest of it isn't much better
 */

#include "kplex.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <pty.h>
#include <limits.h>

#define DEFSERIALQSIZE 128

struct if_serial {
    int fd;
    struct termios otermios;    /* To restore previous interface settings
                   on exit */
};

/*
 * Cleanup interface on exit
 * Args: pointer to interface
 * Returns: Nothing
 */
void cleanup_serial(iface_t *ifa)
{
    struct if_serial *ifs = (struct if_serial *)ifa->info;

        /* Restore saved settings */
    if (tcsetattr(ifs->fd,TCSAFLUSH,&ifs->otermios) < 0) {
        perror("Warning: Failed to restore serial line");
    }
    close(ifs->fd);
}

/*
 * Open terminal (serial interface or pty)
 * Args: pathname and direction (input or output)
 * Returns: descriptor for opened interface or NULL on failure
 */
int ttyopen(char *device, enum iotype direction)
{
    int dev;
    struct stat sbuf;

    /* Check if device exists and is a character special device */
    if (stat(device,&sbuf) < 0) {
        fprintf(stderr,"Could not stat %s: %s\n",device,strerror(errno));
        return(-1);
    }

    if (!S_ISCHR(sbuf.st_mode)){
        fprintf(stderr,"%s is not a character device\n",device);
        return(-1);
    }

    /* Open device (RW for now..let's ignore direction...) */
    if ((dev=open(device,
        ((direction == OUT)?O_WRONLY:O_RDONLY)|O_NOCTTY)) < 0) {
        fprintf(stderr,"failed to open %s: %s\n",device,strerror(errno));
        return(-1);
    }

    return(dev);
}

/*
 * Set up terminal attributes
 * Args: device file descriptor,pointer to structure to save old termios,
 *     control flags and a flag indicating if this is a seatalk interface
 *     All a bit clunky and should be revised
 * Returns: 0 on success, -1 otherwise
 */
int ttysetup(int dev,struct termios *otermios_p, tcflag_t cflag, int st)
{
    struct termios ttermios,ntermios;

    /* Get existing terminal attributes and save them */
    if (tcgetattr(dev,otermios_p) < 0) {
        perror("failed to get terminal attributes");
        return (-1);
    }

    memset(&ntermios,0,sizeof(struct termios));

    ntermios.c_cflag=cflag;
    /* PARMRK is set for seatalk interface as parity errors are how we
     * identify commands
     */
    ntermios.c_iflag=IGNBRK|INPCK|st?PARMRK:0;
    ntermios.c_cc[VMIN]=1;
    ntermios.c_cc[VTIME]=0;

    if (tcflush(dev,TCIOFLUSH) < 0)
        perror("Warning: Failed to flush serial device");

    if (tcsetattr(dev,TCSAFLUSH,&ntermios) < 0) {
        perror("Failed to set up serial line!");
        return(-1);
    }

    /* Read back terminal attributes to check we set what we needed to */
    if (tcgetattr(dev,&ttermios) < 0) {
        perror("Failed to re-read serial line attributes");
        return(-1);
    }

    if ((ttermios.c_cflag != ntermios.c_cflag) ||
        (ttermios.c_iflag != ntermios.c_iflag)) {
        fprintf(stderr,"Failed to correctly set up serial line");
        return(-1);
    }

    return(0);
}

/*
 * Read from a serial interface
 * Args: pointer to interface structure
 * Returns: Pointer to interface structure
 */
struct iface * read_serial(struct iface *ifa)
{
    char buf[BUFSIZ];        /* Buffer for serial reads */
    char *bptr,*eptr=buf+BUFSIZ,*senptr;
    senblk_t sblk;
    struct if_serial *ifs = (struct if_serial *) ifa->info;
    int nread,cr=0,count=0,overrun=0;
    int fd;

    senptr=sblk.data;
    fd=ifs->fd;

    /* Read up to BUFSIZ data */
    while ((nread=read(fd,buf,BUFSIZ)) > 0) {
        /* Process the data we just read */
        for(bptr=buf,eptr=buf+nread;bptr<eptr;bptr++) {
            /* Copy to our senblk if we haven't exceeded max
             * sentence length */
            if (count < SENMAX) {
                ++count;
                *senptr++=*bptr;
            } else
            /* if max length exceeded, note that we've overrrun */
                ++overrun;

            if ((*bptr) == '\r') {
            /* <CR>: If next char is <LF> that's our sentence */
                ++cr;
            } else {
                if (*bptr == '\n' && cr) {
                /* <CR><LF>: End of sentence */
                    if (overrun) {
                    /* This sentence invalid: discard */
                        overrun=0;
                    } else {
                    /* send the sentence on its way */
                        sblk.len=count;
                        push_senblk(&sblk,ifa->q);
                    }
                    /* Reset the sentence */
                    senptr=sblk.data;
                    count=0;
                }
                /* The last char was NOT <CR> */
                cr=0;
            }
        }
    }
    iface_destroy(ifa,(void *) &errno);
}

/*
 * Write nmea sentences to serial output
 * Args: pointer to interface
 * Returns: pointer to interface
 */
struct iface * write_serial(struct iface *ifa)
{
    struct if_serial *ifs = (struct if_serial *) ifa->info;
    senblk_t *senblk_p;
    int fd=ifs->fd;
    int n=0;
    char *ptr;

    while(n >= 0) {
        /* NULL return from next_senblk means the queue has been shut
         * down. Time to die */
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

/*
 * Initialise a serial interface for nmea 0183 data
 * Args: interface specification string and pointer to interface structure
 * Retuns: Pointer to (completed) interface structure
 */
struct iface *init_serial (char *str, struct iface *ifa)
{
    char *devname,*option;
    struct if_serial *ifs;
    int baud=B4800;        /* Default for NMEA 0183. AIS will need
                   explicit baud rate specification */
    tcflag_t cflag;
    int st=0;

    /* Get device name */
    if ((devname=strtok(str+4,",")) == NULL) {
        fprintf(stderr,"Bad specification for serial device: \'%s\'\n",
            str);
        exit(1);
    }

    /* Get baud rate: Only the main 3 NMEA rates supported */
    if ((option = strtok(NULL,",")) != NULL) {
        if (!strcmp(option,"38400"))
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

    /* CREAD could be just be set. Ignored on some interfaces in any case */
    cflag=baud|CS8|CLOCAL|((ifa->direction == OUT)?0:CREAD);

    /* Allocate serial specific data storage */
    if ((ifs = malloc(sizeof(struct if_serial))) == NULL) {
        perror("Could not allocate memory\n");
        exit(1);
    }

    /* Open interface or die */
    if ((ifs->fd=ttyopen(devname,ifa->direction)) < 0) {
        exit (1);
    }

    /* Set up interface or die */
    if (ttysetup(ifs->fd,&ifs->otermios,cflag,0) < 0)
        exit(1);


    /* Allocate queue for outbound interfaces */
    if (ifa->direction == OUT)
        if ((ifa->q =init_q(DEFSERIALQSIZE)) == NULL) {
            perror("Could not create queue");
            exit(1);
        }

    /* Assign pointers to read, write and cleanup routines */
    ifa->read=read_serial;
    ifa->write=write_serial;
    ifa->cleanup=cleanup_serial;
    /* Link in serial specific data */
    ifa->info=(void *)ifs;
    return(ifa);
}

/*
 * Initialise a pty interface. For inputs, this is equivalent to init_serial
 * Args: string specifying the interface and pointer to (incomplete) interface
 * Returns: Completed interface structure
 */
struct iface *init_pty (char *str, struct iface *ifa)
{
    char *devname,*option;
    struct if_serial *ifs;
    int baud=B4800,slavefd;
    tcflag_t cflag;
    int st=0;
    struct stat statbuf;
    char slave[PATH_MAX];

    /* Get device name. Required for inputs.  For outputs, this is used to
     * create a symlink to the actual slave pty. If unspecified for an output,
     * the pty name is printed for the user to use as s/he sees fit */
    if ((devname=strtok(str+4,",")) == NULL) {
        if (ifa->direction == IN) {
            fprintf(stderr,"Bad specification for serial device: \'%s\'\n",
                str);
            exit(1);
        }
    }

    /* Baud rate (default 4800 */
    if ((option = strtok(NULL,",")) != NULL) {
        if (!strcmp(option,"38400"))
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

        if (devname && strcmp(devname,"-")) {
		/* Device name has been specified: Create symlink to slave */
            if (lstat(devname,&statbuf) == 0) {
                /* file exists */
                if (!S_ISLNK(statbuf.st_mode)) {
		/* If it's not a symlink already, don't replace it */
                    fprintf(stderr,"%s: File exists and is not a symbolic link\n",devname);
                    exit(1);
                }
		/* It's a symlink. remove it */
                if (unlink(devname)) {
                    fprintf(stderr,"Could not unlink %s: %s\n",
                        devname,strerror(errno));
                    exit(1);
                }
            }
	    /* link the given name to our new pty */
            if (symlink(slave,devname)) {
                fprintf(stderr,"Could not create symbolic link %s for %s\n",devname,slave);
                exit(1);
            }
        } else
	/* No device name was given: Just print the pty name */
            printf("Slave pty for output at %s baud is %s\n",(baud==B4800)?"4800":(baud==B9600)?"9600": "38.4k",slave);

        if ((ifa->q =init_q(DEFSERIALQSIZE)) == NULL) {
            perror("Could not create queue");
            exit(1);
        }
    } else
	/* Input: This is no different from a serial line */
        if ((ifs->fd=ttyopen(devname,ifa->direction)) < 0) {
            exit (1);
        }

    if (ttysetup(ifs->fd,&ifs->otermios,cflag,0) < 0)
        exit(1);

    ifa->read=read_serial;
    ifa->write=write_serial;
    ifa->cleanup=cleanup_serial;
    ifa->info=(void *)ifs;
    return(ifa);
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
int st2nmea(unsigned char *st, senblk_t *sptr)
{
    unsigned char *cmd=st;
    unsigned char *att=st+1;

#ifdef DEBUG
    int i;
    fprintf(stderr,"ST: %02X %02X %02X",*cmd,*att,st[2]);
    for (i=0;i<*att;i++)
        fprintf(stderr," %02X",st[i+3]);
    fprintf(stderr,"\n");fflush(stderr);
#endif

    /* Only water temperature defined at this point. Probably wrongly */
    switch (*cmd) {
    case 0x23:
        if (st[2]&0x40)
	/* Transducer not functional */
            return(1);
        sprintf(sptr->data,"$MTW,%d,C",(char) st[3]);
        break;
    default:
        return(1);
    }
    sprintf(sptr->data+strlen(sptr->data),"*%2X\r\n",chksum(sptr->data+1));
    return (0);
}

/*
 * Write Seatalk data
 * This is not currently functional
 * Args: pointer to interface
 * Returns: pointer to interface
 */
iface_t * write_seatalk(iface_t *ifa)
{
    int err = -1;

    /* not currently supported */
    iface_destroy(ifa,(void *) &err);
}

/*
 * Read Seatalk data
 * Args: pointer to interface structure
 * Returns: Pointer to interface structure
 */
iface_t * read_seatalk(struct iface *ifa)
{
    struct if_serial *ifs=(struct if_serial *) ifa;
    int n,i,rsize,perr=0;;
    unsigned char buf[18],*bufp;
    unsigned char *cmd=buf,*attr=buf+1,*b=buf+2;
    senblk_t sblk;

    /* Here's what happens here. With PARMRK set, parity errors are signalled
     * by 0xff00 in the byte stream.  We have space parity set. A command bit
     * will will generate a parity error, so if we see 0xff followed by 0x00
     * we know the next byte is a command byte
     * Note that some USB to serial interfaces don't support MARK/SPACE parity.
     * Some (keyspan springs to mind) are just bad at reporting parity errors.
     */
    for (;;) {
        for(rsize=5,perr=0;perr<5;rsize=5-perr) {
            if ((n=read(ifs->fd,b,rsize)) < 0){
		    /* OMG goto! but we're allowed outdated control structures
		     * with outdated protocols, right? */
                goto out;
            }
            for (i=0;i<n;i++) {
                switch (perr) {
                case 0:
                    if (b[i] = 0xff)    
                        perr=1;
                    break;
                case 1:
                    if (b[i] == 0)
                        perr=2;
                    else
                        perr=0;
                    break;
                case 2:
                    *cmd=b[i];
                    perr=3;
                    break;
                case 3:
                    *attr=b[i];
                    ++perr;
                    break;
                case 4:
                    b[0]=b[i];
                    ++perr;
                    break;
                }
            }
        }
        /* Now we have the command, read the data */
        rsize = *attr & 0xff;
        for(n=0,i=rsize,bufp=buf+1;i;i-=n,bufp+=n) {
            if ((n = read(ifs->fd,bufp,i)) < 0) {
                goto out;
            }
        }
        if (st2nmea(buf,&sblk) == 0)
            push_senblk(&sblk,ifa->q);
    }
out:
    iface_destroy(ifa,(void *) &errno);
}

/* Initialise a seatalk interface
 * Args: Pointer to incomplete interface structure
 * Returns: More complete interface structure
 * Seatalk interface not tested or really supported yet. Consider this a
 * placeholder
 */
struct iface *init_seatalk (char *str, struct iface *ifa)
{
    char *devname,*option;
    struct if_serial *ifs;
    int baud=B4800;		/* This is the only supported baud rate */
    tcflag_t cflag;
    int st=1;

    if ((devname=strtok(str+4,",")) == NULL) {
        fprintf(stderr,"Bad specification for serial device: \'%s\'\n",
            str);
        exit(1);
    }

    cflag=baud|CS8|CLOCAL|PARENB|((ifa->direction == OUT)?0:CREAD);

    if ((ifs = malloc(sizeof(struct if_serial))) == NULL) {
        perror("Could not allocate memory\n");
        exit(1);
    }

    if ((ifs->fd=ttyopen(devname,ifa->direction)) < 0) {
        exit (1);
    }

    if (ttysetup(ifs->fd,&ifs->otermios,cflag,st) < 0)
        exit(1);

    if (ifa->direction == OUT)
        if ((ifa->q =init_q(DEFSERIALQSIZE)) == NULL) {
            perror("Could not create queue");
            exit(1);
        }
    ifa->read=read_seatalk;
    ifa->write=write_seatalk;
    ifa->cleanup=cleanup_serial;
    ifa->info=(void *)ifs;
    return(ifa);
}

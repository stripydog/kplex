/* serial.c
 * This file is part of kplex
 * Copyright Keith Young 2012 - 2020
 * For copying information see the file COPYING distributed with this software
 *
 * This file contains code for serial-like interfaces. This currently
 * comprises:
 *     nmea 0183 serial interfaces
 *     pseudo ttys
 */

#include "kplex.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#if defined  __APPLE__ || defined __NetBSD__ || defined __OpenBSD__
#include <util.h>
#elif defined __FreeBSD__
#include <libutil.h>
#else
#include <pty.h>
#endif
#include <grp.h>
#include <pwd.h>

struct if_serial {
    int fd;
    char *slavename;            /* link to pty slave (if it exists) */
    int saved;                  /* Are stored terminal settins valid? */
    struct termios otermios;    /* To restore previous interface settings
                                 *  on exit */
};

/*
 * Duplicate struct if_serial
 * Args: if_serial to be duplicated
 * Returns: pointer to new if_serial
 * Should we dup, copy or re-open the fd here?
 */
void *ifdup_serial(void *ifs)
{
    struct if_serial *oldif,*newif;

    if ((newif = (struct if_serial *) malloc(sizeof(struct if_serial)))
        == (struct if_serial *) NULL)
        return(NULL);

    oldif = (struct if_serial *) ifs;

    if ((newif->fd=dup(oldif->fd)) <0) {
        free(newif);
        return(NULL);
    }

    newif->slavename=oldif->slavename;
    newif->saved=oldif->saved;
    memcpy(&newif->otermios,&oldif->otermios,sizeof(struct termios));
    return((void *)newif);
}

/*
 * Cleanup interface on exit
 * Args: pointer to interface
 * Returns: Nothing
 */
void cleanup_serial(iface_t *ifa)
{
    struct if_serial *ifs = (struct if_serial *)ifa->info;

    if (!ifa->pair) {
        if (ifs->saved) {
            if (tcsetattr(ifs->fd,TCSAFLUSH,&ifs->otermios) < 0) {
                if (ifa->type != PTY || errno != EIO)
                    logwarn(catgets(cat,8,1,
                            "Failed to restore serial line: %s"),
                            strerror(errno));
            }
        }
        if (ifs->slavename) {
            if (unlink(ifs->slavename) < 0)
                logerr(errno,catgets(cat,8,2,"Failed to remove link %s"),
                        ifs->slavename);
            free(ifs->slavename);
        }
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
    int dev,flags;
    struct stat sbuf;

    /* Check if device exists and is a character special device */
    if (stat(device,&sbuf) < 0) {
        logerr(errno,catgets(cat,8,3,"Could not stat %s"),device);
        return(-1);
    }

    if (!S_ISCHR(sbuf.st_mode)){
        logerr(0,catgets(cat,8,4,"%s is not a character device"),device);
        return(-1);
    }

    /* Open device (RW for now..let's ignore direction...) */
    if ((dev=open(device,
        ((direction == OUT)?O_WRONLY:(direction == IN)?O_RDONLY:O_RDWR)|O_NOCTTY|O_NONBLOCK)) < 0) {
        logerr(errno,catgets(cat,8,5,"Failed to open %s"),device);
        return(-1);
    }

    if ((flags = fcntl(dev,F_GETFL)) < 0)
        logerr(errno,catgets(cat,8,6,"Failed to get flags for %s"),device);
    else if (fcntl(dev,F_SETFL,flags & ~O_NONBLOCK) < 0)
        logerr(errno,catgets(cat,8,7,"Failed to set %s to non-blocking"),
                device);

    return(dev);
}

/*
 * Set up terminal attributes
 * Args: device file descriptor,pointer to structure to save old termios,
 *     control flags and a flag indicating if this is a seatalk interface
 *     All a bit clunky and should be revised
 * Returns: 0 on success, -1 otherwise
 */
int ttysetup(int dev,struct termios *otermios_p, int baud, int st)
{
    struct termios ttermios,ntermios;

    /* Get existing terminal attributes and save them */
    if (tcgetattr(dev,otermios_p) < 0) {
        logerr(errno,catgets(cat,8,8,"failed to get terminal attributes"));
        return(-2);
    }

    memcpy(&ntermios,otermios_p,sizeof(struct termios));

    /* PARMRK is set for seatalk interface as parity errors are how we
     * identify commands
     */
    ntermios.c_iflag|=(IGNBRK|INPCK);
    if (st)
        ntermios.c_iflag |= PARMRK;
    else
        ntermios.c_iflag &= ~PARMRK;

    /* disable software flow control */
    ntermios.c_cflag &= ~(IXON | IXOFF | IXANY);

    /* CS8 1 Stop bit no parity */
    ntermios.c_cflag &= ~PARENB;
    ntermios.c_cflag &= ~CSTOPB;

    ntermios.c_cflag &= ~CSIZE;
    ntermios.c_cflag |= CS8;

    /* Enable receiver (should be a problem for sender) and ignore hardware
     * flow control */
    ntermios.c_cflag |= (CLOCAL | CREAD);

    /* set baud rate */
    cfsetispeed(&ntermios,baud);
    cfsetospeed(&ntermios,baud);

    ntermios.c_cc[VMIN]=1;
    ntermios.c_cc[VTIME]=0;

    /* select raw mode */
    cfmakeraw(&ntermios);

    if (tcsetattr(dev,TCSANOW,&ntermios) < 0) {
        logerr(errno,catgets(cat,8,9,"Failed to set up serial line!"));
        return(-1);
    }

    /* Read back terminal attributes to check we set what we needed to */
    if (tcgetattr(dev,&ttermios) < 0) {
        logerr(errno,catgets(cat,8,10,
                "Failed to re-read serial line attributes"));
        return(-1);
    }

    if ((ttermios.c_cflag != ntermios.c_cflag) ||
        (ttermios.c_iflag != ntermios.c_iflag)) {
        logerr(0,catgets(cat,8,11,"Failed to correctly set up serial line"));
        return(-1);
    }

    return(0);
}

/*
 * Read from a serial interface
 * Args: pointer to interface structure pointer to buffer
 * Returns: Number of bytes read, zero on error or end of file
 */
ssize_t read_serial(void *ptr, char *buf)
{
    iface_t *ifa = (iface_t *) ptr;
    struct if_serial *ifs = (struct if_serial *) ifa->info;
    return(read(ifs->fd,buf,BUFSIZ));
}

/*
 * Write nmea sentences to serial output
 * Args: pointer to interface
 * Returns: Nothing. errno supplied to iface_thread_exit()
 */
void write_serial(struct iface *ifa)
{
    struct if_serial *ifs = (struct if_serial *) ifa->info;
    senblk_t *senblk_p;
    int fd=ifs->fd;
    int n=0,tlen=0;
    char *ptr;
    char *tbuf;

    if (ifa->tagflags) {
        if ((tbuf=malloc(TAGMAX)) == NULL) {
            logerr(errno,catgets(cat,8,12,
                    "Disabing tag output on interface id %u (%s)"),
                    ifa->id,(ifa->name)?ifa->name:catgets(cat,8,13,
                    "unlabelled"));
            ifa->tagflags=0;
        }
    }

    while(n >= 0) {
        /* NULL return from next_senblk means the queue has been shut
         * down. Time to die */
        if ((senblk_p = next_senblk(ifa->q)) == NULL)
            break;

        if (ifa->tagflags) {
            if ((tlen = gettag(ifa,tbuf,senblk_p)) == 0) {
                logerr(errno,catgets(cat,8,14,
                        "Disabing tag output on interface id %u (%s)"),
                        ifa->id,(ifa->name)?ifa->name:catgets(cat,8,13,
                        "unlabelled"));
                ifa->tagflags=0;
                free(tbuf);
            }
            ptr=tbuf;
            while(tlen) {
                if ((n=write(fd,ptr,tlen)) < 0)
                    break;
                tlen-=n;
                ptr+=n;
            }
            if (tlen) {
                senblk_free(senblk_p,ifa->q);
                break;
            }
        }

        ptr=senblk_p->data;
        tlen=senblk_p->len;
        while(tlen) {
            if ((n=write(fd,ptr,tlen)) < 0)
                break;
            tlen-=n;
            ptr+=n;
        }
        senblk_free(senblk_p,ifa->q);
        if (tlen)
            break;
    }

    if (ifa->tagflags)
        free(tbuf);

    iface_thread_exit(errno);
}

/*
 * Initialise a serial interface for nmea 0183 data
 * Args: interface specification string and pointer to interface structure
 * Retuns: Pointer to (completed) interface structure
 */
struct iface *init_serial (struct iface *ifa)
{
    char *devname;
    struct if_serial *ifs;
    int baud=B4800;        /* Default for NMEA 0183. AIS will need
                   explicit baud rate specification */
    int ret;
    struct kopts *opt;
    
    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"filename"))
            devname=opt->val;
        else if (!strcasecmp(opt->var,"baud")) {
            if (!strcmp(opt->val,"38400"))
                baud=B38400;
            else if (!strcmp(opt->val,"9600"))
                baud=B9600;
            else if (!strcmp(opt->val,"4800"))
                baud=B4800;
            else if (!strcmp(opt->val,"19200"))
                baud=B19200;
            else if (!strcmp(opt->val,"57600"))
                baud=B57600;
            else if (!strcmp(opt->val,"115200"))
                baud=B115200;
#ifdef B230400
            else if (!strcmp(opt->val,"230400"))
                baud=B230400;
#endif
#ifdef B460800
            else if (!strcmp(opt->val,"460800"))
                baud=B460800;
#endif
            else {
                logerr(0,catgets(cat,8,15,
                        "Unsupported baud rate \'%s\' in interface specification '\%s\'"),
                        opt->val,devname);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"qsize")) {
            if (!(ifa->qsize=atoi(opt->val))) {
                logerr(0,catgets(cat,8,16,"Invalid queue size specified: %s"),
                        opt->val);
                return(NULL);
            }
        } else  {
            logerr(0,catgets(cat,8,17,"unknown interface option %s"),opt->var);
            return(NULL);
        }
    }

    /* Allocate serial specific data storage */
    if ((ifs = malloc(sizeof(struct if_serial))) == NULL) {
        logerr(errno,catgets(cat,8,17,"Could not allocate memory"));
        return(NULL);
    }

    /* Open interface or die */
    if ((ifs->fd=ttyopen(devname,ifa->direction)) < 0) {
        return(NULL);
    }
    DEBUG(3,catgets(cat,8,18,"%s: opened serial device %s for %s"),ifa->name,
            devname,(ifa->direction==IN)?catgets(cat,8,20,"input"):
            (ifa->direction==OUT)?catgets(cat,8,21,"output"):
            catgets(cat,8,22,"input/output"));

    free_options(ifa->options);

    /* Set up interface or die */
    if ((ret = ttysetup(ifs->fd,&ifs->otermios,baud,0)) < 0) {
        if (ret == -1) {
            if (tcsetattr(ifs->fd,TCSANOW,&ifs->otermios) < 0) {
                logerr(errno,catgets(cat,8,23,"Failed to reset serial line"));
            }
        }
        return(NULL);
    }
    ifs->saved=1;
    ifs->slavename=NULL;

    /* Assign pointers to read, write and cleanup routines */
    ifa->read=do_read;
    ifa->readbuf=read_serial;
    ifa->write=write_serial;
    ifa->cleanup=cleanup_serial;

    /* Allocate queue for outbound interfaces */
    if (ifa->direction != IN) {
        if ((ifa->q = init_q(ifa->qsize,ifa->ofilter,ifa->name)) == NULL) {
            logerr(errno,catgets(cat,8,24,"Could not create queue"));
            cleanup_serial(ifa);
            return(NULL);
        }
    }

    /* Link in serial specific data */
    ifa->info=(void *)ifs;

    if (ifa->direction == BOTH) {
        if ((ifa->next=ifdup(ifa)) == NULL) {
            logerr(0,catgets(cat,8,25,"Interface duplication failed"));
            cleanup_serial(ifa);
            return(NULL);
        }
        ifa->direction=OUT;
        ifa->pair->direction=IN;
    }
    return(ifa);
}

/*
 * Initialise a pty interface. For inputs, this is equivalent to init_serial
 * Args: string specifying the interface and pointer to (incomplete) interface
 * Returns: Completed interface structure
 */
struct iface *init_pty (struct iface *ifa)
{
    char *devname=NULL;
    char* baudstr="4800";
    struct if_serial *ifs;
    int baud=B4800,slavefd;
    int ret;
    struct kopts *opt;
    char *master="s";
    char *cp;
    mode_t perm = 0;
    struct passwd *owner;
    struct group *group;
    uid_t uid=-1;
    gid_t gid=-1;
    struct stat statbuf;
    char slave[PATH_MAX];

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"mode")) {
            master=opt->val;
            if(strcmp(master,"master") && strcmp(master,"slave")) {
                logerr(0,catgets(cat,8,26,
                        "pty mode \'%s\' unsupported: must be master or slave"),
                        master);
                return(NULL);
            }
        }
        else if (!strcasecmp(opt->var,"filename"))
            devname=opt->val;
        else if (!strcasecmp(opt->var,"owner")) {
            if ((owner=getpwnam(opt->val)) == NULL) {
                logerr(0,catgets(cat,8,27,"No such user '%s'"),opt->val);
                return(NULL);
            }
            uid=owner->pw_uid;
        } else if (!strcasecmp(opt->var,"group")) {
            if ((group=getgrnam(opt->val)) == NULL) {
                logerr(0,catgets(cat,8,28,"No such group '%s'"),opt->val);
                return(NULL);
            }
            gid=group->gr_gid;
        }
        else if (!strcasecmp(opt->var,"perm")) {
            for (cp=opt->val;*cp;cp++) {
                if (*cp >= '0' && *cp < '8') {
                    perm <<=3;
                    perm += (*cp-'0');
                } else {
                    perm = 0;
                    break;
                }
            }
            perm &= ACCESSPERMS;
            if (perm == 0) {
                logerr(0,catgets(cat,8,29,
                        "Invalid permissions for tty device \'%s\'"),opt->val);
                return 0;
            }
        } else if (!strcasecmp(opt->var,"baud")) {
            baudstr=opt->val;
            if (!strcmp(opt->val,"38400"))
                baud=B38400;
            else if (!strcmp(opt->val,"9600"))
                baud=B9600;
            else if (!strcmp(opt->val,"4800"))
                baud=B4800;
            else if (!strcmp(opt->val,"19200"))
                baud=B19200;
            else if (!strcmp(opt->val,"57600"))
                baud=B57600;
            else if (!strcmp(opt->val,"115200"))
                baud=B115200;
            else {
                logerr(0,catgets(cat,8,15,
                        "Unsupported baud rate \'%s\' in interface specification '\%s\'"),
                        opt->val,devname);
                return(NULL);
            }
        } else {
            logerr(0,catgets(cat,8,32,"Unknown interface option %s"),opt->var);
            return(NULL);
        }
    }

    if ((ifs = malloc(sizeof(struct if_serial))) == NULL) {
        logerr(errno,catgets(cat,8,18,"Could not allocate memory"));
        return(NULL);
    }

    ifs->saved=0;
    ifs->slavename=NULL;

    if (*master != 's') {
        if (openpty(&ifs->fd,&slavefd,slave,NULL,NULL) < 0) {
            logerr(errno,catgets(cat,8,33,"Error opening pty"));
            return(NULL);
        }
        if (gid != -1 || uid != -1) {
            if (chown(slave,uid,gid) < 0) {
                logerr(errno,catgets(cat,8,34,
                        "Failed to set ownership or group for slave pty"));
                return(NULL);
            }
        }
        if (perm != 0) {
            if (chmod(slave,perm) < 0) {
                logerr(errno,catgets(cat,8,35,
                        "Failed to set permissions for slave pty"));
                return(NULL);
            }
        }
        if (devname) {
        /* Device name has been specified: Create symlink to slave */
            if (lstat(devname,&statbuf) == 0) {
            /* file exists */
                if (!S_ISLNK(statbuf.st_mode)) {
            /* If it's not a symlink already, don't replace it */
                    logerr(0,catgets(cat,8,36,
                            "%s: File exists and is not a symbolic link"),
                            devname);
                    return(NULL);
                }
            /* It's a symlink. remove it */
                if (unlink(devname) && errno != ENOENT) {
                    logerr(errno,catgets(cat,8,37,"Could not unlink %s"),
                            devname);
                    return(NULL);
                }
            }
        /* link the given name to our new pty */
            if (symlink(slave,devname)) {
                logerr(errno,catgets(cat,8,38,
                        "Could not create symbolic link %s for %s"),devname,
                        slave);
                return(NULL);
            }
            DEBUG(3,catgets(cat,8,39,"%s: created pty link %s to %s"),ifa->name,
                    devname,slave);

            /* Save the name to unlink it on exit */
            if ((ifs->slavename=strdup(devname)) == NULL) {
                logerr(errno,catgets(cat,8,40,
                        "Failed to save device name. Link will not be removed on exit"));
            }
        } else
    /* No device name was given: Just print the pty name */
            loginfo(catgets(cat,8,41,"Slave pty for output at %s baud is %s"),
                    baudstr,slave);
    } else {
    /* Slave mode: This is no different from a serial line */
        if (!devname) {
            logerr(0,catgets(cat,8,42,
                    "Must Specify a filename for slave mode pty"));
            return(NULL);
        }
        if ((ifs->fd=ttyopen(devname,ifa->direction)) < 0) {
            return(NULL);
        }
        DEBUG(3,catgets(cat,8,43,"%s: opened pty slave %s for %s"),ifa->name,
                devname,(ifa->direction==IN)?catgets(cat,8,20,"input"):
                (ifa->direction==OUT)?catgets(cat,8,21,"output"):
                catgets(cat,8,22,"input/output"));
    }

    free_options(ifa->options);

    if ((ret=ttysetup(ifs->fd,&ifs->otermios,baud,0)) < 0) {
        if (ret == -1) {
            if (tcsetattr(ifs->fd,TCSANOW,&ifs->otermios) < 0) {
                logerr(errno,catgets(cat,8,23,"Failed to reset serial line"));
            }
        }
        return(NULL);
    }
    ifs->saved=1;

    ifa->read=do_read;
    ifa->readbuf=read_serial;
    ifa->write=write_serial;
    ifa->cleanup=cleanup_serial;

    if (ifa->direction != IN) {
        if ((ifa->q = init_q(ifa->qsize,ifa->ofilter,ifa->name)) == NULL) {
            logerr(errno,catgets(cat,8,24,"Could not create queue"));
            cleanup_serial(ifa);
            return(NULL);
        }
    }

    ifa->info=(void *)ifs;
    if (ifa->direction == BOTH) {
        if ((ifa->next=ifdup(ifa)) == NULL) {
            logerr(0,catgets(cat,8,44,"Interface duplication failed"));
            cleanup_serial(ifa);
            return(NULL);
        }
        ifa->direction=OUT;
        ifa->pair->direction=IN;
    }
    return(ifa);
}

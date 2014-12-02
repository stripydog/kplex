/* fileio.c
 * This file is part of kplex
 * Copyright Keith Young 2012 - 2014
 * For copying information see the file COPYING distributed with this software
 *
 * This file contains code for i/o from files (incl stdin/stdout)
 */

#include "kplex.h"
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>

#define DEFFILEQSIZE 128

struct if_file {
    int fd;
    char *filename;
    size_t qsize;
};

/*
 * Duplicate struct if_file
 * Args: if_file to be duplicated
 * Returns: pointer to new if_file
 */
void *ifdup_file(void *iff)
{
    struct if_file  *newif;

    if ((newif = (struct if_file *) malloc(sizeof(struct if_file)))
        == (struct if_file *) NULL)
        return(NULL);

    memset ((void *)newif,0,sizeof(struct if_file));

    /* Read/Write only supported for stdin/stdout so don't allocate fp
     * And don't bother to duplicate filename
     */
    return(newif);
}

void cleanup_file(iface_t *ifa)
{
    struct if_file *iff = (struct if_file *) ifa->info;

    if (iff->fd >= 0)
        close(iff->fd);
    if (iff->filename)
        free(iff->filename);
}

void write_file(iface_t *ifa)
{
    struct if_file *ifc = (struct if_file *) ifa->info;
    senblk_t *sptr;
    int usereturn=flag_test(ifa,F_NOCR)?0:1;
    int data=0;
    int cnt=1;
    struct iovec iov[2];

    /* ifc->fd will only be < 0 if we're opening a FIFO.
     */
    if (ifc->fd < 0) {
        if ((ifc->fd=open(ifc->filename,O_WRONLY)) < 0) {
            logerr(errno,"Failed to open FIFO %s for writing\n",ifc->filename);
            iface_thread_exit(errno);
        }
        if ((ifa->q =init_q(ifc->qsize)) == NULL) {
            logerr(errno,"Could not create queue for FIFO %s",ifc->filename);
            iface_thread_exit(errno);
        }
    }

    if (ifa->tagflags) {
        if ((iov[0].iov_base=malloc(TAGMAX)) == NULL) {
                logerr(errno,"Disabing tag output on interface id %u (%s)",
                        ifa->id,(ifa->name)?ifa->name:"unlabelled");
                ifa->tagflags=0;
        } else {
            cnt=2;
            data=1;
        }
    }


    for(;;)  {
        if ((sptr = next_senblk(ifa->q)) == NULL) {
            break;
        }

        if (senfilter(sptr,ifa->ofilter)) {
            senblk_free(sptr,ifa->q);
            continue;
        }

        if (!usereturn) {
            sptr->data[sptr->len-2] = '\n';
            sptr->len--;
        }

        if (ifa->tagflags)
            if ((iov[0].iov_len = gettag(ifa,iov[0].iov_base,sptr)) == 0) {
                logerr(errno,"Disabing tag output on interface id %u (%s)",
                        ifa->id,(ifa->name)?ifa->name:"unlabelled");
                ifa->tagflags=0;
                cnt=1;
                data=0;
                free(iov[0].iov_base);
            }

        iov[data].iov_base=sptr->data;
        iov[data].iov_len=sptr->len;
        if (writev(ifc->fd,iov,cnt) <0) {
            if (!(flag_test(ifa,F_PERSIST) && errno == EPIPE) )
                break;
            if ((ifc->fd=open(ifc->filename,O_WRONLY)) < 0)
                break;
        }
        senblk_free(sptr,ifa->q);
    }

    if (cnt == 2)
        free(iov[0].iov_base);

    iface_thread_exit(errno);
}

void file_read_wrapper(iface_t *ifa)
{
    struct if_file *ifc = (struct if_file *) ifa->info;

    /* Create FILE stream here to allow for non-blocking opening FIFOs */
    if (ifc->fd == -1)
        if ((ifc->fd = open(ifc->filename,O_RDONLY)) < 0) {
            logerr(errno,"Failed to open FIFO %s for reading\n",ifc->filename);
            iface_thread_exit(errno);
        }

    do_read(ifa);
}

ssize_t read_file(iface_t *ifa, char *buf)
{
    struct if_file *ifc = (struct if_file *) ifa->info;
    ssize_t nread;

    for(;;) {
        if ((nread=read(ifc->fd,buf,BUFSIZ)) <=0) {
            if (!flag_test(ifa,F_PERSIST))
                break;
            close(ifc->fd);
            if ((ifc->fd=open(ifc->filename,O_RDONLY)) < 0) {
                logerr(errno,"Failed to re-open FIFO %s for reading\n",
                            ifc->filename);
                break;
            }
            continue;
        } else
            break;
    }
    return nread;
}

iface_t *init_file (iface_t *ifa)
{
    struct if_file *ifc;
    struct kopts *opt;
    struct stat statbuf;
    int ret;
    int append=0;

    if ((ifc = (struct if_file *)malloc(sizeof(struct if_file))) == NULL) {
        logerr(errno,"Could not allocate memory");
        return(NULL);
    }

    memset ((void *)ifc,0,sizeof(struct if_file));

    ifc->qsize=DEFFILEQSIZE;
    ifc->fd=-1;
    ifa->info = (void *) ifc;

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"filename")) {
            if (strcmp(opt->val,"-"))
                if ((ifc->filename=strdup(opt->val)) == NULL) {
                    logerr(errno,"Failed to duplicate argument string");
                    return(NULL);
                }
        } else if (!strcasecmp(opt->var,"qsize")) {
            if (!(ifc->qsize=atoi(opt->val))) {
                logerr(0,"Invalid queue size specified: %s",opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"append")) {
            if (!strcasecmp(opt->val,"yes")) {
                append++;
            } else if (!strcasecmp(opt->val,"no")) {
                append = 0;
            } else {
                logerr(0,"Invalid option \"append=%s\"",opt->val);
                return(NULL);
            }
        } else {
            logerr(0,"Unknown interface option %s\n",opt->var);
            return(NULL);
        }
    }

    /* We do allow use of stdin and stdout, but not if they're connected to
     * a terminal. This allows re-direction in background mode
     */
    if (ifc->filename == NULL) {
        if (flag_test(ifa,F_PERSIST)) {
            logerr(0,"Can't use persist mode with stdin/stdout");
            return(NULL);
        }

        if (((ifa->direction != IN) &&
                (((struct if_engine *)ifa->lists->engine->info)->flags &
                K_NOSTDOUT)) ||
                ((ifa->direction != OUT) &&
                (((struct if_engine *)ifa->lists->engine->info)->flags &
                K_NOSTDIN))) {
            logerr(0,"Can't use terminal stdin/stdout in background mode");
            return(NULL);
        }
        ifc->fd = (ifa->direction == IN)?STDIN_FILENO:STDOUT_FILENO;
    } else {
        if (ifa->direction == BOTH) {
            logerr(0,"Bi-directional file I/O only supported for stdin/stdout");
            return(NULL);
        }

        if ((ret=stat(ifc->filename,&statbuf)) < 0) {
            if (ifa->direction != OUT) {
                logerr(errno,"stat %s",ifc->filename);
                return(NULL);
            }
        }
        if ((ret == 0) && S_ISFIFO(statbuf.st_mode)) {
            /* Special rules for FIFOs. Opening here would hang for a reading
             * interface with no writer. Given that we're single threaded here,
             * that would be bad
             */
            if (access(ifc->filename,(ifa->direction==IN)?R_OK:W_OK) != 0) {
                logerr(errno,"Could not access %s",ifc->filename);
                return(NULL);
            }
        }
        else {
            if (flag_test(ifa,F_PERSIST)) {
                logerr(0,"Can't use persist mode on %s: Not a FIFO",
                        ifc->filename);
                return(NULL);
            }
            if ((ifc->fd=open(ifc->filename,(ifa->direction==IN)?O_RDONLY:
                    (O_WRONLY|O_CREAT|(append)?O_APPEND:0))) < 0) {
                logerr(errno,"Failed to open %s",ifc->filename);
                return(NULL);
            }
        }
    }

    free_options(ifa->options);

    ifa->write=write_file;
    ifa->read=file_read_wrapper;
    ifa->readbuf=read_file;
    ifa->cleanup=cleanup_file;

    if (ifa->direction != IN && ifc->fd >= 0)
        if ((ifa->q =init_q(ifc->qsize)) == NULL) {
            logerr(0,"Could not create queue");
            cleanup_file(ifa);
            return(NULL);
        }

    if (ifa->direction == BOTH) {
        if ((ifa->next=ifdup(ifa)) == NULL) {
            logerr(0,"Interface duplication failed");
            cleanup_file(ifa);
            return(NULL);
        }
        ifa->direction=OUT;
        ifa->pair->direction=IN;
        ifc = (struct if_file *) ifa->pair->info;
        ifc->fd=STDIN_FILENO;
    }
    return(ifa);
}


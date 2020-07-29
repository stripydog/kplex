/* fileio.c
 * This file is part of kplex
 * Copyright Keith Young 2012 - 2020
 * For copying information see the file COPYING distributed with this software
 *
 * This file contains code for i/o from files (incl stdin/stdout)
 */

#include "kplex.h"
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <pwd.h>
#include <grp.h>
#include <sys/utsname.h>
#include <sys/time.h>

#define KEYWORD_MAX 15
#define FNAME_MAX 255

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
            logerr(errno,catgets(cat,4,1,
                    "Failed to open FIFO %s for writing\n"),ifc->filename);
            iface_thread_exit(errno);
        }
        if (init_q(ifa,ifc->qsize) < 0) {
            logerr(errno,catgets(cat,4,2,"Could not create queue for FIFO %s"),
                    ifc->filename);
            iface_thread_exit(errno);
        }
        DEBUG(3,catgets(cat,4,3,"%s opened FIFO %s for writing"),ifa->name,
                ifc->filename);
    }

    if (ifa->tagflags) {
        if ((iov[0].iov_base=malloc(TAGMAX)) == NULL) {
                logerr(errno,catgets(cat,4,4,"%s: Disabing tag output"),
                ifa->name);
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
                logerr(errno,catgets(cat,4,4,"%s: Disabing tag output"),
                        ifa->name);
                ifa->tagflags=0;
                cnt=1;
                data=0;
                free(iov[0].iov_base);
            }

        iov[data].iov_base=sptr->data;
        iov[data].iov_len=sptr->len;
        if (writev(ifc->fd,iov,cnt) <0) {
            if (!(flag_test(ifa,F_PERSIST) && errno == EPIPE) ) {
                logerr(errno,catgets(cat,4,5,"%s: write failed"),ifa->name);
                break;
            }

            if ((ifc->fd=open(ifc->filename,O_WRONLY)) < 0) {
                logerr(errno,catgets(cat,4,6,"%s: failed to re-open %s"),
                        ifa->name,ifc->filename);
                break;
            }
            DEBUG(4,catgets(cat,4,7,"%s: reconnected to FIFO %s"),ifa->name,
                    ifc->filename);
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
    if (ifc->fd == -1) {
        if ((ifc->fd = open(ifc->filename,O_RDONLY)) < 0) {
            logerr(errno,catgets(cat,4,8,
                    "Failed to open FIFO %s for reading\n"),ifc->filename);
            iface_thread_exit(errno);
        } else {
            DEBUG(3,catgets(cat,4,9,"%s: opened %s for reading"),ifa->name,
                    ifc->filename);
        }
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
                logerr(errno,catgets(cat,4,10,
                        "Failed to re-open FIFO %s for reading\n"),
                        ifc->filename);
                break;
            }
            DEBUG(4,catgets(cat,4,11,"%s: re-opened %s for reading"),ifa->name,
                    ifc->filename);
            continue;
        } else
            break;
    }
    return nread;
}

/*
 * replace a keyword with its expansion
 * Args: buffer to load the result to, size of buffer, keyword to expand
 * Returns: Number of characters in expanded buffer (excluding terminating
 * NULL) on success, 0 on failure or if expansion too large for buffer.
 */
size_t replace_keyword(char *buf, size_t max, const char * const keyword)
{
    struct utsname u;
    size_t n;

    if (!strcmp(keyword,"host")) {
        if (uname (&u) < 0) {
            return(0);
        }
        if ((n = strlen(u.nodename)) >= max) {
            return(0);
        }
        strcpy(buf,u.nodename);
        return(n);
    }

    return(0);
}

/*
 * Expand an extended filename string
 * Args: format string
 * Returns: expanded filename string on success, NULL on error
 */
char *expand_filename(const char * const format)
{
    char buf[FNAME_MAX+1],buf2[FNAME_MAX+1];
    char keyword[KEYWORD_MAX+1];
    const char *fptr;
    char *filename;
    int i,j,ret;
    int dotime=0;
    struct timeval tv;
    struct tm tms;

    errno = 0;

    for (i=0,fptr=format;i <= FNAME_MAX;) {
        if ((buf[i] = *fptr++) == '\0') {
            break;
        }
        if (buf[i] == '%') {
            if (*fptr == '{') {
                for (j=0,++fptr;j<KEYWORD_MAX;j++) {
                    if ((keyword[j] = *fptr++) == '}') {
                        keyword[j] = '\0';
                        break;
                    }
                }
                if (j == KEYWORD_MAX) {
                    return NULL;
                }
                if ((ret = replace_keyword(buf+i,sizeof(buf)-i,keyword)) == 0) {
                    return NULL;
                }

                i += ret;
                continue;
            } else {
                if (*fptr == '%') {
                    buf[++i] = *fptr++;
                }
                dotime=1;
            }
        }
        ++i;
    }
    if ( i > FNAME_MAX ) {
        return NULL;
    }

    ret = i;
    if (dotime) {
        (void) gettimeofday(&tv,NULL);
        localtime_r(&tv.tv_sec,&tms);
        if ((ret = strftime(buf2,sizeof(buf2),buf,&tms)) == 0) {
            return NULL;
        }
    }
    if ((filename = (char *) malloc(++ret)) == NULL) {
        return NULL;
    }
    strcpy(filename,(dotime)?buf2:buf);
    return filename;
}

iface_t *init_file (iface_t *ifa)
{
    struct if_file *ifc;
    struct kopts *opt;
    struct stat statbuf;
    int ret;
    int append=0;
    uid_t uid=-1;
    gid_t gid=-1;
    struct passwd *owner;
    struct group *group;
    mode_t tperm,perm=0;
    char *cp;

    if ((ifc = (struct if_file *)malloc(sizeof(struct if_file))) == NULL) {
        logerr(errno,catgets(cat,4,12,"Could not allocate memory"));
        return(NULL);
    }

    memset ((void *)ifc,0,sizeof(struct if_file));

    ifc->qsize=DEFQSIZE;
    ifc->fd=-1;
    ifa->info = (void *) ifc;

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"filename")) {
            if (strcmp(opt->val,"-"))
                if ((ifc->filename=strdup(opt->val)) == NULL) {
                    logerr(errno,catgets(cat,4,13,
                            "Failed to duplicate argument string"));
                    return(NULL);
                }
        } else if (!strcasecmp(opt->var,"filenamex")) {
            if (strcmp(opt->val,"-"))
                if ((ifc->filename=expand_filename(opt->val)) == NULL) {
                    logerr(errno,catgets(cat,4,36,
                            "Failed to expand filenamex"));
                    return(NULL);
                }
        } else if (!strcasecmp(opt->var,"qsize")) {
            if (!(ifc->qsize=atoi(opt->val))) {
                logerr(0,catgets(cat,4,14,"Invalid queue size specified: %s"),
                        opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"append")) {
            if (!strcasecmp(opt->val,"yes")) {
                append++;
            } else if (!strcasecmp(opt->val,"no")) {
                append = 0;
            } else {
                logerr(0,catgets(cat,4,15,"Invalid option \"append=%s\""),
                        opt->val);
                return(NULL);
            }
        } else if (!strcasecmp(opt->var,"owner")) {
            if ((owner=getpwnam(opt->val)) == NULL) {
                logerr(0,catgets(cat,4,16,"No such user '%s'"),opt->val);
                return(NULL);
            }
            uid=owner->pw_uid;
        } else if (!strcasecmp(opt->var,"group")) {
            if ((group=getgrnam(opt->val)) == NULL) {
                logerr(0,catgets(cat,4,17,"No such group '%s'"),opt->val);
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
                logerr(0,catgets(cat,4,18,
                        "Invalid permissions for tty device \'%s\'"),opt->val);
                return 0;
            }
        } else {
            logerr(0,catgets(cat,4,19,"Unknown interface option %s\n"),
                    opt->var);
            return(NULL);
        }
    }

    /* We do allow use of stdin and stdout, but not if they're connected to
     * a terminal. This allows re-direction in background mode
     */
    if (ifc->filename == NULL) {
        if (flag_test(ifa,F_PERSIST)) {
            logerr(0,catgets(cat,4,20,
                    "Can't use persist mode with stdin/stdout"));
            return(NULL);
        }

        if (((ifa->direction != IN) &&
                (((struct if_engine *)ifa->lists->engine->info)->flags &
                K_NOSTDOUT)) ||
                ((ifa->direction != OUT) &&
                (((struct if_engine *)ifa->lists->engine->info)->flags &
                K_NOSTDIN))) {
            logerr(0,catgets(cat,4,21,
                    "Can't use terminal stdin/stdout in background mode"));
            return(NULL);
        }
        if (ifa->direction == IN) {
            ifc->fd = STDIN_FILENO;
            DEBUG(3,catgets(cat,4,22,"%s: using stdin"),ifa->name);
        } else {
            ifc->fd = STDOUT_FILENO;
            DEBUG(3,catgets(cat,4,23,"%s: using %s"),ifa->name,
                    (ifa->direction==OUT)?"stdout":"stdin/stdout");
        }
    } else {
        if (ifa->direction == BOTH) {
            logerr(0,catgets(cat,4,24,
                    "Bi-directional file I/O only supported for stdin/stdout"));
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
                logerr(errno,catgets(cat,4,25,"Could not access %s"),
                        ifc->filename);
                return(NULL);
            }
        } else {
            if (flag_test(ifa,F_PERSIST)) {
                logerr(0,catgets(cat,4,26,
                        "Can't use persist mode on %s: Not a FIFO"),
                        ifc->filename);
                return(NULL);
            }
            if (perm)
                tperm=umask(0);

            errno=0;
            /* If file is for output and doesn't currently exist...*/
            if (ifa->direction != IN && (ifc->fd=open(ifc->filename,
                        O_WRONLY|O_CREAT|O_EXCL|((append)?O_APPEND:0),
                        (perm)?perm:0664)) >= 0) {
                if (gid != 0 || uid != -1) {
                    if (chown(ifc->filename,uid,gid) < 0) {
                        logerr(errno,catgets(cat,4,27,
                                "Failed to set ownership or group on output file %s"),
                                ifc->filename);
                        return(NULL);
                    }
                }
            DEBUG(3,catgets(cat,4,28,"%s: created %s for output"),ifa->name,
                    ifc->filename);
            } else {
                if (errno && errno != EEXIST) {
                    logerr(errno,catgets(cat,4,29,"Failed to create file %s"),
                            ifc->filename);
                    return(NULL);
                }
                /* file is for input or already exists */
                if ((ifc->fd=open(ifc->filename,(ifa->direction==IN)?O_RDONLY:
                        (O_WRONLY|((append)?O_APPEND:O_TRUNC)))) < 0) {
                    logerr(errno,catgets(cat,4,30,"Failed to open file %s"),
                            ifc->filename);
                    return(NULL);
                }
                DEBUG(3,catgets(cat,4,31,"%s: opened %s for %s"),ifa->name,
                        ifc->filename,
                        (ifa->direction==IN)?catgets(cat,4,32,"input"):
                        catgets(cat,4,33,"output"));
            }
            /* reset umask: not really necessary */
            if (perm)
                (void) umask(tperm);
        }
    }

    free_options(ifa->options);

    ifa->write=write_file;
    ifa->read=file_read_wrapper;
    ifa->readbuf=read_file;
    ifa->cleanup=cleanup_file;

    if (ifa->direction != IN && ifc->fd >= 0)
        if (init_q(ifa, ifc->qsize)< 0) {
            logerr(0,catgets(cat,4,34,"Could not create queue"));
            cleanup_file(ifa);
            return(NULL);
        }

    if (ifa->direction == BOTH) {
        if ((ifa->next=ifdup(ifa)) == NULL) {
            logerr(0,catgets(cat,4,35,"Interface duplication failed"));
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

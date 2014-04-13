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

#define DEFFILEQSIZE 128

struct if_file {
    FILE *fp;
    char *filename;
    size_t qsize;
    int usereturn;
};

/*
 * Duplicate struct if_file
 * Args: if_file to be duplicated
 * Returns: pointer to new if_file
 */
void *ifdup_file(void *iff)
{
    struct if_file  *newif,*oldif;

    if ((newif = (struct if_file *) malloc(sizeof(struct if_file)))
        == (struct if_file *) NULL)
        return(NULL);

    memset ((void *)newif,0,sizeof(struct if_file));
    
    oldif = (struct if_file *) iff;

    newif->usereturn = oldif->usereturn;

    /* Read/Write only supported for stdin/stdout so don't allocate fp
     * And don't bother to duplicate filename
     */
    return(newif);
}

void cleanup_file(iface_t *ifa)
{
    struct if_file *iff = (struct if_file *) ifa->info;

    if (iff->fp)
        fclose(iff->fp);
    if (iff->filename)
        free(iff->filename);
}

void write_file(iface_t *ifa)
{
    int n;
    struct if_file *ifc = (struct if_file *) ifa->info;
    senblk_t *sptr;
    char *tbuf;

    /* ifc->fp will only be NULL if we're opening a FIFO.
     */
    if (ifc->fp == NULL) {
        if ((ifc->fp=fopen(ifc->filename,"w")) == NULL) {
            logerr(errno,"Failed to open FIFO %s for writing\n",ifc->filename);
            iface_thread_exit(errno);
        }
        if ((ifa->q =init_q(ifc->qsize)) == NULL) {
            logerr(errno,"Could not create queue for FIFO %s",ifc->filename);
            iface_thread_exit(errno);
        }
        if (setvbuf(ifc->fp,NULL,_IOLBF,0) !=0)
            iface_thread_exit(errno);
    }

    if ((tbuf=(char *)malloc(TAGMAX)) == NULL) {
        logerr(errno,"Disabing tag output on interface id %u (%s)",
                ifa->id,(ifa->name)?ifa->name:"unlabelled");
        ifa->tagflags=0;
    }

    for(;;)  {
        if ((sptr = next_senblk(ifa->q)) == NULL) {
            break;
        }

        if (senfilter(sptr,ifa->ofilter)) {
            senblk_free(sptr,ifa->q);
            continue;
        }
            
        if (ifc->usereturn == 0) {
            sptr->data[sptr->len-2] = '\n';
            sptr->data[sptr->len-1] = '\0';
        }


        if (ifa->tagflags)
            if ((n = gettag(ifa,tbuf)) == 0) {
                logerr(errno,"Disabing tag output on interface id %u (%s)",
                    ifa->id,(ifa->name)?ifa->name:"unlabelled");
                ifa->tagflags=0;
                free(tbuf);
            }

        if ((ifa->tagflags && (fwrite(tbuf,1,n,ifc->fp) == EOF )) || fputs(sptr->data,ifc->fp) == EOF) {
            if (!(flag_test(ifa,F_PERSIST) && errno == EPIPE) )
                break;
            if (((ifc->fp=freopen(ifc->filename,"w",ifc->fp)) == NULL) ||
                    (setvbuf(ifc->fp,NULL,_IOLBF,0) !=0))
                break;
        }
        senblk_free(sptr,ifa->q);
    }

    if (ifa->tagflags)
        free(tbuf);

    iface_thread_exit(errno);
}

void read_file(iface_t *ifa)
{
    struct if_file *ifc = (struct if_file *) ifa->info;
    senblk_t sblk;
    char tbuf[TAGMAX];
    int ch,countmax,count;
    char *ptr;
    enum sstate senstate=SEN_NODATA;

    /* Create FILE stream here to allow for non-blocking opening FIFOs */
    if (ifc->fp == NULL)
        if ((ifc->fp = fopen(ifc->filename,"r")) == NULL) {
            logerr(errno,"Failed to open FIFO %s for reading\n",ifc->filename);
            iface_thread_exit(errno);
        }

    sblk.src=ifa->id;

    for(;;) {
        if ((ch = fgetc(ifc->fp)) == EOF) {
            if (errno) {
                if (errno == EINTR)
                    errno=0;
                break;
            }
            if (feof(ifc->fp) && (flag_test(ifa,F_PERSIST))) {
                if ((ifc->fp = freopen(ifc->filename,"r",ifc->fp)) == NULL) {
                    logerr(errno,"Failed to re-open FIFO %s for reading\n",
                            ifc->filename);
                    break;
                }
                continue;
            }
            break;
        }
        switch (ch) {
        case '$':
        case '!':
            if (senstate == SEN_NODATA || senstate == SEN_TAGSEEN) {
                ptr=sblk.data;
                countmax=SENMAX+1+ifc->usereturn;
                count=1;
                *ptr++=ch;
                senstate=SEN_SENPROC;
            } else {
                senstate = SEN_ERR;
            }
            continue;
        case '\\':
            if (senstate==SEN_TAGPROC) {
                *ptr++=ch;
                /* Here we would do some actual work, but for this first
                 * cut we're just going to discard the tagblock */
                senstate=SEN_TAGSEEN;
            } else if (senstate == SEN_NODATA || senstate == SEN_TAGSEEN) {
                    senstate=SEN_TAGPROC;
                ptr=tbuf;
                countmax=TAGMAX-1;
                *ptr++=ch;
                count=1;
            } else
                senstate=SEN_ERR;
            continue;
        default:
            break;
        }

        if (senstate != SEN_SENPROC && senstate != SEN_TAGPROC) {
                if (senstate != SEN_NODATA && senstate != SEN_ERR)
                    senstate=SEN_ERR;
                continue;
        }

        if (count++ > countmax) {
            senstate=SEN_ERR;
            continue;
        }

        if ((*ptr++=ch) == '\n') {
            if (ifc->usereturn) {
                if (*(ptr-1) != '\r') {
                    senstate=SEN_NODATA;
                    continue;
                }
            } else {
                *(ptr-1)='\r';
                *ptr='\n';
                ++count;
            }
            sblk.len=count;
            if (!(ifa->checksum && checkcksum(&sblk) &&
                    (sblk.len > 0)) &&
                    senfilter(&sblk,ifa->ifilter) == 0) {
                push_senblk(&sblk,ifa->q);
            }
            senstate=SEN_NODATA;
        }
    }
    iface_thread_exit(errno);
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
        } else if (!strcasecmp(opt->var,"eol")) {
            if (!strcasecmp(opt->val,"rn"))
                ifc->usereturn=1;
            else if (!strcasecmp(opt->val,"n")) {
                ifc->usereturn=0;
            } else {
                logerr(0,"Invalid option \"eol=%s\": Must be \"n\" or \"rn\"",
                        opt->val);
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
        ifc->fp = (ifa->direction == IN)?stdin:stdout;
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
            if ((ifc->fp=fopen(ifc->filename,(ifa->direction==IN)?"r":
                    (append)?"a":"w")) == NULL) {
                logerr(errno,"Failed to open %s",ifc->filename);
                return(NULL);
            }
            if (ifa->direction == OUT)
                /* Make output line buffered */
                setlinebuf(ifc->fp);
        }
    }

    free_options(ifa->options);

    ifa->write=write_file;
    ifa->read=read_file;
    ifa->cleanup=cleanup_file;

    if (ifa->direction != IN && ifc->fp != NULL)
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
        ifc->fp=stdin;
    }
    return(ifa);
}


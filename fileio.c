/* fileio.c
 * This file is part of kplex
 * Copyright Keith Young 2012
 * For copying information see the file COPYING distributed with this software
 *
 * This file contains code for i/o from files (incl stdin/stdout)
 */

#include "kplex.h"
#include <stdlib.h>

#define DEFFILEQSIZE 128

struct if_file {
    FILE *fp;
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

    /* Read/Write only supported for stdin/stdout so don't allocate fp */
    return(newif);
}

void cleanup_file(iface_t *ifa)
{
    struct if_file *iff = (struct if_file *) ifa->info;

    fclose(iff->fp);
}

iface_t *write_file(iface_t *ifa)
{
    struct if_file *ifc = (struct if_file *) ifa->info;
    FILE *fp = ifc->fp;
    senblk_t *sptr;

    for(;;)  {
        if ((sptr = next_senblk(ifa->q)) == NULL) {
		    break;
        }

        if (senfilter(sptr,ifa->ofilter)) {
            senblk_free(sptr,ifa->q);
            continue;
        }
            
        sptr->data[sptr->len-2] = '\n';
        sptr->data[sptr->len-1] = '\0';
        if (fputs(sptr->data,fp) == EOF)
		break;
        senblk_free(sptr,ifa->q);
    }
    iface_thread_exit(errno);
}

iface_t *read_file(iface_t *ifa)
{
    struct if_file *ifc = (struct if_file *) ifa->info;
    senblk_t sblk;
    int len;
    char *eptr;

    sblk.src=ifa->id;
    while (fgets(sblk.data,SENMAX,ifc->fp) == sblk.data) {
        if ((len = strlen(sblk.data)) == 0) {
            break;
        }
        if (sblk.data[len-1]  != '\n') {
            logwarn("Line exceeds max sentence length (discarding)\n");
            while ((eptr = fgets(sblk.data,SENMAX,ifc->fp)) == sblk.data) {
                if (sblk.data[strlen(sblk.data)-1]  == '\n') {
                    break;
                }
            }
            if (eptr == NULL)
                break;
            continue;
        }
        sblk.data[len-1]='\r';
        sblk.data[len]='\n';
        sblk.len=len+1;
        if (ifa->checksum && checkcksum(&sblk))
            continue;
        if (senfilter(&sblk,ifa->ifilter))
            continue;
        push_senblk(&sblk,ifa->q);
    }
    iface_thread_exit(errno);
}

iface_t *init_file (iface_t *ifa)
{
    char *fname=NULL;
    size_t qsize=DEFFILEQSIZE;
    struct if_file *ifc;
    struct kopts *opt,*nextopt;

    if ((ifc = (struct if_file *)malloc(sizeof(struct if_file))) == NULL) {
        logerr(errno,"Could not allocate memory");
        return(NULL);
    }
    ifa->info = (void *) ifc;

    for(opt=ifa->options;opt;opt=opt->next) {
        if (!strcasecmp(opt->var,"filename"))
            fname=opt->val;
        else if (!strcasecmp(opt->var,"qsize")) {
            if (!(qsize=atoi(opt->val))) {
                logerr(0,"Invalid queue size specified: %s",opt->val);
                return(NULL);
            }
        } else  {
            logerr(0,"Unknown interface option %s\n",opt->var);
            return(NULL);
        }
    }

    if (!fname || !strcmp(fname,"-")) {
        ifc->fp = (ifa->direction == IN)?stdin:stdout;
    } else {
        if (ifa->direction == BOTH) {
            logerr(0,"Bi-directional file I/O only supported for stdin/stdout\n");
            return(NULL);
        }
        if ((ifc->fp = fopen(fname,(ifa->direction == IN)?"r":"w"))
                == NULL) {
            logerr(errno,"Could not open %s: %s\n",fname);
            return(NULL);
        }
    }

    free_options(ifa->options);

    ifa->write=write_file;
    ifa->read=read_file;
    ifa->cleanup=cleanup_file;

    if (ifa->direction != IN)
        if ((ifa->q =init_q(qsize)) == NULL) {
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


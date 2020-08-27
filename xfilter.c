/* xfilter.c.
 * This file is part of kplex.
 * Copyright Keith Young 2020
 * For copying information see the file COPYING distributed with this software
 *
 */


#include "kplex.h"
#include <signal.h>

void xf_thread_exit(xfilter_t *xf,int ret)
{
    (void) kill(xf->child,SIGTERM);
    pthread_exit((void *)&ret);
}

void xf_cleanup(xfilter_t *xf)
{
    (void) pthread_cancel(xf->read_thread);
    (void) pthread_cancel(xf->write_thread);
    (void) pthread_join(xf->read_thread,NULL);
    (void) pthread_join(xf->write_thread,NULL);
    kill(xf->child,SIGTERM);
    if (xf->q) {
        free(xf->q->base);
        free(xf->q);
    };

    close(xf->infd);
    close(xf->outfd);
    free(xf);
};

xfilter_t *init_xfilter(iface_t *ifa, char *fname) {
    struct xfilter *xf;

    if ((xf = malloc(sizeof(struct xfilter))) == NULL) {
        return(NULL);
    }

    if ((xf->name = strdup(fname)) == NULL) {
        free(xf);
        return(NULL);
    }

    xf->parent = ifa;

    return xf;
}

ssize_t xf_read(void *xf, char *buf)
{
    return(read(((xfilter_t *)xf)->outfd,buf,BUFSIZ));
}

void xfilter_read(void *arg)
{
    xfilter_t *xf = (struct xfilter *) arg;

    read_input(xf->parent, xf_read, xf->type);
    xf_thread_exit(xf,errno);

}

void xfilter_write (void *arg)
{
    xfilter_t *xf = (struct xfilter *) arg;
    senblk_t *senblk_p;
    char *ptr;
    size_t tlen,n;
    
    for  (tlen=0;;) {
        if ((senblk_p = next_senblk(xf->q)) == NULL)
            break;

        for (tlen=senblk_p->len,ptr=senblk_p->data;tlen;tlen-=n,ptr+=n) {
            if ((n=write(xf->infd,ptr,tlen)) < 0)
                break;
        }
        senblk_free(senblk_p,xf->q);
        if (tlen)
            break;
    }
    xf_thread_exit(xf,errno);
};

int start_xfilter(xfilter_t *xf) {
    int finfd[2],foutfd[2];
    pid_t pid;
    if (xf->q == NULL || xf->name == NULL || xf->parent == NULL) {
        return -1;
    }

    if (pipe(finfd) || pipe(foutfd)) {
        return(-1);
    }

    if ((pid = fork()) < 0) {
        return(-1);
    }


    if (pid == 0) {
        dup2(finfd[0],0);
        close(finfd[1]);
        dup2(foutfd[1],1);
        close(foutfd[0]);
        signal(SIGTERM,SIG_DFL);
        execlp(xf->name,xf->name,NULL);
        exit(-1);
    }

    close(finfd[0]);
    close(foutfd[1]);

    xf->infd=finfd[1];
    xf->outfd=foutfd[0];
    xf->child = pid;

    pthread_create(&xf->read_thread,NULL,(void *)xfilter_read,(void *) xf);
    pthread_create(&xf->write_thread,NULL,(void *)xfilter_write,(void *) xf);
    return(0);
}

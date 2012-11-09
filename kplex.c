/* kplex: An anything to anything boat data multiplexer for Linux
 * Currently this program only supports nmea-0183 data.
 * For currently supported interfaces see kplex_mods.h
 * Copyright Keith Young 2012
 * For copying information, see the file COPYING distributed with this file
 */

/* This file (kplex.c) contains the main body of the program and
 * central multiplexing engine. Initialisation and read/write routines are
 * defined in interface-specific files
 */

#include "kplex.h"
#include "kplex_mods.h"
#include <signal.h>
#include <unistd.h>
#include <pwd.h>


void terminate (int sig)
{}

/*
 *  Initialise an ioqueue
 *  Args: size of queue (in senblk structures)
 *  Returns: pointer to new queue
 */

ioqueue_t *init_q(size_t size)
{
    ioqueue_t *newq;
    senblk_t *sptr;
    int    i;
    if ((newq=(ioqueue_t *)malloc(sizeof(ioqueue_t))) == NULL)
        return(NULL);
    if ((newq->base=(senblk_t *)calloc(size,sizeof(senblk_t))) ==NULL) {
        free(newq);
        return(NULL);
    }

    /* "base" always points to the allocated memory so that we can free() it.
     * All senblks initially allocated to the free list
     */
    newq->free=newq->base;

    /* Initiailise senblk queue pointers */
    for (i=0,sptr=newq->free,--size;i<size;++i,++sptr)
        sptr->next=sptr+1;

    sptr->next = NULL;

    newq->qhead = newq->qtail = NULL;

    pthread_mutex_init(&newq->q_mutex,NULL);
    pthread_cond_init(&newq->freshmeat,NULL);

    newq->active=1;
    return(newq);
}

/*
 *  Copy information in a senblk structure (data and len only)
 *  Args: pointers to dest and source senblk structures
 *  Returns: pointer to dest senblk
 */
senblk_t *senblk_copy(senblk_t *dptr,senblk_t *sptr)
{
    dptr->len=sptr->len;
    dptr->src=sptr->src;
    dptr->next=NULL;
    return (senblk_t *) memcpy((void *)dptr->data,(const void *)sptr->data,
            sptr->len);
}

/*
 * Add an senblk to an ioqueue
 * Args: Pointer to senblk and Pointer to queue it is to be added to
 * Returns: None
 */
void push_senblk(senblk_t *sptr, ioqueue_t *q)
{
    senblk_t *tptr;

    pthread_mutex_lock(&q->q_mutex);

    if (sptr == NULL) {
        /* NULL senblk pointer is magic "off" switch for a queue */
        q->active = 0;
    } else {
        /* Get a senblk from the queue's free list if possible...*/
        if (q->free) {
            tptr=q->free;
            q->free=q->free->next;
        } else {
            /* ...if not steal from the head of the queue, dropping previous
               contents. Should probably keep a counter for this */
            tptr=q->qhead;
            q->qhead=q->qhead->next;
        }
    
        (void) senblk_copy(tptr,sptr);
    
        /* If there is anything on the queue already, set it's "next" member
           to point to the new senblk */
        if (q->qtail)
            q->qtail->next=tptr;
    
        /* Set tail pointer to the new senblk */
        q->qtail=tptr;
    
        /* queue head needs to point to new senblk if there was nothing
           previously on the queue */
        if (q->qhead == NULL)
            q->qhead=tptr;
    
    }
    pthread_cond_broadcast(&q->freshmeat);
    pthread_mutex_unlock(&q->q_mutex);
}

/*
 *  Get the next senblk from the head of a queue
 *  Args: Queue to retrieve from
 *  Returns: Pointer to next senblk on the queue or NULL if the queue is
 *  no longer active
 *  This function blocks until data are available or the queue is shut down
 */
senblk_t *next_senblk(ioqueue_t *q)
{
    senblk_t *tptr;

    pthread_mutex_lock(&q->q_mutex);
    while ((tptr = q->qhead) == NULL) {
        /* No data available for reading */
        if (!q->active) {
            /* Return NULL if the queue has been sgut down */
            pthread_mutex_unlock(&q->q_mutex);
            return ((senblk_t *)NULL);
        }
        /* Wait until something is available */
        pthread_cond_wait(&q->freshmeat,&q->q_mutex);
    }

    /* set qhead to next element (which may be NULL)
       If the last element in the queue, set the tail pointer to NULL too */
    if ((q->qhead=tptr->next) == NULL)
        q->qtail=NULL;
    pthread_mutex_unlock(&q->q_mutex);
    return(tptr);
}

/*
 * Return a senblk to a queue's free list
 * Args: pointer to senblk, and pointer to the queue whose free list it is to
 * be added to
 * Returns: Nothing
 */
void senblk_free(senblk_t *sptr, ioqueue_t *q)
{
    pthread_mutex_lock(&q->q_mutex);
    /* Adding to head of free list is quicker than tail */
    sptr->next = q->free;
    q->free=sptr;
    pthread_mutex_unlock(&q->q_mutex);
}

iface_t *get_default_global()
{
    iface_t *ifp;

    if ((ifp = (iface_t *) malloc(sizeof(iface_t))) == NULL)
        return(NULL);

    ifp->type = GLOBAL;
    ifp->options = NULL;

    return(ifp);
}

/*
 * This is the heart of the multiplexer.  All inputs add to the tail of the
 * Engine's queue.  The engine takes from the head of its queue and copies
 * to all outputs on its output list.
 * Args: Pointer to information structure (iface_t, cast to void)
 * Returns: Nothing
 */
void *engine(void *info)
{
    senblk_t *sptr;
    iface_t *optr;
    iface_t *eptr = (iface_t *)info;
    int retval=0;

    (void) pthread_detach(pthread_self());

    for (;;) {
        sptr = next_senblk(eptr->q);
        pthread_mutex_lock(&eptr->lists->io_mutex);
        /* Traverse list of outputs and push a copy of senblk to each */
        for (optr=eptr->lists->outputs;optr;optr=optr->next) {
            if ((optr->direction == OUT) && ((!sptr) || (sptr->src != optr->pair))) {
                push_senblk(sptr,optr->q);
            }
        }
        pthread_mutex_unlock(&eptr->lists->io_mutex);
        if (sptr==NULL)
            /* Queue has been marked inactive */
            break;
        senblk_free(sptr,eptr->q);
    }
    pthread_exit(&retval);
}

/*
 * Interface startup routine
 * Args: Pointer to interface structure (cast to void)
 * Returns: Nothing
 */
void start_interface (void *ptr)
{
    iface_t *ifa = ptr;

    link_interface(ifa);
    if (ifa->direction == IN)
        ifa->read(ifa);
    else
        ifa->write(ifa);
}

/*
 * Add an interface structure to an iolist, input or output, depending on
 * direction
 * Args: Pointer to interface structure
 * 0. Other return vals a possible later addition
 */
int link_interface(iface_t *ifa)
{
    iface_t **lptr;
    iface_t **iptr;
    int ret=0;
    sigset_t set,saved;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, &saved);
    pthread_mutex_lock(&ifa->lists->io_mutex);
    ifa->tid = pthread_self();

    if (ifa->direction == NONE) {
        pthread_mutex_unlock(&ifa->lists->io_mutex);
        pthread_sigmask(SIG_SETMASK,&set, &saved);
        iface_destroy(ifa,(void *)&ret);
    }

    for (iptr=&ifa->lists->initialized;*iptr!=ifa;iptr=&(*iptr)->next)
        if (*iptr == NULL) {
            perror("interface does not exist on initialized list!");
            exit(1);
        }

    *iptr=(*iptr)->next;

    /* Set lptr to point to the input or output list, as appropriate */
    lptr=(ifa->direction==IN)?&ifa->lists->inputs:&ifa->lists->outputs;
    if (*lptr)
        ifa->next=(*lptr);
    else
        ifa->next=NULL;
    (*lptr)=ifa;

    if (ifa->lists->initialized == NULL)
        pthread_cond_signal(&ifa->lists->init_cond);
    pthread_mutex_unlock(&ifa->lists->io_mutex);
    pthread_sigmask(SIG_SETMASK,&saved,NULL);
    return(0);
}

/*
 * link an interface into the initialized list
 * Args: interface structure pointer
 * Returns: 0 on success. There is no failure condition
 * Side Effects: links interface to the initialized list
 */
int link_to_initialized(iface_t *ifa)
{
    iface_t **iptr;

    pthread_mutex_lock(&ifa->lists->io_mutex);
    for (iptr=&ifa->lists->initialized;(*iptr);iptr=&(*iptr)->next);
    (*iptr)=ifa;
    ifa->next=NULL;
    pthread_mutex_unlock(&ifa->lists->io_mutex);
    return(0);
}

/*
 * Take an interface off the input or output iolist and place it on the "dead"
 * list waiting to be cleaned up
 * Args: Pointer to interface structure
 * Returns: 0 on success. Might add other possible return vals later
 * Should this be broken into link from input/output then link to dead?
 */
int unlink_interface(iface_t *ifa)
{
    iface_t **lptr;
    iface_t *tptr;

    sigset_t set,saved;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, &saved);
    pthread_mutex_lock(&ifa->lists->io_mutex);
    /* Set lptr to point to the input or output list, as appropriate */
    lptr=(ifa->direction==IN)?&ifa->lists->inputs:&ifa->lists->outputs;
    if ((*lptr) == ifa) {
        /* If target interface is the head of the list, set the list pointer
           to point to the next interface in the list */
        (*lptr)=(*lptr)->next;
    } else {
        /* Traverse the list until we find the interface before our target and
           make its next pointer point to the element after our target */
        for (tptr=(*lptr);tptr->next != ifa;tptr=tptr->next);
            tptr->next = ifa->next;
    }

    if ((ifa->direction != IN) && ifa->q) {
        /* output interfaces have queues which need freeing */
        free(ifa->q->base);
        free(ifa->q);
    } else {
        if (!ifa->lists->inputs) {
        pthread_mutex_lock(&ifa->q->q_mutex);
            ifa->q->active=0;
            pthread_cond_broadcast(&ifa->q->freshmeat);
        pthread_mutex_unlock(&ifa->q->q_mutex);
        }
    }
    ifa->cleanup(ifa);
    free(ifa->info);
    if (ifa->pair) {
        ifa->pair->pair=NULL;
        if (ifa->pair->direction == OUT) {
            pthread_mutex_lock(&ifa->pair->q->q_mutex);
            ifa->pair->q->active=0;
            pthread_cond_broadcast(&ifa->pair->q->freshmeat);
            pthread_mutex_unlock(&ifa->pair->q->q_mutex);
        } else {
            ifa->pair->direction = NONE;
            if (ifa->pair->tid)
                pthread_kill(ifa->pair->tid,SIGUSR1);
        }
    }

    /* Add to the dead list */
    if ((tptr=ifa->lists->dead) == NULL)
        ifa->lists->dead=ifa;
    else {
        for(;tptr->next;tptr=tptr->next);
    tptr->next=ifa;
    }
    ifa->next=NULL;
    /* Signal the reaper thread */
    pthread_cond_signal(&ifa->lists->dead_cond);
    pthread_mutex_unlock(&ifa->lists->io_mutex);
    pthread_sigmask(SIG_SETMASK,&saved,NULL);
    return(0);
}

/*
 * Shut down an interface
 * Args: Pointer to interface structure and pointer to code giving reason
 * for shut down
 * Retruns: Nothing
 * This just unlinks the interface and calls pthread_exit
 */
void iface_destroy(iface_t *ifa,void * ret)
{
    unlink_interface(ifa);
    pthread_exit(ret);
}

/*
 * Duplicate an interface
 * Used when creating IN/OUT pair for bidirectional communication
 * Args: pointer to interface to be duplicated
 * Returns: Pointer to duplicate interface
 */
iface_t *ifdup (iface_t *ifa)
{
    iface_t *newif;

    if ((newif=(iface_t *) malloc(sizeof(iface_t))) == (iface_t *) NULL)
        return(NULL);
    if (iftypes[ifa->type].ifdup_func) {
        if ((newif->info=(*iftypes[ifa->type].ifdup_func)(ifa->info)) == NULL) {
            free(newif);
            return(NULL);
        }
    } else
        newif->info = NULL;

    ifa->pair=newif;
    newif->pair=ifa;
    newif->next=NULL;
    newif->type=ifa->type;
    newif->lists=ifa->lists;
    newif->read=ifa->read;
    newif->write=ifa->write;
    newif->cleanup=ifa->cleanup;
    newif->options=NULL;
    return(newif);
}

/*
 * Return the path to the kplex config file
 * Args: None
 * Returns: pointer to name of config file
 *
 * First choice is conf file in user's home directory, seocnd is global
 */
char *get_def_config()
{
    char *confptr;
    char *buf;
    struct passwd *pw;

    if (confptr=getenv("KPLEXCONF"))
        return (confptr);
    if ((confptr=getenv("HOME")) == NULL)
        if (pw=getpwuid(getuid()))
            confptr=pw->pw_dir;
    if (confptr) {
        if ((buf = malloc(strlen(confptr)+strlen(KPLEXHOMECONF)+2)) == NULL) {
            perror("failed to allocate memory");
            exit(1);
        }
        strcpy(buf,confptr);
        strcat(buf,"/");
        strcat(buf,KPLEXHOMECONF);
        if (!access(buf,F_OK))
            return(buf);
        free(buf);
    }
    if (!access(KPLEXGLOBALCONF,F_OK))
        return(KPLEXGLOBALCONF);
    return(NULL);
}

main(int argc, char ** argv)
{
    pthread_t tid;
    char *config=NULL;
    iface_t  *e_info;
    iface_t *ifptr,*ifptr2;
    iface_t **tiptr;
    int opt,err=0;
    struct kopts *option;
    int qsize=0;
    void *ret;
    struct iolists lists = {
        .io_mutex = PTHREAD_MUTEX_INITIALIZER,
        .dead_mutex = PTHREAD_MUTEX_INITIALIZER,
        .dead_cond = PTHREAD_COND_INITIALIZER,
    .init_cond = PTHREAD_COND_INITIALIZER,
    .initialized = NULL,
    .outputs = NULL,
    .inputs = NULL,
    .dead = NULL
    };

    /* command line argument processing */
    while ((opt=getopt(argc,argv,"q:f:")) != -1) {
        switch (opt) {
            case 'q':
                if ((qsize=atoi(optarg)) < 2) {
                    fprintf(stderr,"%s: Minimum qsize is 2\n",
                            argv[0]);
                    err++;
                }
                break;
            case 'f':
                config=optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s [-q <size> ] [ -f <config file>] [<interface specification> ...]\n",argv[0]);
                err++;
        }
    }
    if (err)
        exit(1);

    /* If a config file is specified by a commad line argument, read it.  If
     * not, look for a default config file unless told not to using "-f-" on the
     * command line
     */
    if ((config && (strcmp(config,"-"))) ||
            (!config && (config = get_def_config()))) {
        if ((e_info=parse_file(config)) == NULL) {
            fprintf(stderr,"Error parsing config file: %s\n",errno?
                    strerror(errno):"Syntax Error");
            exit(1);
        }
    } else
        /* global options for engine configuration are also returned in config
         * file parsing. If we didn't do that, get default options here */
        e_info = get_default_global();

    /* queue size is taken from (in order of preference), command line arg, 
     * config file option in [global] section, default */
    if (!qsize) {
        if (e_info->options) {
            for (option=e_info->options;option;option=option->next);
                if (!strcmp(option->var,"qsize")) {
                    if(!(qsize = atoi(option->val))) {
                        fprintf(stderr,"Invalid queue size: %s\n",option->val);
                        exit(1);
                    }
                }
        }
    }

    if ((e_info->q = init_q(qsize?qsize:DEFQUEUESZ)) == NULL) {
        perror("failed to initiate queue");
        exit(1);
    }

    e_info->lists = &lists;
    lists.engine=e_info;

    if (e_info->options)
        free_options(e_info->options);

    for (tiptr=&e_info->next;optind < argc;optind++) {
        if (!(ifptr=parse_arg(argv[optind]))) {
            fprintf(stderr,"Failed to parse interface specifier %s\n",
                    argv[optind]);
            exit(1);
        }
        ifptr->next=(*tiptr);
        (*tiptr)=ifptr;
        tiptr=&ifptr->next;
    }
    /* our list of "real" interfaces starts after the first which is the
     * dummy "interface" specifying the multiplexing engine
     * walk the list, initialising the interfaces.  Sometimes "BOTH" interfaces
     * are initialised to one IN and one OUT which then need to be linked back
     * into the list
     */
    for (ifptr=e_info->next,tiptr=&lists.initialized;ifptr;ifptr=ifptr2) {
        ifptr2 = ifptr->next;
        if ((ifptr=(*iftypes[ifptr->type].init_func)(ifptr)) == NULL) {
            fprintf(stderr, "Failed to initialize Interface\n");
            exit(1);
        }

        for (;ifptr;ifptr = ifptr->next) {
        /* This loop should be done once for IN or OUT interfaces twice for
         * interfaces where the initialisation routine has expanded them to an
         * IN/OUT pair.
         */
            if (ifptr->direction != OUT)
                ifptr->q=e_info->q;

            ifptr->lists = &lists;
            (*tiptr)=ifptr;
            tiptr=&ifptr->next;
            if (ifptr->next==ifptr2)
                ifptr->next=NULL;
        }
    }
    pthread_create(&tid,NULL,engine,(void *) e_info);

    signal(SIGUSR1,terminate);
    pthread_mutex_lock(&lists.io_mutex);
    for (ifptr=lists.initialized;ifptr;ifptr=ifptr->next) {
        /* Create a thread to run each output */
            pthread_create(&tid,NULL,(void *)start_interface,(void *) ifptr);
    }

    /* While there are remaining outputs, wait until something is added to the 
     * dead list, reap everything on the dead list and check for outputs again
     * until all the outputs have been reaped
     * Note that when there are no more inputs, unlink_interface will set the
     * engine's queue inactive causing it to set all the outputs' queues
     * inactive and shutting them down. Thus the last input exiting also shuts
     * everything down */
    while (lists.outputs || lists.initialized) {
        while (lists.dead  == NULL) {
            pthread_cond_wait(&lists.dead_cond,&lists.io_mutex);
	}
        for (ifptr=lists.dead;ifptr;ifptr=lists.dead) {
        	lists.dead=ifptr->next;
                pthread_join(ifptr->tid,&ret);
                free(ifptr);
        }
        pthread_mutex_unlock(&lists.io_mutex);
    }
fprintf(stderr,"no more outputs\n");
    exit(0);
}

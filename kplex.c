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
#include <unistd.h>

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

/*
 * This is the heart of the multiplexer.  All inputs add to the tail of the
 * Engine's queue.  The engine takes from the head of its queue and copies
 * to all outputs on its output list.
 * Args: Pointer to information structure (struct engine_info, cast to void)
 * Returns: Nothing
 */
void *engine(void *info)
{
    senblk_t *sptr;
    iface_t *optr;
    struct engine_info *eptr = (struct engine_info *)info;
    int retval=0;

    (void) pthread_detach(pthread_self());

    for (;;) {
        sptr = next_senblk(eptr->q);
    	pthread_mutex_lock(&eptr->lists->io_mutex);
        /* Traverse list of outputs and push a copy of senblk to each */
        for (optr=eptr->lists->outputs;optr;optr=optr->next) {
            if (optr->q)
                push_senblk(sptr,optr->q);
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

    ifa->tid = pthread_self();

    link_interface(ifa);

    if (ifa->direction == OUT)
    	ifa->write(ifa);
    else
	ifa->read(ifa);

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

    pthread_mutex_lock(&ifa->lists->io_mutex);
    /* Set lptr to point to the input or output list, as appropriate */
    lptr=(ifa->direction==IN)?&ifa->lists->inputs:&ifa->lists->outputs;
    if (*lptr)
        ifa->next=(*lptr);
    else
        ifa->next=NULL;
    (*lptr)=ifa;
    if (ifa->direction == OUT)
	    if (--ifa->lists->uninitialized == 0)
		    pthread_cond_signal(&ifa->lists->init_cond);
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

    pthread_mutex_lock(&ifa->lists->io_mutex);
    /* Set lptr to point to the input or output list, as appropriate */
    lptr=(ifa->direction==IN)?&ifa->lists->inputs:&ifa->lists->outputs;

    if ((*lptr) == ifa) {
        /* If target interface is the head of the list, set the list pointer
           to point to the next interface in the list */
	    (*lptr)=(*lptr)->next;
    } else {
        /* Traverse the list until we find the interface before our target and
           make it's next pointer point to the element after our target */
	    for (tptr=(*lptr);tptr->next != ifa;tptr=tptr->next);
	        tptr->next = ifa->next;
    }
    if ((ifa->direction == OUT) && ifa->q) {
        /* output interfaces have queues which need freeing */
        free(ifa->q->base);
        free(ifa->q);
    } else {
        if (!ifa->lists->inputs) {
	    ifa->q->active=0;
	    pthread_cond_broadcast(&ifa->q->freshmeat);
	}
    }
    pthread_mutex_unlock(&ifa->q->q_mutex);
    ifa->cleanup(ifa);
    free(ifa->info);
    pthread_mutex_lock(&ifa->lists->dead_mutex);

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
    pthread_mutex_unlock(&ifa->lists->dead_mutex);
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
 * Interface initialisation routine. Check an interface specification string
 * to determine the direction (input or output) and type f the interface and
 * calls the interface-specific initialisation routine
 * Args: String specifying the interface
 * Returns: Pointer to interface structure
 */
iface_t *init_interface(char *dev)
{
	char *ptr;
	int len,err=0;
	iface_t *newif;

    if ((len=strlen(dev)) < 4) {
        fprintf(stderr,"interface specifier %s too short\n",dev);
        exit(1);
    }

	if ((newif = malloc(sizeof(iface_t))) == NULL) {
		perror("Could not allocate memory");
		exit (1);
	}


	switch (*dev) {
	case 'i':
	case 'I':
		newif->direction=IN;
		break;
	case 'o':
	case 'O':
		newif->direction=OUT;
		break;
	default:
		err++;
	}

	if (err || (*(dev+1) != ',') || (*(dev+3) != ',')) {
		fprintf(stderr,"Incorrect interface specification '%s'\n",dev);
		exit(1);
	}
	switch(*(dev+2)) {
	case 's':
	case 'S':
		newif->type=SERIAL;
		break;
	case 't':
	case 'T':
		newif->type=TCP;
		break;
	case 'b':
	case 'B':
		newif->type=BCAST;
		break;
	case 'f':
	case 'F':
		newif->type=FILEIO;
		break;
	case 'p':
	case 'P':
		newif->type=PTY;
		break;
	case 'r':
	case 'R':
		newif->type=ST;
		break;
	default:
		fprintf(stderr,"Unknown interface specification '%s'\n",dev);
		exit(1);
	}

	return((*init_func[newif->type])(dev,newif));
}

main(int argc, char ** argv)
{
    pthread_t tid;
    struct engine_info e_info;
    iface_t *ifptr,*iflist;
    iface_t **tiptr;
    size_t qsize=DEFQUEUESZ;                            /* For engine only */
    int opt,err=0;;
    void *ret;
    struct iolists lists = {
        .io_mutex = PTHREAD_MUTEX_INITIALIZER,
        .dead_mutex = PTHREAD_MUTEX_INITIALIZER,
        .dead_cond = PTHREAD_COND_INITIALIZER,
	.init_cond = PTHREAD_COND_INITIALIZER,
	.uninitialized = 0,
	.outputs = NULL,
	.inputs = NULL,
	.dead = NULL
    };

    while ((opt=getopt(argc,argv,"q:")) != -1) {
        switch (opt) {
            case 'q':
                if ((qsize=atoi(optarg)) < 2) {
                    fprintf(stderr,"%s: Minimum qsize is 2\n",
                            argv[0]);
                    err++;
                }
                break;
            default:
                fprintf(stderr, "Usage: %s\n",argv[0]);
                err++;
        }
    }
    if (err)
        exit(1);

    e_info.q = init_q(qsize);

    for (ifptr=iflist=NULL,tiptr=&iflist;optind < argc;optind++) {
        /* Remaining arguments should all be interfaces */
        if ((ifptr=init_interface(argv[optind])) == NULL) {
            fprintf(stderr,"%s: Failed to initialize interface specified by \'%s\'\n",argv[0],argv[optind]);
            exit(1);
        }

	(*tiptr) = ifptr;
	tiptr=&ifptr->next;
	if (ifptr->direction == IN)
		ifptr->q=e_info.q;
	else
		lists.uninitialized++;

        ifptr->lists = &lists;
    }

    e_info.lists = &lists;

    pthread_create(&tid,NULL,engine,(void *) &e_info);
    for (ifptr=iflist,tiptr=&ifptr;ifptr;ifptr=ifptr->next)
        /* Create a thread to run each output */
        if (ifptr->direction == OUT) {
            pthread_create(&tid,NULL,(void *)start_interface,(void *) ifptr);
	    *tiptr=ifptr->next;
	} else
            tiptr=(&ifptr->next);

    /* I do so love a good linked list */

    for (ifptr=iflist;ifptr;ifptr=ifptr->next) {
        /* Create a thread to run each input */
        pthread_create(&tid,NULL,(void *)start_interface,(void *) ifptr);
    }

    pthread_mutex_lock(&lists.io_mutex);
    while (lists.uninitialized)
	    pthread_cond_wait(&lists.init_cond,&lists.io_mutex);
    pthread_mutex_unlock(&lists.io_mutex);

    /* While there are remaining outputs, wait until something is added to the 
     * dead list, reap everything on the dead list and check for outputs again
     * until all the outputs have been reaped
     * Note that when there are no more inputs, unlink_interface will set the
     * engine's queue inactive causing it to set all the outputs' queues
     * inactive and shutting them down. Thus the last input exiting also shuts
     * everything down */
    while (lists.outputs) {
        pthread_mutex_lock(&lists.dead_mutex);
        while (lists.dead  == NULL)
            pthread_cond_wait(&lists.dead_cond,&lists.dead_mutex);
        for (ifptr=lists.dead;ifptr;ifptr=lists.dead) {
            pthread_join(ifptr->tid,&ret);
            lists.dead=ifptr->next;
            free(ifptr);
    	}
        pthread_mutex_unlock(&lists.dead_mutex);
    }
    exit(0);
}

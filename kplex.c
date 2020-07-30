/* kplex: An anything to anything boat data multiplexer for Linux
 * Currently this program only supports nmea-0183 data.
 * For currently supported interfaces see kplex_mods.h
 * Copyright Keith Young 2012-2020
 * For copying information, see the file COPYING distributed with this file
 */

/* This file (kplex.c) contains the main body of the program and
 * central multiplexing engine. Initialisation and read/write routines are
 * defined in interface-specific files
 */

#include "kplex.h"
#include "kplex_mods.h"
#include "version.h"
#include <signal.h>
#include <pwd.h>
#include <time.h>
#include <syslog.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <inttypes.h>
#include <fcntl.h>
#include <locale.h>

/* Macro to identify kplex Proprietary sentences */
#define isprop(sptr) ((sptr) && sptr->len >= 7 && sptr->data[1] == 'P' && sptr->data[2] == 'K' && sptr->data[3] == 'P' && sptr->data[4] == 'X')

/* Message catalogue location passed from Makefile */
#ifndef SHAREDIR
#define SHAREDIR "/usr/share/kplex"
#endif

/* Globals. Sadly. Used in signal handlers so few other simple options */
pthread_key_t ifkey;    /* Key for Thread local pointer to interface struct */
pthread_t reaper;       /* tid of thread responsible for reaping */
int timetodie=0;        /* Set on receipt of SIGTERM or SIGINT */
time_t graceperiod=3;   /* Grace period for unsent data before shutdown (secs)*/
int debuglevel=0;       /* debug off by default */
nl_catd cat;            /* i8n catalogue */

/* Signal handler for SIGUSR1 used by interface threads.  Note that this is
 * highly dubious: pthread_exit() is not async safe.  No associated problems
 * reported so far and if they do occur they should occur on exit, but this
 * will be changed in the next release
 */
void terminate(int sig)
{
    pthread_exit((void *)&sig);
}

/* Sleep function not relying on SIGALRM for thread safety
 * Unnecessary on many platforms but here to minimise portability issues
 * Could do this with nanosleep() or select()
 */
int mysleep(time_t sleepytime)
{
    struct timespec rqtp;

    rqtp.tv_sec = sleepytime;
    rqtp.tv_nsec=0;

    return (nanosleep(&rqtp,NULL));
}

/* functions */

/*
 * Check an NMEA 0183 checksum
 * Args: pointer to struct senblk, checking/addition required
 * Returns: 0 if check/addition "successful", -1 otherwise
 */
int checkcksum (senblk_t *sptr, enum cksm how)
{
    int cksm=0;
    int rcvdcksum=0,i,end;
    char *ptr;

    /* Shouldn't be necessary but for safety... */
    switch (how) {
        case CKSM_STRICT:
        case CKSM_LOOSE:
        case CKSM_ADD:
        case CKSM_ADDONLY:
            break;
        default:
            return(0);
            break;
    }

    for(i=0,end=sptr->len-6,ptr=sptr->data+1; i < end;i++)
            cksm ^= *ptr++;

    if (*ptr != '*') {
    /* There's no checksum or it's incomplete */
        if (how == CKSM_STRICT) {
            return(-1);
        }

        for(end=sptr->len-3;i < end;i++) {
            cksm ^= *ptr++;
            if (*ptr == '*') {
            /* There's an incomplete checksum */
                if (how == CKSM_ADDONLY) {
                /* for "addonly" we don't add but don't reject */
                    return 0;
                }
                /* CKSM_LOOSE or CKSM_ADD reject bad checksums */
                return(-1);
            }
        }

        /* No checksum */
        if (how == CKSM_LOOSE) {
            return(0);
        }

        /* We can't add if the sentence is already too long */
        if (sptr->len > SENMAX - 1) {
            return(-1);
        }

        /* Add the checksum */
        *ptr++ = '*';
        *ptr++ = ((i = cksm / 16) > 9)?i + 55:i + 48;
        *ptr++ = ((i = cksm % 16) > 9)?i + 55:i + 48;
        *ptr++ = '\r';
        *ptr++ = '\n';

        sptr->len += 3;

        return(0);
    }

    /* ADDONLY doesn't care if a checksum is wrong */
    if (how == CKSM_ADDONLY) {
        return(0);
    }

    for (i=0,++ptr;i<2;i++,ptr++) {
        if (*ptr>47 && *ptr<58)
            rcvdcksum+=*ptr-48;
        else if (*ptr>64 && *ptr<71)
             rcvdcksum+=*ptr-55;
        else if (*ptr>96 && *ptr<103)
            rcvdcksum+=*ptr-87;
        if (!i)
            rcvdcksum<<=4;
    }

    if (cksm == rcvdcksum)
        return (0);
    else
        return(-1);
}

/*
 * Perform filtering on sentences
 * Args: senblk to be filtered, pointer to filter
 * Returns: 0 if contents of senblk passes filter, -1 otherwise
 */
int senfilter(senblk_t *sptr, sfilter_t *filter)
{
    unsigned int mask = (unsigned int) -1 ^ IDMINORMASK;
    sf_rule_t *fptr;
    char *cptr;
    int i;
    time_t tsecs;
    struct timeval tv;

    /* We shouldn't actually be filtering any NULL packets, but check anyway */
    if (sptr == NULL || filter == NULL || filter->rules == NULL)
        return(0);

    /* inputs should have ensured all sentences ended with \r\n so if we check
     * for \r here, we only have to check for \r in the for loops, not \n too */
    if (*sptr->data == '\r')
        return(1);

    for (fptr=filter->rules;fptr;fptr=fptr->next) {
        if ((fptr->src.id) && (fptr->src.id != (sptr->src&mask)))
            continue;
        for (i=0,cptr=sptr->data+1;i<5 && *cptr != '\r';i++,cptr++)
            if(fptr->match[i] && fptr->match[i] != *cptr)
                break;
        if (i!=5)
            continue;

        if (fptr->type == ACCEPT) {
            return(0);
        }
        if (fptr->type == DENY) {
            return(-1);
        }
        /* type is limit. Hopefully. */
        (void) gettimeofday(&tv,NULL);
        if (tv.tv_sec < fptr->info.limit->timeout)
            return(-1);
        if ((tsecs=(tv.tv_sec - fptr->info.limit->timeout)) <
                fptr->info.limit->last.tv_sec)
            return(-1);
        if (tsecs == fptr->info.limit->last.tv_sec &&
                (tv.tv_usec < fptr->info.limit->last.tv_usec ))
            return(-1);
        /* at least timeout since last seen: Update info and pass */
        memcpy(&fptr->info.limit->last,&tv,sizeof(struct timeval));
        return(0);
    }
    return(0);
}

/*
 * Free a failover rule and any attached source list
 * Args: Pointer to rule structure
 * Returns: Nothing
 */
void free_srclist(struct srclist *src)
{
    struct srclist *tsrc;

    for (;src;src=tsrc) {
        tsrc=src->next;
        if (src->src.name)
            free(src->src.name);
        free(src);
    }
}

/*
 * Free a filter
 * Args: pointer to filter to be freed
 * Returns: Nothing
 */
void free_filter(sfilter_t *fptr)
{
    sf_rule_t *rptr,*trptr;

    if (fptr == NULL)
        return;

    pthread_mutex_lock(&fptr->lock);
    if (--fptr->refcount) {
        pthread_mutex_unlock(&fptr->lock);
        return;
    }

    if (fptr->rules)
        for (rptr=fptr->rules;rptr;rptr=trptr) {
            trptr=rptr->next;
            if (fptr->type == FAILOVER)
                free_srclist(rptr->info.source);
            free(rptr);
        }

    free(fptr);
}

/*
 * Add a failover source to a failover rule
 * Args: address of head of the source list and new source item
 * Returns: Nothing
 * Side Effect: source item linked into source list according to failover time
 */
void link_src_to_rule (struct srclist **list, struct srclist *src)
{
    for (;*list;list=&(*list)->next)
        if ((*list)->failtime > src->failtime)
            break;
    src->next=(*list);
    *list=src;
}

/*
 * Test if a sentence came from a failover input that is active
 * Args: Pointer to filter head, pointer to senblk to be tested
 * Returns: 1 if  senblk should be passed, 0 if not
 */
int isactive(sfilter_t *filter,senblk_t *sptr)
{
    time_t now=time(NULL);
    unsigned int mask = (unsigned int) -1 ^ IDMINORMASK;
    unsigned int src;
    char *cptr,*mptr;
    sf_rule_t *rule;
    struct srclist *rptr;
    time_t last;
    int i;

    if (filter == NULL || sptr == NULL)
        return(1);

    src = sptr->src & mask;

    for(rule=filter->rules;rule;rule=rule->next) {
        for (i=0,cptr=sptr->data+1,mptr=rule->match;i<5;i++,cptr++,mptr++)
            if(*mptr && *cptr != *mptr)
                break;
        if (i == 5)
            break;
    }
    if (!rule)
        return(1);
    for (last=0,rptr=rule->info.source;rptr;rptr=rptr->next) {
        if (rptr->src.id == src) {
            rptr->lasttime = now;
            if (last+rptr->failtime < now)
                return(1);
            else
                return(0);
        }
        if (rptr->lasttime > last)
            last = rptr->lasttime;
    }
    return(0);
}
/*
 *  Add a failover specification
 *  Args: address of ofilter pointer and pointer to string containing failover
 *  spec
 *  Returns: 0 on success, -1 on error
 *  Side effects: New Failover added to ofilter
 */
int addfailover(sfilter_t **head,char *spec)
{
    sf_rule_t *newrule;
    struct srclist *src;
    char *cptr,*nptr;
    int n,done;
    time_t now;

    if ((newrule=(sf_rule_t *)malloc(sizeof(sf_rule_t))) == NULL) {
        return(-1);
    }
    newrule->info.source=NULL;

    for (errno=0,cptr=spec,n=0;n<5;spec++,n++,cptr++) {
        if (!*cptr || *cptr== ':') {
            free(newrule);
            return(-1);
        }
        newrule->match[n] = (*cptr == '*')?0:*cptr;
    }

    if (*cptr++ != ':') {
        free(newrule);
        return(-1);
    }
    for (now=time(NULL),done=0;!done && *cptr;src=NULL,cptr++) {
        if ((src=(struct srclist *)malloc(sizeof(struct srclist))) == NULL) {
            free(newrule);
            return(-1);
        }
        for(src->failtime=0;*cptr && *cptr >= '0' && *cptr <= '9';cptr++)
            src->failtime=src->failtime*10+(*cptr - '0');

        if (*cptr++ != ':')
            break;

        for(nptr=cptr;*cptr && *cptr != ':';)
            cptr++;
        if (*cptr)
            *cptr='\0';
        else
            done++;

        if ((src->src.name = strdup(nptr)) == NULL) {
            logerr(errno,catgets(cat,2,32,
                    "Failed to allocate memory for string duplication"));
            free(newrule);
            return(-1);
        }

        src->lasttime=now;
        link_src_to_rule(&newrule->info.source,src);
    }
    if (done) {
        if (!*head) {
            if (((*head)=(sfilter_t *)malloc(sizeof(sfilter_t)))) {
                (*head)->type=FAILOVER;
                (*head)->refcount=1;
                pthread_mutex_init(&(*head)->lock,NULL);
                (*head)->rules=NULL;
            }
        }
        if (*head) {
            newrule->next=(*head)->rules;
            (*head)->rules=newrule;
            return(0);
        }
    }
    if (src)
        free(src);
    if (newrule->info.source)
        free_srclist(newrule->info.source);
    free(newrule);
    return(-1);
}


/*
 * Exit function used by interface handlers.  Interface objects are cleaned
 * up by the destructor functions of thread local storage
 * Args: exit status (unused)
 * Returns: Nothing
 */
void iface_thread_exit(int ret)
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set,SIGUSR1);
    pthread_sigmask(SIG_BLOCK,&set,NULL);

    pthread_exit((void *)&ret);
}

/*
 *  Initialise an ioqueue
 *  Args: iface_t to add queue to, size of queue (in senblk structures)
 *  Returns: 0 on success, -1 on failure
 */
int init_q(iface_t *ifa, size_t size)
{
    ioqueue_t *newq;
    senblk_t *sptr;
    int    i;
    if ((newq=(ioqueue_t *)malloc(sizeof(ioqueue_t))) == NULL)
        return(-1);
    if ((newq->base=(senblk_t *)calloc(size,sizeof(senblk_t))) ==NULL) {
        i=errno;
        free(newq);
        errno=i;
        return(-1);
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
    newq->owner=ifa;

    pthread_mutex_init(&newq->q_mutex,NULL);
    pthread_cond_init(&newq->freshmeat,NULL);

    newq->active=1;
    ifa->q=newq;
    return(0);
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
               contents. */
            tptr=q->qhead;
            q->qhead=q->qhead->next;
            if (q->drops < 0)
                q->drops++;
            DEBUG(4,catgets(cat,2,33,"Dropped senblk q=%s"),
                    (q->owner->name)?q->owner->name:
                    catgets(cat,2,34,"(unknown)"));
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
            /* Return NULL if the queue has been shut down */
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
 *  Get the last senblk from a queue, discarding all before it
 *  Args: Queue to retrieve from
 *  Returns: Pointer to last senblk on the queue or NULL if the queue is
 *  no longer active
 *  This function blocks until data are available or the queue is shut down
 */
senblk_t *last_senblk(ioqueue_t *q)
{
    senblk_t *tptr,*nptr;

    pthread_mutex_lock(&q->q_mutex);
    /* Move all but last senblk on the queue to the free list */
    if ((tptr=q->qhead) != NULL) {
        for (nptr=tptr->next;nptr;tptr=nptr,nptr=nptr->next) {
            tptr->next=q->free;
            q->free=tptr;
        }
        q->qhead=tptr;
    }

    while ((tptr = q->qhead) == NULL) {
        /* No data available for reading */
        if (!q->active) {
            /* Return NULL if the queue has been shut down */
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
 * Flush a queue, returning anything on it to the free list
 * Args: Queue to be flushed
 * Returns: Nothing
 * Side Effect: Returns anything on the queue to the free list
 */
void flush_queue(ioqueue_t *q)
{
    pthread_mutex_lock(&q->q_mutex);
    if (q->qhead != NULL) {
        q->qtail->next = q->free;
        q->free=q->qhead;
        q->qhead=q->qtail=NULL;
    }
    pthread_mutex_unlock(&q->q_mutex);
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
    struct if_engine *ifg;

    if ((ifp = (iface_t *) malloc(sizeof(iface_t))) == NULL)
        return(NULL);

    memset((void *) ifp,0,sizeof(iface_t));

    ifp->type = GLOBAL;
    ifp->options = NULL;
    if ((ifg = (struct if_engine *)malloc(sizeof(struct if_engine))) == NULL) {
        free(ifp);
        return(NULL);
    }
    ifg->flags=0;
    ifg->logto=LOG_DAEMON;
    ifp->strict=-1;
    ifp->checksum=CKSM_NO;
    ifp->info = (void *)ifg;

    return(ifp);
}

/* Process proprietary sentence.  Anything starting $PKPX
 * Args: senblk_t * containing sentence, iface_t pointing to engine.
 * currently unused but we may use it later for adding to the engine's queue
 * Returns -1 if sentence is unrecognised or invalid, 0 if processing
 * should continue with current senblk, 1 if this senblk should be dropped
 * but sentence was valid
 * Side effects: Swaps Query with Response where appropriate
 */
int process_prop(senblk_t *sptr, iface_t *eptr)
{
    if (sptr->data[6] != ',')
        return(-1);
    switch (sptr->data[5]) {
    case 'Q':
        /* Query Sentence */
        if (sptr->data[7] == 'V') {
             sptr->len=sprintf(sptr->data,"$PKPXR,%s",VERSION);
        } else
            return -1;
        break;
    case 'I':
        /* Informational message from another kplex instance */
        return 1;
    case 'C':
        /* Command: None currently defined */
    case 'R':
        /* Response: shouldn't get this */
    default:
        return -1;
    }

    sptr->len+=sprintf(sptr->data+sptr->len,"*%02X\r\n",
            calcsum(sptr->data+1,sptr->len-1));
    sptr->src=0;
    return(0);
}

/*
 * This is the heart of the multiplexer.  All inputs add to the tail of the
 * Engine's queue.  The engine takes from the head of its queue and copies
 * to all outputs on its output list.
 * Args: Pointer to information structure (iface_t, cast to void)
 * Returns: Nothing
 */
void *run_engine(void *info)
{
    senblk_t *sptr;
    iface_t *optr;
    iface_t *eptr = (iface_t *)info;
    int retval=0;

    (void) pthread_detach(pthread_self());

    for (;;) {
        sptr = next_senblk(eptr->q);

        if (isprop(sptr)) {
            if (process_prop(sptr,eptr)) {
                senblk_free(sptr,eptr->q);
                continue;
            }
        }

        if (isactive(eptr->ofilter,sptr)) {
            pthread_mutex_lock(&eptr->lists->io_mutex);
            /* Traverse list of outputs and push a copy of senblk to each */
            for (optr=eptr->lists->outputs;optr;optr=optr->next) {
                if ((optr->q) && ((!sptr) ||
                        ((sptr->src != optr->id) || (flag_test(optr,F_LOOPBACK))))) {
                    push_senblk(sptr,optr->q);
                }
            }
            pthread_mutex_unlock(&eptr->lists->io_mutex);
        }

        if (sptr == NULL) {
            /* Queue has been marked inactive */
            break;
        }

        senblk_free(sptr,eptr->q);
    }
    pthread_exit(&retval);
}

/*
 * Start processing an interface and add it to an iolist, input or output, 
 * depending on direction
 * Args: Pointer to interface structure (cast to void *)
 * Returns: Nothing
 * We should come into this with SIGUSR1 blocked
 */
void start_interface(void *ptr)
{
    iface_t *ifa = (iface_t *)ptr;
    iface_t **lptr;
    iface_t **iptr;
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);

    pthread_mutex_lock(&ifa->lists->io_mutex);
    ifa->tid = pthread_self();

    if (pthread_setspecific(ifkey,ptr)) {
        perror(catgets(cat,2,35,"Falied to set key"));
        exit(1);
    }

    for (iptr=&ifa->lists->initialized;*iptr!=ifa;iptr=&(*iptr)->next)
        if (*iptr == NULL) {
            perror(catgets(cat,2,36,
                    "interface does not exist on initialized list!"));
            exit(1);
        }

    *iptr=(*iptr)->next;

    /* We've unlinked from initialized. Exit if we've been told to already */
    if (ifa->direction == NONE) {
        pthread_mutex_unlock(&ifa->lists->io_mutex);
        iface_thread_exit(0);
    }

    /* Set lptr to point to the input or output list, as appropriate */
    lptr=(ifa->direction==IN)?&ifa->lists->inputs:&ifa->lists->outputs;
    if (*lptr)
        ifa->next=(*lptr);
    else
        ifa->next=NULL;
    (*lptr)=ifa;

    if (ifa->lists->initialized == NULL)
        pthread_cond_broadcast(&ifa->lists->init_cond);
    else 
        while (ifa->lists->initialized)
            pthread_cond_wait(&ifa->lists->init_cond,&ifa->lists->io_mutex);

    pthread_mutex_unlock(&ifa->lists->io_mutex);
    pthread_sigmask(SIG_UNBLOCK,&set,NULL);
    if (ifa->direction == IN) {
        ifa->read(ifa);
    } else
        ifa->write(ifa);
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
 * Free all the data associated with an interface except the iface_t itself
 * Args: Pointer to iface_t to be freed
 * Returns: Nothing
 * Side Effects: Cleanup routines invoked, de-coupled from any pair, all data
 * other than the main interface structure is freed
 * Because of dealing with the pair, the io_mutex should be locked before
 * involing this routine
 */
void free_if_data(iface_t *ifa)
{
    if ((ifa->direction == OUT) && ifa->q) {
        /* output interfaces have queues which need freeing */
        free(ifa->q->base);
        free(ifa->q);
    }

    free_filter(ifa->ifilter);
    free_filter(ifa->ofilter);

    if (ifa->info) {
        if (ifa->cleanup)
            ifa->cleanup(ifa);
        free(ifa->info);
    }

    if (ifa->pair) {
        ifa->pair->pair=NULL;
        if (ifa->pair->direction == OUT) {
            pthread_mutex_lock(&ifa->pair->q->q_mutex);
            ifa->pair->q->active=0;
            pthread_cond_broadcast(&ifa->pair->q->freshmeat);
            pthread_mutex_unlock(&ifa->pair->q->q_mutex);
        } else {
            if (ifa->pair->tid)
                pthread_kill(ifa->pair->tid,SIGUSR1);
            else
                ifa->pair->direction = NONE;
        }
    } else
        if (ifa->name && !(ifa->id & IDMINORMASK)) {
            free(ifa->name);
       }
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

    if (ifa->direction != NONE) {
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
    
        if (ifa->direction != OUT)
            if (!ifa->lists->inputs) {
                for(tptr=ifa->lists->outputs;tptr;tptr=tptr->next)
                    if (tptr->direction == BOTH)
                        break;
                if (tptr == NULL) {
                    pthread_mutex_lock(&ifa->lists->engine->q->q_mutex);
                    ifa->lists->engine->q->active=0;
                    pthread_cond_broadcast(&ifa->lists->engine->q->freshmeat);
                    pthread_mutex_unlock(&ifa->lists->engine->q->q_mutex);
                    if (timetodie == 0)
                        timetodie++;
                }
            }
    }

    free_if_data(ifa);

    /* Add to the dead list */
    if ((tptr=ifa->lists->dead) == NULL)
        ifa->lists->dead=ifa;
    else {
        for(;tptr->next;tptr=tptr->next);
        tptr->next=ifa;
    }
    ifa->next=NULL;
    return(0);
}

/*
 * Cleanup routine for interfaces, used as destructor for pointer to interface
 * structure in the handler thread's local storage
 * Args: pointer to interface structure
 * Returns: Nothing
 */
void iface_destroy(void *ifptr)
{
    iface_t *ifa = (iface_t *) ifptr;

    DEBUG(3,catgets(cat,2,37,"Cleaning up data for exiting %s %s %s id %x"),
            (ifa->direction == IN)?catgets(cat,2,38,"input"):
            catgets(cat,2,39,"output"),(ifa->id & IDMINORBITS)?
            catgets(cat,2,40,"connection"):catgets(cat,2,41,"interface"),
            ifa->name,ifa->id);
    sigset_t set,saved;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, &saved);
    if (ifa->heartbeat && ifa->q) {
        stop_heartbeat(ifa);
    }
    pthread_mutex_lock(&ifa->lists->io_mutex);
    if (ifa->tid) {
        unlink_interface(ifa);
        /* Signal the reaper thread */
        (void) pthread_kill(reaper,SIGUSR2);
    } else
        free_if_data(ifa);

    pthread_mutex_unlock(&ifa->lists->io_mutex);
    pthread_sigmask(SIG_SETMASK,&saved,NULL);
}

/*
 * add a filter to an interface
 * Args: pointer to filter to be added
 * Returns: pointer to filter to be added
 */
sfilter_t *addfilter(sfilter_t *filter)
{
    if (!filter)
        return (NULL);

    pthread_mutex_lock(&filter->lock);
    ++(filter->refcount);
    pthread_mutex_unlock(&filter->lock);
    return(filter);
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

    memset(newif,0,sizeof(iface_t));

    if (iftypes[ifa->type].ifdup_func) {
        if ((newif->info=(*iftypes[ifa->type].ifdup_func)(ifa->info)) == NULL) {
            free(newif);
            return(NULL);
        }
    } else
        newif->info = NULL;

    ifa->pair=newif;
    newif->tid=ifa->tid;
    newif->flags=ifa->flags;
    newif->id=ifa->id;
    newif->name=ifa->name;
    newif->pair=ifa;
    newif->next=NULL;
    newif->type=ifa->type;
    newif->lists=ifa->lists;
    newif->read=ifa->read;
    newif->readbuf=ifa->readbuf;
    newif->write=ifa->write;
    newif->cleanup=ifa->cleanup;
    newif->options=NULL;
    newif->ifilter=addfilter(ifa->ifilter);
    newif->ofilter=addfilter(ifa->ofilter);
    newif->checksum=ifa->checksum;
    newif->heartbeat = 0;
    newif->strict=ifa->strict;
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

    if ((confptr=getenv("KPLEXCONF")))
        return (confptr);
    if ((confptr=getenv("HOME")) == NULL)
        if ((pw=getpwuid(getuid())))
            confptr=pw->pw_dir;
    if (confptr) {
        if ((buf = malloc(strlen(confptr)+strlen(KPLEXHOMECONF)+2)) == NULL) {
            perror(catgets(cat,2,2,"failed to allocate memory"));
            exit(1);
        }
        strcpy(buf,confptr);
        strcat(buf,"/");

#ifdef KPLEXHOMECONFOSX
/*
 * Deprecate OSX specific config file.  This seemed like a good idea at the time
 * but has no apparent advantage  over ~/.kplex.conf
 */
        int doosxconf=1;
        size_t osxlen,baselen;

        if ((osxlen=strlen(KPLEXHOMECONFOSX)) <
                (baselen=strlen(KPLEXOLDHOMECONFOSX))) {
            osxlen=baselen;
        }

        if ((strlen(KPLEXHOMECONF)) < osxlen) {
            if (realloc(buf,(baselen=strlen(buf))+osxlen+1) == NULL) {
                perror(catgets(cat,2,42,"Can't query OSX config file"));
                doosxconf=0;
            }
        }

        if (doosxconf) {
            strcat(buf,KPLEXOLDHOMECONFOSX);
            if (!access(buf,F_OK)) {
                logwarn(catgets(cat,2,43,
                        "Use of %s is deprecated for kplex config.\nPlease move this file to ~/%s to suppress this warning"),
                        buf,KPLEXHOMECONF);
                return(buf);
            }
            strcpy(buf+baselen,KPLEXHOMECONFOSX);
            if (!access(buf,F_OK)) {
                return(buf);
            }
            buf[baselen]='\0';
        }
#endif

        strcat(buf,KPLEXHOMECONF);
        if (!access(buf,F_OK))
            return(buf);
        free(buf);
    }
    if (!access(KPLEXGLOBALCONF,F_OK))
        return(KPLEXGLOBALCONF);
    return(NULL);
}

/*
 * Translate a string like "local7" to a log facility like LOG_LOCAL7
 * Args: string representation of log facility
 * Returns: Numeric representation of log facility, or -1 if string doesn't
 * map to anything appropriate
 */
int string2facility(char *fac)
{
    int facnum;

    if (!strcasecmp(fac,"kern"))
        return(LOG_KERN);
    if (!strcasecmp(fac,"user"))
        return(LOG_USER);
    if (!strcasecmp(fac,"mail"))
        return(LOG_MAIL);
    if (!strcasecmp(fac,"daemon"))
        return(LOG_DAEMON);
    if (!strcasecmp(fac,"auth"))
        return(LOG_AUTH);
    if (!strcasecmp(fac,"syslog"))
        return(LOG_SYSLOG);
    if (!strcasecmp(fac,"lpr"))
        return(LOG_LPR);
    if (!strcasecmp(fac,"news"))
        return(LOG_NEWS);
    if (!strcasecmp(fac,"cron"))
        return(LOG_CRON);
    if (!strcasecmp(fac,"authpriv"))
        return(LOG_AUTHPRIV);
    if (!strcasecmp(fac,"ftp"))
        return(LOG_FTP);
    /* if we don't map to "localX" where X is 0-7, return error */
    if (strncasecmp(fac,"local",5) || (*fac + 6))
        return(-1);
    if ((facnum = (((int) *fac+5) - 32) < 16) || facnum > 23)
        return(-1);
    return(facnum<<3);
}

/*
 * Convert interface names to IDs
 * Args: Pointer to engine output filter, pointer to initialized interface list
 * Returns: -1 on failure, 0 on success
 */
int name2id(sfilter_t *filter)
{
    unsigned int id;
    sf_rule_t *rptr;
    struct srclist *sptr;

    if (!filter)
        return(0);

    if (filter->type == FILTER) {
        for (rptr=filter->rules;rptr;rptr=rptr->next) {
            if (rptr->src.name == NULL)
                continue;
            if (!(id=namelookup(rptr->src.name))) {
                logwarn(catgets(cat,2,44,
                        "Unknown interface \'%s\' in filter rules"),
                        rptr->src.name);
                return(-1);
            }
            free(rptr->src.name);
            rptr->src.name=NULL;
            rptr->src.id=id;
        }
        return(0);
    }
    
    for (rptr=filter->rules;rptr;rptr=rptr->next)
        for (sptr=rptr->info.source;sptr;sptr=sptr->next) {
            if (!(id=namelookup(sptr->src.name))) {
               logwarn(catgets(cat,2,45,
                        "Unknown interface \'%s\' in failover rules"),
                        sptr->src.name);
                return(-1);
            }
            free(sptr->src.name);
            sptr->src.name=NULL;
            sptr->src.id=id;
        }
    return(0);
}

int proc_engine_options(iface_t *e_info,struct kopts *options)
{
    struct kopts *optr;
    size_t qsize=DEFQSIZE;
    struct if_engine *ifg = (struct if_engine *) e_info->info;

    if (e_info->options) {
        for (optr=e_info->options;optr->next;optr=optr->next);
        optr->next=options;
    } else {
        e_info->options = options;
    }

    for (optr=e_info->options;optr;optr=optr->next) {
        if (!strcasecmp(optr->var,"qsize")) {
            if(!(qsize = atoi(optr->val))) {
                fprintf(stderr,catgets(cat,2,46,"Invalid queue size: %s\n"),
                        optr->val);
                exit(1);
            }
        } else if (!strcasecmp(optr->var,"mode")) {
            if (!strcasecmp(optr->val,"background"))
                ifg->flags|=K_BACKGROUND;
            else if (!strcasecmp(optr->val,"foreground"))
                ifg->flags &= ~K_BACKGROUND;
            else
                fprintf(stderr,catgets(cat,2,47,
                        "Warning: unrecognized mode \'%s\' specified\n"),
                        optr->val);
        } else if (!strcasecmp(optr->var,"logto")) {
            if ((ifg->logto = string2facility(optr->val)) < 0) {
                fprintf(stderr,catgets(cat,2,48,
                        "Unknown log facility \'%s\' specified\n"),optr->val);
                exit(1);
            }
        } else if (!strcasecmp(optr->var,"debuglevel")) {
            if ((*optr->val < '0') || (*optr->val > '9') || *(optr->val+1)) {
                fprintf(stderr,catgets(cat,2,49,
                        "Bad debug level \"%s\": Must be 0-9\n"),optr->val);
                exit(0);
            } else {
                debuglevel = (int) (*optr->val - '0');
            }
        } else if (!strcasecmp(optr->var,"graceperiod")) {
            if (((graceperiod=(time_t) strtoumax(optr->val,NULL,0)) == 0) &&
                    (errno)) {
                fprintf(stderr,catgets(cat,2,50,
                        "Bad value for graceperiod: %s\n"),
                        optr->val);
                exit(1);
            }
        } else if (!strcasecmp(optr->var,"checksum")) {
            if (!strcasecmp(optr->val,"yes") || !strcasecmp(optr->val,"strict"))
                e_info->checksum=CKSM_STRICT;
            else if (!strcasecmp(optr->val,"no"))
                e_info->checksum=CKSM_NO;
            else if (!strcasecmp(optr->val,"add"))
                e_info->checksum=CKSM_ADD;
            else if (!strcasecmp(optr->val,"addonly"))
                e_info->checksum=CKSM_ADDONLY;
            else {
                fprintf(stderr,"%s",catgets(cat,2,51,
                        "Checksum option must be one of: \'yes\',\'no\',\'strict\',\'loose\',\'add\',\'addonly\'\n"));
                exit(1);
            }
        } else if (!strcasecmp(optr->var,"strict")) {
            if (!strcasecmp(optr->val,"yes"))
                e_info->strict=1;
            else if (!strcasecmp(optr->val,"no"))
                e_info->strict=0;
            else {
                fprintf(stderr,"%s",catgets(cat,2,52,
                        "Strict option must be either \'yes\' or \'no\'\n"));
                exit(1);
            }
        } else if (!strcasecmp(optr->var,"failover")) {
            if (addfailover(&e_info->ofilter,optr->val) != 0) {
                fprintf(stderr,catgets(cat,2,53,
                        "Failed to add failover %s\n"),optr->val);
                exit(1);
            }
        } else {
            fprintf(stderr,catgets(cat,2,54,
                    "Warning: Unrecognized option \'%s\'\n"),optr->var);
            exit(0);
        }
    }

    if (init_q(e_info, qsize) < 0) {
        perror(catgets(cat,2,55,"failed to initiate queue"));
        exit(1);
    }
    return(0);
}

int calcsum(const char *buf, size_t len)
{
    int c = 0;

    for (;len;len--)
        c ^=*buf++;

    return c;
}

/* Add tag data
 * Args: Interface pointer, buffer for tags
 * Returns: Length of tag buffer on success
 */
size_t gettag(iface_t *ifa, char *buf, senblk_t *sptr)
{
    char *ptr=buf;
    char *nameptr;
    int first=1;
    struct timeval tv;
    unsigned char cksum;
    size_t len;

    *ptr++='\\';
    if (ifa->tagflags & TAG_SRC){
        first=0;
        memcpy(ptr,"s:",2);
        ptr+=2;
        if (ifa->tagflags & TAG_ISRC) {
            if (((nameptr=idlookup(sptr->src))==NULL) || (*nameptr == '_'))
                nameptr=DEFSRCNAME;
        } else
            nameptr=(*ifa->name=='_')?DEFSRCNAME:ifa->name;

        for (len=0;*nameptr && len < 15; len++)
            *ptr++=*nameptr++;
    }

    if (ifa->tagflags & TAG_TS) {
        if (!first)
            *ptr++=',';
        memcpy(ptr,"c:",2);
        ptr+=2;
        (void) gettimeofday(&tv,NULL);
        ptr+=sprintf(ptr,"%010u",(unsigned) tv.tv_sec);
        if (ifa->tagflags & TAG_MS)
            ptr += sprintf(ptr,"%03u",((unsigned) tv.tv_usec)/1000);
    }
    /* Don't include initial '/' */
    cksum=calcsum(buf+1,(len=ptr-buf)-1);
    len+=sprintf(ptr,"*%02X\\",cksum);
    return(len);
}

/* generic read routine
 * Args: Interface Pointer
 * Returns: nothing
 */ 
void do_read(iface_t *ifa)
{
    senblk_t sblk;
    char buf[BUFSIZ];
    char tbuf[TAGMAX];
    char *bptr,*eptr,*ptr;
    int nread,countmax,count=0;
    enum sstate senstate;
    int nocr=flag_test(ifa,F_NOCR)?1:0;
    int loose = (ifa->strict)?0:1;
    sblk.src=ifa->id;
    senstate=SEN_NODATA;

    while ((nread=(*ifa->readbuf)(ifa,buf)) > 0) {
        for(bptr=buf,eptr=buf+nread;bptr<eptr;bptr++) {
            switch (*bptr) {
            case '$':
            case '!':
                ptr=sblk.data;
                countmax=SENMAX-(nocr|loose);
                count=1;
                *ptr++=*bptr;
                senstate=SEN_SENPROC;
                continue;
            case '\\':
                if (senstate==SEN_TAGPROC) {
                    *ptr++=*bptr;
                    senstate=SEN_TAGSEEN;
                } else {
                    senstate=SEN_TAGPROC;
                    ptr=tbuf;
                    countmax=TAGMAX-1;
                    *ptr++=*bptr;
                    count=1;
                }
                continue;
            case '\r':
            case '\n':
            case '\0':
                if (senstate == SEN_SENPROC || senstate == SEN_TAGSEEN) {
                    if (loose || (nocr && *bptr == '\n')) {
                        *ptr++='\r';
                        *ptr='\n';
                        sblk.len = count+2;
                    } else {
                        if ((!nocr) && *bptr == '\r') {
                            senstate = SEN_CR;
                            *ptr++=*bptr;
                            ++count;
                        } else {
                            senstate = SEN_NODATA;
                        }
                        continue;
                    }
                } else if (senstate == SEN_CR) {
                    if (*bptr != '\n') {
                        senstate = SEN_NODATA;
                        continue;
                    }
                    *ptr=*bptr;
                    sblk.len = ++count;
                } else {
                    senstate = SEN_NODATA;
                    continue;
                }
                /* If we're not checksumming OR the checksum is correct OR
                 * it's a zero length packet, the first clause is false which
                 * is true when negated...*/
                if (!(ifa->checksum && checkcksum(&sblk,ifa->checksum) && (sblk.len > 0 )) &&
                        senfilter(&sblk,ifa->ifilter) == 0) {
                    push_senblk(&sblk,ifa->q);
                }
                senstate=SEN_NODATA;
                continue;
            default:
                break;
            }

            if (senstate != SEN_SENPROC && senstate != SEN_TAGPROC) {
                if (senstate != SEN_NODATA )
                    senstate=SEN_NODATA;
                continue;
            }

            if (count++ > countmax) {
                senstate=SEN_NODATA;
                continue;
            }

            *ptr++=*bptr;
        }
    }
    iface_thread_exit(errno);
}

/* Make an interface name based on file type and index
 * Args: Pointer to interface structure and index
 * Returns: Pointer to newly malloced string containing constructed name
 * Side effects: None (but string will need free-ing)
 */
char * mkname(iface_t *ifa, unsigned int i)
{
    char *ptr,*dst;
    char nambuf[128];

    /* auto assigned names are "_<type>-<id>", e.g. "_udp-2" */

    for (ptr=iftypes[ifa->type].name,dst=nambuf,*dst++='_';*ptr;ptr++)
        *dst++=*ptr;

    *dst++='-';
    *dst++='i';
    *dst++='d';

    do {
        *dst++='0'+(i%10);
        i/=10;
    } while(i);

    *dst=*(dst+1)='\0';

    /* We *could* Append stuff until a unique name was found but that's
     * unnecessary complexity.  Return simple error instead */

    if (namelookup(nambuf)) {
        logerr(0,catgets(cat,2,56,
                "\"%s\" already specified as an interface name"),
                nambuf);
        return(NULL);
    }

    return(strdup(nambuf));
}        

nl_catd init_i8n()
{
    char * tmpbuf;
    char *lang;

    /* initialize i8n: First try NLSPATH/defaults */
    setlocale(LC_ALL,"");

    if ((cat = catopen("kplex.cat",NL_CAT_LOCALE)) == (nl_catd) -1) {
        /* try our compiled-in default */
        if ((lang=getenv("LANG")) || (lang=setlocale(LC_MESSAGES,NULL))) {
            if (strcmp(lang,"POSIX") && strcmp(lang,"C") &&
                    (strlen(lang) >= 2) && ( lang[2] == '\0' ||
                    (lang[2] == '_') || (lang[2] == '.'))) { 
                if ((tmpbuf = (char *) malloc(strlen(SHAREDIR) +
                        strlen("/locale//kplex.cat") + (size_t) 6)) == NULL) {
                    perror("Failed to allocate memory");
                    exit(1);
                }

                if (strlen(lang) >= 5) {
                    sprintf(tmpbuf,"%s/locale/%.5s/kplex.cat",SHAREDIR,lang);
                    cat = catopen(tmpbuf,0);
                }
                if (cat == (nl_catd) -1) {
                    sprintf(tmpbuf,"%s/locale/%.2s/kplex.cat",SHAREDIR,lang);
                    cat = catopen(tmpbuf,0);
                }
                free(tmpbuf);
            }
        }
    }
    return cat;
}

int main(int argc, char ** argv)
{
    char *tmpbuf;
    pthread_t tid;
    pid_t pid;
    int pfd;
    char *config=NULL;
    char *pidfile=NULL;
    iface_t  *engine;
    struct if_engine *ifg;
    iface_t *ifptr,*ifptr2,*rptr;
    iface_t **tiptr;
    unsigned int i=1;
    int opt,err=0;
    void *ret;
    struct kopts *options=NULL;
    sigset_t set,oset;
    struct iolists lists = {
        /* initialize io_mutex separately below */
        .init_mutex = PTHREAD_MUTEX_INITIALIZER,
        .init_cond = PTHREAD_COND_INITIALIZER,
        .dead_cond = PTHREAD_COND_INITIALIZER,
    .initialized = NULL,
    .outputs = NULL,
    .inputs = NULL,
    .dead = NULL,
    .eventmgr = NULL
    };
    struct rlimit lim;
    struct flock *fl;
    int gotinputs=0;
    int rcvdsig;
    struct sigaction sa;

    (void) init_i8n();

    pthread_mutex_init(&lists.io_mutex,NULL);

    /* command line argument processing */
    while ((opt=getopt(argc,argv,"d:f:o:p:V")) != -1) {
        switch (opt) {
            case 'd':
                if (*(optarg+1)) {
                    /* debuglevel is more than one character long */
                    fprintf(stderr,catgets(cat,2,1,
                            "Bad debug level \"%s\": Must be 0-9\n"),optarg);
                    err++;
                } else if ((tmpbuf = (char *) malloc(13)) == NULL ) {
                    logerr(errno,catgets(cat,2,2,"failed to allocate memory"));
                    err++;
                } else {
                    sprintf(tmpbuf,"debuglevel=%c",*optarg);
                    if (cmdlineopt(&options,tmpbuf) < 0)
                        err++;
                    free(tmpbuf);
                }
                break;
            case 'o':
                if (cmdlineopt(&options,optarg) < 0)
                    err++;
                break;
            case 'f':
                config=optarg;
                break;
            case 'p':
                pidfile=optarg;
                break;
            case 'V':
                printf("%s\n",VERSION);
                if (argc == 2)
                    exit(0);
                else
                    err++;
                break;
            default:
                err++;
        }
    }

    if (err) {
        fprintf(stderr, catgets(cat,2,3,"Usage: %s [-V] | [ -d <level> ] [ -p <pid file> ] [ -f <config file>] [-o <option=value>]... [<interface specification> ...]\n"),argv[0]);
        exit(1);
    }


    /* If a config file is specified by a commad line argument, read it.  If
     * not, look for a default config file unless told not to using "-f-" on the
     * command line
     */
    if ((config && (strcmp(config,"-"))) ||
            (!config && (config = get_def_config()))) {
        DEBUG(1,catgets(cat,2,4,"Using config file %s"),config);
        if ((engine=parse_file(config)) == NULL) {
            fprintf(stderr,catgets(cat,2,5,"Error parsing config file: %s\n"),
                    errno?strerror(errno):catgets(cat,2,6,"Syntax Error"));
            exit(1);
        }
    } else {
        /* global options for engine configuration are also returned in config
         * file parsing. If we didn't do that, get default options here */
        DEBUG(1,catgets(cat,2,7,"Not using config file"));
        engine = get_default_global();
    }

    proc_engine_options(engine,options);

    engine->lists = &lists;
    lists.engine=engine;

    for (tiptr=&engine->next;optind < argc;optind++) {
        if (!(ifptr=parse_arg(argv[optind]))) {
            fprintf(stderr,catgets(cat,2,8,
                    "Failed to parse interface specifier %s\n"),
                    argv[optind]);
            exit(1);
        }
        ifptr->next=(*tiptr);
        (*tiptr)=ifptr;
        tiptr=&ifptr->next;
    }

    /* We choose to go into the background here before interface initialzation
     * rather than later. Disadvantage: Errors don't get fed back on stderr.
     * Advantage: We can close all the file descriptors now rather than pulling
     * then from under erroneously specified stdin/stdout etc.
     */
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK,&set,&oset);
    ifg=(struct if_engine *)engine->info;
    if (ifg->flags & K_BACKGROUND) {
         if ((pid = fork()) < 0) {
            perror(catgets(cat,2,9,"fork failed"));
            exit(1);
        } else if (pid) {
            sigwait(&set,&rcvdsig);
            if (rcvdsig == SIGCHLD) {
                if (wait(&err) < 0) {
                    perror("Wait failed");
                    exit (1);
                }
                if (WIFEXITED(err))
                    exit(WEXITSTATUS(err));

                exit(1);
            }
            exit(0);
        }
    }

    /* Continue as child */

    sigprocmask(SIG_SETMASK,&oset,NULL);

    if (pidfile) {
        /* Check for pidfile and lock it if not locked already */
        if ((pfd=open(pidfile,O_RDWR|O_CREAT,0644)) < 0) {
            fprintf(stderr,catgets(cat,2,11,"Could not create pid file: %s"),
                    strerror(errno));
            exit(1);
        }

        if ((fl=(struct flock *) malloc(sizeof(struct flock))) == NULL) {
            perror(catgets(cat,2,12,"Could not allocate memory for pid lock"));
            exit(1);
        }

        fl->l_type=F_WRLCK;
        fl->l_whence=SEEK_SET;
        fl->l_start=0;
        fl->l_len=0;

        if ((err=fcntl(pfd,F_SETLK,fl)) < 0) {
            if (errno == EACCES || err == EAGAIN ) {
                fprintf(stderr,catgets(cat,2,13,
                        "pid file %s currently lock by pid %d\n"),pidfile,
                        (int) fl->l_pid); 
            } else {
                fprintf(stderr,catgets(cat,2,14,
                        "Could not lock pid file %s: %s\n"),pidfile,
                        strerror(err));
            }
            exit(1);
        }
        if (truncate(pidfile,0) < 0) {
            fprintf(stderr,catgets(cat,2,15,
                    "Could not truncate pid file %s: %s\n"),pidfile,
                    strerror(errno));
            exit(1);
        }

        dprintf(pfd,"%d",getpid());
    }

    if (ifg->flags & K_BACKGROUND) {

        /* Really should close all file descriptors. Harder to do in OS
         * independent way.  Just close the ones we know about for this cut
         * Check first if connected to a tty to allow redirection / piping in
         * background mode
         */
        if (isatty(fileno(stdin))) {
            fclose(stdin);
            ifg->flags |= K_NOSTDIN;
        }
        if (isatty(fileno(stdout))) {
            fclose(stdout);
            ifg->flags |= K_NOSTDOUT;
        }
        if (isatty(fileno(stderr))) {
            fclose(stderr);
            ifg->flags |= K_NOSTDERR;
        }

        /* Tell Parent it's OK to exit */
        kill(getppid(),SIGUSR1);

        setsid();
        (void) chdir("/");
        umask(0);
    }

    /* log to stderr or syslog, as appropriate */
    initlog((ifg->flags & K_NOSTDERR)?ifg->logto:-1);

    /* Lower max open files if necessary. We do this to ensure that ids for
     * all connections can be represented in IDMINORBITS. Actually we only
     * need to do that per server, so this is a bit of a hack and should be
     * corrected
     */
    if (getrlimit(RLIMIT_NOFILE,&lim) < 0)
            logterm(errno,catgets(cat,2,16,"Couldn't get resource limits"));
    if (lim.rlim_cur > 1<<IDMINORBITS) {
        DEBUG(3,catgets(cat,2,17,
                "Lowering NOFILE from %u to %u"),lim.rlim_cur,1<<IDMINORBITS);
        lim.rlim_cur=1<<IDMINORBITS;
        if(setrlimit(RLIMIT_NOFILE,&lim) < 0)
            logterm(errno,catgets(cat,2,18,
                    "Could not set file descriptor limit"));
    }

    DEBUG(1,catgets(cat,2,19,"kplex starting, config file %s"),
            (config && strcmp(config,"-"))?config:catgets(cat,2,20,"<none>"));

    /* our list of "real" interfaces starts after the first which is the
     * dummy "interface" specifying the multiplexing engine
     * walk the list, initialising the interfaces.  Sometimes "BOTH" interfaces
     * are initialised to one IN and one OUT which then need to be linked back
     * into the list
     */
    for (ifptr=engine->next,tiptr=&lists.initialized,i=0;ifptr;ifptr=ifptr2) {
        ifptr2 = ifptr->next;

        if (i == MAXINTERFACES)
            logterm(0,catgets(cat,2,21,"Too many interfaces"));
        ifptr->id=++i<<IDMINORBITS;
        if (!ifptr->name) {
            if (!(ifptr->name=mkname(ifptr,i)))
                logterm(errno,catgets(cat,2,22,
                        "Failed to make interface name"));
        }

        if (insertname(ifptr->name,ifptr->id) < 0)
            logterm(errno,catgets(cat,2,23,
                    "Failed to associate interface name and id"));

        ifptr->lists = &lists;

        if ((rptr=(*iftypes[ifptr->type].init_func)(ifptr)) == NULL) {
            logerr(0,catgets(cat,2,24,"Failed to initialize Interface %s"),
                    (ifptr->name)?ifptr->name:catgets(cat,2,25,"(unnamed)"));
            if (!flag_test(ifptr,F_OPTIONAL)) {
                timetodie++;
                break;
            }
            /* Free all resources associated with interface
             * This is a bigger task than it looks.  Before the "optional"
             * flag this was not an issue as we'd just be exiting after this
             * Now we need to clean up properly but not yet implemented.  This
             * is a little memory leak with each failed init attempt
             */
            free(ifptr);
            continue;
        }
        for (;ifptr;ifptr = ifptr->next) {
        /* This loop should be done once for IN or OUT interfaces twice for
         * interfaces where the initialisation routine has expanded them to an
         * IN/OUT pair.
         */
            if (ifptr->direction == IN)
                ifptr->q=engine->q;

            if (ifptr->checksum == CKSM_UNDEF)
                ifptr->checksum = engine->checksum;
            if (ifptr->strict <0) {
                if (engine->strict >= 0) {
                    ifptr->strict = engine->strict;
                } else {
                    ifptr->strict = (ifptr->type == FILEIO)?0:1;
                }
            }
            (*tiptr)=ifptr;
            tiptr=&ifptr->next;
            if (ifptr->next==ifptr2)
                ifptr->next=NULL;
        }
    }

    /* One more spin through the list now we've initialised the name to id
     * mapping so we can update references to "name" with an id
     */
    for (ifptr=lists.initialized;ifptr;ifptr=ifptr->next) {
        if (ifptr->direction != IN && ifptr->ofilter) {
            if (name2id(ifptr->ofilter)) {
                logterm(errno,catgets(cat,2,26,
                        "Name to interface translation failed"));
            }
        }
        if (ifptr->heartbeat) {
            if (lists.eventmgr == NULL) {
                if ((lists.eventmgr = init_evtmgr()) == NULL) {
                    logterm(errno, catgets(cat,2,57,
                            "failed to initialize event manager"));
                }
            }
            if (ifptr->q && add_event(EVT_HB,(void *)ifptr,0) < 0) {
                logterm(errno, catgets(cat,2,58,
                        "failed to add interface heartbeat"));
            }
        }
    }

    /* Create the key for thread local storage: in this case for a pointer to
     * the interface each thread is handling
     */
    if (pthread_key_create(&ifkey,iface_destroy)) {
        logerr(errno,catgets(cat,2,27,"Error creating key"));
        timetodie++;
    }

    if (timetodie) {
        for (ifptr=lists.initialized;ifptr;ifptr=ifptr2) {
            ifptr2=ifptr->next;
            iface_destroy(ifptr);
        }
        exit(1);
    }

    if (name2id(engine->ofilter))
            logterm(errno,catgets(cat,2,29,
                    "Failed to translate interface names to IDs"));

    if (engine->options)
        free_options(engine->options);

    pthread_setspecific(ifkey,(void *)&lists);
    reaper=pthread_self();

    sigemptyset(&set);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler=terminate;
    sa.sa_flags=0;
    sigaction(SIGUSR1,&sa,NULL);
    sigaddset(&set,SIGUSR1);
    sigaddset(&set,SIGUSR2);
    sigaddset(&set,SIGALRM);
    sigaddset(&set,SIGTERM);
    sigaddset(&set,SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    sigdelset(&set,SIGUSR1);
    signal(SIGPIPE,SIG_IGN);
    pthread_create(&tid,NULL,run_engine,(void *) engine);

    pthread_mutex_lock(&lists.io_mutex);
    for (ifptr=lists.initialized;ifptr;ifptr=ifptr->next) {
        /* Check we've got at least one input */
        if ((ifptr->direction == IN ) || (ifptr->direction == BOTH))
            gotinputs=1;
        /* Create a thread to run each interface */
        pthread_create(&tid,NULL,(void *)start_interface,(void *) ifptr);
    }

    while (lists.initialized)
        pthread_cond_wait(&lists.init_cond,&lists.io_mutex);

    /* Have to wait until here to do something about no inputs to
     * avoid deadlock on io_mutex
     */
    if (!gotinputs) {
        logerr(0,catgets(cat,2,29,"No Inputs!"));
        pthread_mutex_lock(&engine->q->q_mutex);
        engine->q->active=0;
        pthread_cond_broadcast(&engine->q->freshmeat);
        pthread_mutex_unlock(&engine->q->q_mutex);
        timetodie++;
    }

    if (lists.eventmgr) {
        if (!timetodie) {
            if (pthread_create(&lists.eventmgr->tid,NULL,(void *)proc_events,
                    (void *) NULL) == 0) {
                lists.eventmgr->active=1;
            }
        }
    }

    /* While there are remaining outputs, wait until something is added to the 
     * dead list, reap everything on the dead list and check for outputs again
     * until all the outputs have been reaped
     * Note that when there are no more inputs, we set the
     * engine's queue inactive causing it to set all the outputs' queues
     * inactive and shutting them down. Thus the last input exiting also shuts
     * everything down */
    while (lists.outputs || lists.inputs || lists.dead) {
        if (lists.dead  == NULL && (timetodie <= 0)) {
            pthread_mutex_unlock(&lists.io_mutex);
            /* Here we're waiting for SIGTERM/SIGINT (user shutdown requests),
             * SIGUSR2 (notifications of termination from interface threads)
             * and (later) SIGALRM to notify of the grace period expiry
             */
            (void) sigwait(&set,&rcvdsig);
            pthread_mutex_lock(&lists.io_mutex);
        } else {
            rcvdsig = 0;
        }

        if ((timetodie > 0) || ( lists.outputs == NULL && (timetodie == 0)) ||
                rcvdsig == SIGTERM || rcvdsig == SIGINT) {
            timetodie=-1;
            /* Once we've caught a user shutdown address we don't need to be
             * told twice
             */
            signal(SIGTERM,SIG_IGN);
            signal(SIGINT,SIG_IGN);
            sigdelset(&set,SIGTERM);
            sigdelset(&set,SIGINT);
            for (ifptr=lists.inputs;ifptr;ifptr=ifptr->next) {
                pthread_kill(ifptr->tid,SIGUSR1);
            }
            for (ifptr=lists.outputs;ifptr;ifptr=ifptr->next) {
                if (ifptr->q == NULL)
                    pthread_kill(ifptr->tid,SIGUSR1);
            }
            /* Set up the graceperiod alarm */
            if (graceperiod)
                alarm(graceperiod);
        }
        if (rcvdsig == SIGALRM || graceperiod == 0) {
            sigdelset(&set,SIGALRM);
            /* Make sure we don't come back here with 0 graceperiod */
            if (graceperiod == 0)
                graceperiod=1;
            for (ifptr=lists.outputs;ifptr;ifptr=ifptr->next) {
                if (ifptr->q)
                    pthread_kill(ifptr->tid,SIGUSR1);
            }
        }
        for (ifptr=lists.dead;ifptr;ifptr=lists.dead) {
            lists.dead=ifptr->next;
            pthread_join(ifptr->tid,&ret);
            free(ifptr);
        }
    }

    /* Kill the event manager only after all other threads */
    if (lists.eventmgr && lists.eventmgr->active) {
        pthread_kill(lists.eventmgr->tid,SIGUSR1);
        pthread_join(lists.eventmgr->tid,NULL);
    }

    /* For neatness... */
    pthread_mutex_unlock(&lists.io_mutex);

    DEBUG(3,catgets(cat,2,30,"Removing pid file"));

    if (pidfile)
        unlink(pidfile);

    DEBUG(1,catgets(cat,2,31,"Kplex exiting"));

    catclose(cat);

    exit(0);
}

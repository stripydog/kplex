/* events.c.
 * This file is part of kplex.
 * Copyright Keith Young 2020
 * For copying information see the file COPYING distributed with this software
 *
 * This file deals with functions implementing the event manager which
 * is initially responsible for application level heartbeats.  Note that
 * timing here uses gettimeofday() because we're not too fussed about
 * microsecond accuracy of timing intervals but want to maximise portability.
 */

#include "kplex.h"
#include <signal.h>
#include <sys/time.h>

/*
 * This gets initialized by init_evtmgr() which is called from kplex.c with
 * the return value being assigned to lists.eventmgr in main()'s iolists
 */
struct evtmgr *mgr;

/*
 * Initialize (but don't start) the event manager.
 * Args: None
 * Return: Address of initialized event manager or NULL on failure
 */
struct evtmgr *init_evtmgr()
{
    if ((mgr = (struct evtmgr *) malloc(sizeof(struct evtmgr))) == NULL) {
        logerr(errno,catgets(cat,12,1,"Failed to initialize event manager"));
        return NULL;
    }

    if (pthread_mutex_init(&mgr->evt_mutex,NULL) ||
            pthread_cond_init(&mgr->evt_cond,NULL)) {
        logerr(errno,catgets(cat,12,1,"Failed to initialize event manager"));
        free(mgr);
        return(NULL);
    }

    mgr->events = NULL;
    /* active gets set to 1 on successful start of event manager thread */
    mgr->active = 0;

    return(mgr);
}

/*
 * Send a proprietary NMEA-0183 sentence as a "heartbeat"
 * Args: pointer (cast to void *) to the interface to heartbeat out of
 * Returns:  0
 */
int heartbeat(void *info)
{
    iface_t *ifp = (iface_t *)info;
    char *heartstring="$PKPXI,HB*7C\r\n"; /* Heartbeat sentence */
    senblk_t sblk;
    char *ptr;

    sblk.len = 0;
    sblk.src = 0;

    /* Copy sentence to senblk structure and set length field */
    for (ptr=sblk.data;*heartstring;*ptr++ = *heartstring++) {
        sblk.len++;
    }

    /* Add sentence to outgoing interface queue */
    push_senblk(&sblk,ifp->q);

    /* Always return 0.  This function is not "void" to allow for
     * generic handlers in the evt_t structure which may in future do
     * something with return codes to indicate success/failure
     */
    return(0);
}

/*
 * Reschedule a periodic event (like a heartbeat)
 * Args: The event to reschedule
 * Returns: Nothing
 */
void reschedule_periodic(evt_t *e)
{
    evt_t *eptr;

    /* Increment the existing absolute time by the period amount */
    e->when.tv_sec += e->period;

    /* Find the event before the next event in the queue which is scheduled
     * for a later or equal time to the reschedule time (or the last if the
     * resceduled time is later than all existing events
     */
    for (eptr = e;eptr->next; eptr = eptr->next) {
        if (eptr->next->when.tv_sec >= e->when.tv_sec) {
            break;
        }
    }

    /* If the found event is the one we just rescheduled, it will be the
     * next event and there's nothing for us to do
     */
    if (eptr == e) {
        return;
    }

    /* Move the event to the required positionin th linked list, linking the
     * new earliest event to the head of the events queue
     */
    mgr->events = e->next;
    e->next = eptr->next;
    eptr->next = e;
    return;
}

/*
 * Main processing loop for the event manager
 * Args: (void *) NULL (unused)
 * Returns: void
 */
void proc_events(void *arg)
{
    struct timeval tv;
    sigset_t set;
    evt_t *teptr;

    /* unblock SIGUSR1 so we can be told to shut down */
    sigemptyset(&set);
    sigaddset(&set,SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK,&set,NULL);

    pthread_mutex_lock(&mgr->evt_mutex);

    for(;;) {
        /* If no events, wait indefinitely until one is added */
        if (mgr->events == NULL) {
            pthread_cond_wait(&mgr->evt_cond,&mgr->evt_mutex);
            continue;
        }
        gettimeofday(&tv,NULL);
        if (tv.tv_sec < mgr->events->when.tv_sec) {
            /* Wait until notified or next event is due */
            pthread_cond_timedwait(&mgr->evt_cond,&mgr->evt_mutex,
                    &mgr->events->when);
            continue;
        }
        /* Handle next event on the queue */
        mgr->events->handle(mgr->events->info);

        /* Don't want to die in the middle of lis manipulation */
        pthread_sigmask(SIG_BLOCK,&set,NULL);

        /* If event is periodic, reschedule; otherwise remove */
        if (mgr->events->period) {
            reschedule_periodic(mgr->events);
        } else {
            teptr = mgr->events;
            mgr->events = teptr->next;
            free(teptr);
        }
        pthread_sigmask(SIG_UNBLOCK,&set,NULL);
    }
}

/*
 * Remove a heartbeat for an interface
 * Args: Pointer to interface
 * Returns: void
 */
void stop_heartbeat(iface_t *ifp)
{
    evt_t **epptr,*eptr;
    sigset_t set,oset;

    sigemptyset(&set);
    sigaddset(&set,SIGUSR1);

    pthread_sigmask(SIG_BLOCK,&set,&oset);
    pthread_mutex_lock(&mgr->evt_mutex);

    /* Search the list for any heartbeats associated with ifp and remove them
     * Multiple heartbeats are possible but not really sure why...
     */
    for (epptr=&mgr->events;(*epptr);) {
        if ((*epptr)->type == EVT_HB && (*epptr)->info == (void *) ifp) {
            eptr = *epptr;
            *epptr = eptr->next;
            free(eptr);
            continue;
        }
        epptr=&(*epptr)->next;
    }
    pthread_mutex_unlock(&mgr->evt_mutex);
    pthread_sigmask(SIG_SETMASK,&oset,NULL);
}

/*
 * Add an event to the event manager queue
 * Args: Event type (EVT_HB only defined for now), associated info and
 *       when the event is scheduled for.  If 0, make it "now"
 * Returns: 0 on success, 1 on failure
 */
int add_event(enum evttype etype, void *info, time_t when)
{
    struct timeval tv;
    evt_t *new_evt,**evtpptr;
    sigset_t set,oset;

    sigemptyset(&set);
    sigaddset(&set,SIGUSR1);

    if ((new_evt = (evt_t *) malloc(sizeof(evt_t))) == NULL) {
        logerr(errno,catgets(cat,12,2,"Failed to add new event"));
    }

    if (when == 0) {
        gettimeofday(&tv,NULL);
        when = tv.tv_sec;
    }

    new_evt->type=etype;
    new_evt->when.tv_sec = when;
    new_evt->when.tv_nsec = 0;

    switch (etype) {
    case EVT_HB:
        new_evt->info = info;
        new_evt->handle = heartbeat;
        new_evt->period = ((iface_t *)info)->heartbeat;
        break;
    default:
        return(-1);
    }

    pthread_sigmask(SIG_BLOCK,&set,&oset);

    pthread_mutex_lock(&mgr->evt_mutex);
    for (evtpptr=&mgr->events;(*evtpptr);evtpptr=&(*evtpptr)->next) {
        if ((*evtpptr)->when.tv_sec > when) {
            break;
        }
    }

    new_evt->next = (*evtpptr)?*evtpptr:NULL;
    *evtpptr=new_evt;
    pthread_mutex_unlock(&mgr->evt_mutex);
    pthread_cond_broadcast(&mgr->evt_cond);
    pthread_sigmask(SIG_SETMASK,&oset,NULL);
    return(0);
}

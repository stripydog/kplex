/* lookup.c
 * This file is part of kplex
 * Copyright Keith Young 2012 - 2019
 * For copying information see the file COPYING distributed with this software
 *
 * functions for associating names with interfaces
 */

#include "kplex.h"
#include<stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

/* Structures holding the name to id mappings in a linked list */
struct nameid {
    unsigned long id;
    char * name;
    struct nameid *next;
};

/* This is used before we start multiple threads */
static struct nameid *idlist;

/*
 * Return an interface name given an ID
 * Args: interface id
 * Returns: pointer to interface name if found, NULL otherwise
 */
char * idlookup(unsigned long id)
{
    struct nameid *nptr;
    id&=~((unsigned int) IDMINORMASK);

    for (nptr=idlist;nptr;nptr=nptr->next) {
        if (nptr->id == id)
            return(nptr->name);
    }

    return(NULL);
}

/*
 * Return an interface ID given a name
 * Args: Pointer to a name
 * Returns: Interface id on success, 0 otherwise
 */
unsigned long namelookup(char *name)
{
    long ret;
    struct nameid *nptr;

    if (name == NULL) {
        logerr(0,catgets(cat,9,1,"namelookup: NULL pointer passed"));
        return(0);
    }

    for (nptr=idlist;nptr;nptr=nptr->next) {
        if((ret=strcasecmp(name,nptr->name))) {
            if (ret<0)
                return(0);
        } else {
            return(nptr->id);
        }
    }
    return(0);
}

/*
 * Insert a name-ID mapping into the list
 * Args: Pointer to a name, associated interface ID
 * Returns: 0 on success, -1 otherwise
 * Side Effects: structure is created and linked into the list of mappings
 */
int insertname(char *name, unsigned long id)
{
    struct nameid *nptr,**nptrp;
    long ret;

    for (nptrp=&idlist;(*nptrp);nptrp=&(*nptrp)->next)
        if ((ret=strcasecmp(name,(*nptrp)->name)) == 0) {
            logwarn(catgets(cat,9,2,
                    "%s used as name for more than one interface"),name);
            return(-1);
        } else if (ret < 0 )
            break;
    if ((nptr = (struct nameid *)malloc(sizeof(struct nameid))) < 0) {
        logerr(errno,catgets(cat,9,3,"Memory allocation failed"));
        return(-1);
    }
    nptr->name=name;
    nptr->id=id;
    nptr->next=(*nptrp);
    *nptrp=nptr;
    return(0);
}

/*
 * Free a name/ID mapping list
 * Args: none
 * Returns: nothing
 * Side Effects: All name/ID mapping strings are freed
 */
void freenames()
{
    struct nameid *nptr,*nptr2;

    for (nptr=idlist;nptr;nptr=nptr2) {
        nptr2=nptr->next;
        free(nptr);
    }
}

/* lookup.c
 * This file is part of kplex
 * Copyright Keith Young 2012 - 2013
 * For copying information see the file COPYING distributed with this software
 *
 * functions for associating names with interfaces
 */

#include "kplex.h"
#include<stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

struct nameid {
    unsigned int id;
    char * name;
    struct nameid *next;
};

/* This is used before we start multiple threads */
static struct nameid *idlist;

unsigned int namelookup(char *name)
{
    int ret;
    struct nameid *nptr;

    for (nptr=idlist;nptr;nptr=nptr->next) {
        if((ret=strcmp(name,nptr->name))) {
            if (ret<0)
                return(-1);
        } else {
            return(nptr->id);
        }
    }
    return(-1);
}

int insertname(char *name, unsigned int id)
{
    struct nameid *nptr,**nptrp;
    int ret;

    for (nptrp=&idlist;(*nptrp);nptrp=&(*nptrp)->next)
        if ((ret=strcmp(name,(*nptrp)->name)) == 0) {
            logwarn("%s used as name for more than one interface",name);
            return(-1);
        } else if (ret < 0 )
            break;
    if ((nptr = (struct nameid *)malloc(sizeof(struct nameid))) < 0) {
        logerr(errno,"Memory allocation failed");
        return(-1);
    }
    nptr->name=name;
    nptr->id=id;
    nptr->next=(*nptrp);
    *nptrp=nptr;
    return(0);
}

void freenames()
{
    struct nameid *nptr,*nptr2;

    for (nptr=idlist;nptr;nptr=nptr2) {
        nptr2=nptr->next;
        free(nptr);
    }
}

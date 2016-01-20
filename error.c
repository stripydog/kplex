/* error.c
 * This file is part of kplex
 * Copyright Keith Young 2012 - 2016
 * For copying information see the file COPYING distributed with this software
 *
 * This files contains error handling and logging functions
 */

#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#define IDENT "kplex"

/* This should not be changed once we start multiple threads */
static int facility = -1;

void initlog(int where)
{
    if (facility >=0)
        closelog();

    if ((facility=where) >= 0)
        openlog(IDENT,LOG_NOWAIT,facility);
}

void logdebug(int err, char *fmt, ...)
{
    char *str;
    char ebuf[128];
    va_list ap;

    va_start(ap,fmt);

    if (facility >= 0) {
        if (err && ((str = malloc(strlen(fmt) + 5)) != NULL)) {
            strcpy(str,fmt);
            strcat(str,": %m");
        } else {
            str = fmt;
        }
        vsyslog(LOG_DEBUG,str,ap);
        if (str != fmt)
            free(str);
    } else {
        fprintf(stderr,"%s DEBUG: ",IDENT);
        vfprintf(stderr,fmt,ap);
        if (err) {
            if (strerror_r(err,ebuf,128) == 0 || errno == ERANGE)
                fprintf(stderr,": %s",ebuf);
            else
                fprintf(stderr,": Unknown Error");
        }
        fputc('\n',stderr);
    }
    va_end(ap);
    return;
}

void loginfo(char *fmt, ...)
{
    va_list ap;

    va_start(ap,fmt);

    if (facility >= 0)
        vsyslog(LOG_INFO,fmt,ap);
    else {
        vfprintf(stderr,fmt,ap);
        fputc('\n',stderr);
    }
    va_end(ap);
    return;
}


void logwarn(char *fmt, ...)
{
    va_list ap;

    va_start(ap,fmt);

    if (facility >= 0) {
        vsyslog(LOG_WARNING,fmt,ap);
    } else {
        vfprintf(stderr,fmt,ap);
        fputc('\n',stderr);
    }
    va_end(ap);
    return;
}

void logerr2(int err, char *fmt, va_list args)
{
    char *str;
    char ebuf[128];

    if (facility >= 0) {
        if (err && ((str = malloc(strlen(fmt) + 5)) != NULL)) {
            strcpy(str,fmt);
            strcat(str,": %m");
        } else
            str = fmt;
        vsyslog(LOG_ERR,str,args);
        if (str != fmt)
            free(str);
    } else {
        vfprintf(stderr,fmt,args);
        if (err) {
            if (strerror_r(err,ebuf,128) == 0 || errno == ERANGE)
                fprintf(stderr,": %s",ebuf);
            else
                fprintf(stderr,": Unknown Error");
        }
        fputc('\n',stderr);
    }
    return;
}

void logerr(int err, char *fmt, ...)
{
    va_list ap;

    va_start(ap,fmt);
    logerr2(err,fmt,ap);
    va_end(ap);
}

void logterm(int err, char *fmt, ...)
{
    va_list ap;

    va_start(ap,fmt);
    logerr2(err,fmt,ap);
    va_end(ap);
    err=1;
    pthread_exit(&err);
}

void logtermall(int err, char *fmt, ...)
{
    va_list ap;

    va_start(ap,fmt);
    logerr2(err,fmt,ap);
    va_end(ap);
    kill(getpid(),SIGINT);
    err=1;
    pthread_exit(&err);
}

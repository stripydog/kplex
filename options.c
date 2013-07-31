/* options.c.
 * This flie is part of kplex.
 * Copyright Keith Young 2012-2013
 * For copying information see the file COPYING distributed with this software
 *
 * This file deals with option parsing, either from a file or the command
 * line
 */

#include "kplex.h"
#include <syslog.h>
#include <ctype.h>

#define ARGDELIM ','
#define FILTERDELIM ':'

/* This is used before we start multiple threads */
static char configbuf[BUFSIZE];

void lineerror(unsigned int line)
{
    fprintf(stderr,"Error parsing config file at line %d\n",line);
    exit(1);
}

enum itype name2type(const char *str)
{
    struct iftypedef *iftptr;

    for (iftptr=iftypes; iftptr->index != END; iftptr++)
        if (!strcmp(str,iftptr->name))
            break;

    return(iftptr->index);
}

/*
 * Get the type of the next [Interface] section
 * Args: file pointer to config file, pointer to current line number in the file
 * pointer to an itype
 * Returns: 0 on successfully finding a valid section or nothing but white space
 * being found before end of file, -1 otherwise
 * Side effects: On success, itype is set to the type of the interface name
 * found or END if the end of file is reached without error. line number is
 * incremented as the file is read. If an error other than a syntax error is
 * encountered, the line number is set to 0.
 *
 * Note that this routine reads the *existing* contentes of configbuf and
 * refreshes it only if nothing but white space or comments is found before EOL
 */
int get_interface_section(FILE *fp, unsigned int *line, enum itype *type)
{
    char *ptr,*sptr;
    enum itype ttype;
    /* Discard white space and comments until '[' character is found or EOF
     * any other char is an error */
    for(ptr=configbuf;*ptr != '[';) {
        if (*ptr == ' ' || *ptr == '\t') {
            ++ptr;
            continue;
        }
        if (*ptr == '\n' || *ptr == '#' || *ptr == '\0') {
        /* End of line: Read more */
            if (!fgets(configbuf,BUFSIZE,fp)) {
                if (feof(fp)) {
                    *type=END;
                    return (0);
                }
                line=0;
                return(-1);
            }
            /* Increment line count and start again */
            (*line)++;
            ptr=configbuf;
            continue;
        }
        return(-1);
    }

    /* Eat white space */
    for (++ptr; *ptr == ' ' || *ptr == '\t'; ptr++);

    /* EOL without finding section details is an error */
    if (!*ptr || *ptr == '\n')
        return(-1);

    /* points to start of section name. Find the end of it */
    for(sptr=ptr;*ptr != ']' && *ptr != ' ' && *ptr != '\t';ptr++)
        if (*ptr == '\0' || *ptr == '\n')
        /* End of line before closing ']' */
            return(-1);

    /* Terminate section name with null, checking for closing ']' */
    if (*ptr == ']')
        *ptr++='\0';
    else {
        for (*ptr='\0';*ptr == ' ' || *ptr == '\t';ptr++);
        if (*ptr != ']')
            return(-1);
    }

    /* Check nothing else on the line except white space and comments */
    for (++ptr; *ptr == ' ' || *ptr == '\t'; ptr++);
    if (*ptr && *ptr != '#' && *ptr != '\n')
        return(-1);

    /* Convert string to itype identifier.  Error if string is not known type */
    if ((ttype = name2type(sptr)) == END)
        return(-1);

    /* Set tyoe and return success */
    *type = ttype;
    return(0);
}

/*
 * Get the next config file line
 * Args: FILE pointer to config file, pointer to line counter, pointers to
 * char pointers which will be set with return var/val pair
 * Returns: 0 on success, -1 on error
 * EOF or finding a (potential) section header terminating the current section
 * is "success", but var will be set to NULL
 * Side effects: If error is not a config fil syntax error, line count is set
 * to 0, otherwise it is incremented as the file is read.  var and val are set
 * to point to a var/value pair
 * Anything after a '#' character is ignored
 * Lines containing nothing other than white space are discarded
 * White space at the start and end of a config line is discarded
 * A config line is valid if it contains a single string not starting with '[',
 * optional white space, "=", optional white space, an another string.
 * VERY IMPORTANT: var and val are only valid until the next call to next_config
 * or get_interface_section. This is NOT multithread safe either.
 */
int next_config(FILE *fp, unsigned int *line, char **var, char **val)
{
    char *ptr;
    char quote;
    while ((ptr=fgets(configbuf,BUFSIZE,fp))) {
        /* discard white space */
        for (++(*line);*ptr==' '||*ptr=='\t';++ptr);

        /* end of line or comment: next line */
        if ((*ptr=='#') || (*ptr=='\n') || (*ptr=='\0'))
            continue;

        if (*ptr == '[') {
            /* Section header (terminating this section) */
            *var=NULL;
            return(0);
        }
        break;
    }

    if (!ptr) {
        if (feof(fp)) {
            *configbuf='\0';
            *var=NULL;
            return(0);
        }

        /* Some other error */
        (*line)=0;
        return(-1);
    }

    for (*var=ptr;(*ptr != '=') && (*ptr != ' ') && (*ptr != '\t') ;ptr++) {
        if (*ptr == '\0') {
            return(-1);
        }
    }
    if (*ptr == '=') {
        *ptr++='\0';
    } else {
        *ptr='\0';
        for (*ptr++='\0';*ptr == ' ' || *ptr == '\t';ptr++);
        if (*ptr++ != '=')
            return(-1);
    }

    for (;(*ptr == ' ') || (*ptr == '\t');ptr++);
    if (*ptr == '\'' || *ptr == '"') {
        for (quote=*ptr++,*val=ptr;*ptr != quote; ptr++)
            if (*ptr == '\0' || *ptr == '\n')
                return(-1);
    } else {
        for (*val=ptr;*ptr != ' ' && *ptr != '\t'; ptr++)
            if (*ptr == '\n' || *ptr == '#') {
                *ptr='\0';
                return (0);
            } else if (*ptr == '\0') {
                if (feof(fp))
                    return(0);
                else
                    return(-1);
		}
    }
    *ptr++='\0';
    for (;*ptr == ' ' || *ptr == '\t';ptr++);
        if (*ptr == '\n' || *ptr == '#' || *ptr == '\0')
            return(0);
    return(-1);
}

struct kopts *add_option(char *var, char *val)
{
    struct kopts *vv;

    if (((vv = (struct kopts *)malloc(sizeof(struct kopts))) == NULL) ||
            ((vv->var = strdup(var)) == NULL) ||
            ((vv->val = strdup(val)) == NULL)) {
        if (vv->var)
            free(vv->var);
        if (vv)
            free(vv);
        return (NULL);
    }
    vv->next = NULL;
    return(vv);
}

sfilter_t *getfilter(char *fstring)
{
    char *sptr;
    sfilter_t *head;
    sf_rule_t *filter;
    sf_rule_t *tfilter,**fptr;
    int i,ok=0;

    tfilter=filter=NULL;
    fptr=&filter;

    for (;*fstring;fstring++) {
        ok=0;
        if ((tfilter=(sf_rule_t *)malloc(sizeof(sf_rule_t))) == NULL)
            break;
        tfilter->next=NULL;
        if (*fstring == '+')
            tfilter->info.type=1;
        else if (*fstring == '-')
            tfilter->info.type=0;
        else
            break;

        if (!strncmp(++fstring,"all",3)) {
            memset(tfilter->match,0,5);
            fstring+=3;
        } else {
            for (sptr=tfilter->match,i=0;i<5;i++,fstring++) {
                if (*fstring == '\0' || *fstring == FILTERDELIM)
                    break;
                *sptr++=(*fstring == '*')?0:*fstring;
            }
        }

        if ((*fstring == FILTERDELIM) || (*fstring == '\0')) {
            (*fptr)=tfilter;
            fptr=&tfilter->next;
            ok=1;
            if (*fstring == '\0')
                break;
        } else
            break;
    }
    if (ok) {
        if ((head=(sfilter_t *)malloc(sizeof(sfilter_t))) != NULL) {
            head->type=FILTER;
            pthread_mutex_init(&head->lock,NULL);
            head->refcount=1;
            head->rules=filter;
            return(head);
        }
    }

    if (tfilter)
        free(tfilter);
    for(;filter;filter=tfilter) {
        tfilter=filter->next;
        free(filter);
    }
    return(NULL);
}

int add_common_opt(char *var, char *val,iface_t *ifp)
{
    char *ptr;

    if (!strcasecmp(var,"direction")) {
        if (!strcasecmp(val,"in"))
            ifp->direction = IN;
        else if (!strcasecmp(val,"out"))
            ifp->direction = OUT;
        else if (!strcasecmp(val,"both"))
            ifp->direction = BOTH;
        else return(-2);
    } else if (!strcmp(var,"ifilter")) {
        if (ifp->ifilter)
            free_filter(ifp->ifilter);
        if ((ifp->ifilter=getfilter(val)) == NULL)
		return(-2);
    } else if (!strcmp(var,"ofilter")) {
        if (ifp->ofilter)
            free_filter(ifp->ofilter);
        if ((ifp->ofilter=getfilter(val)) == NULL)
		return(-2);
    } else if (!strcmp(var,"checksum")) {
        if (!strcasecmp(val,"yes")) {
            ifp->checksum=1;
        } else if (!strcasecmp(val,"no")) {
            ifp->checksum=0;
        } else
            return(-2);
    } else if (!strcmp(var,"persist")) {
        if (!strcasecmp(val,"yes")) {
            ifp->persist=1;
        } else if (!strcasecmp(val,"no")) {
            ifp->persist=0;
        } else
            return(-2);
    } else if (!strcasecmp(var,"name")) {
        if ((ifp->name=(char *)malloc(strlen(val)+1)) == NULL)
            return(-1);
        /* Normalize to lower case */
        for (ptr=ifp->name;*val;)
                *ptr++=tolower(*val++);
    } else
        return(1);

    return(0);
}

void free_options(struct kopts *options)
{
    struct kopts *optr,*nextopt;

    for(optr=options;optr;optr=nextopt) {
        nextopt=optr->next;
        free(optr->var);
        free(optr->val);
        free(optr);
    }
}

iface_t *get_config(FILE *fp, unsigned int *line)
{
    char *var,*val;
    struct kopts **opt;
    iface_t *ifp;
    int ret;

    if((ifp = (iface_t *) malloc(sizeof(iface_t))) == NULL) {
        *line = 0;
        return(NULL);
    }
    memset((void *) ifp,0,sizeof(iface_t));

    ifp->direction = BOTH;
    ifp->name=NULL;
    ifp->ifilter=NULL;
    ifp->ofilter=NULL;
    ifp->checksum=-1;
    ifp->options=NULL;

    for(opt = &ifp->options;next_config(fp,line,&var,&val) == 0;) {
        if (!var)
            return(ifp);
        if ((ret = add_common_opt(var,val,ifp)) == 0)
            continue;
        if ((ret < 0) || (((*opt) = add_option(var,val)) == NULL && (ret=-1))) {
            if (ret == -1)
                *line=0;
            break;
        }
        opt=&(*opt)->next;
    }
    free_options(ifp->options);
    free(ifp);
    return(NULL);
}

iface_t *parse_file(char *fname)
{
    FILE *fp;
    unsigned int line=0;
    enum itype type;
    iface_t *ifp,**ifpp;
    iface_t *list = NULL;
    struct if_engine *ifg;

    if ((fp = fopen(fname,"r")) == NULL) {
        fprintf(stderr,"Failed to open config file %s: %s\n",fname,
            strerror(errno));
        exit(1);
    }
    
    for (ifpp=&list;get_interface_section(fp,&line,&type) == 0;) {
        if (type == END) {
            if (!list || list->type != GLOBAL) {
                ifp = get_default_global();
                ifp->next = list;
                list = ifp;
            }
            return(list);
        } else if (type == GLOBAL && list && list->type == GLOBAL) {
            fprintf(stderr,"Error: duplicate global section in config file line %d\n",line);
            exit(1);
        }
        
        if ((ifp = get_config(fp,&line)) == NULL) {
            if (line == 0) {
                perror("Error creating interface");
                exit(1);
            } else {
                lineerror(line);
            }
        }

        if ((ifp->type=type) == GLOBAL) {
            if ((ifg = (struct if_engine *)malloc(sizeof(struct if_engine)))
                    == NULL) {
                free(ifp);
                perror("Error creating interface");
                exit(1);
            }
            ifg->flags=0;
            ifg->logto=LOG_DAEMON;
            ifp->info = (void *)ifg;
            if (ifp->checksum <0)
                ifp->checksum = 0;

            ifp->next=list;
            list=ifp;
            if (ifpp == &list)
                ifpp=&list->next;
        } else {
            (*ifpp)=ifp;
            ifpp=&ifp->next;
        }
    }
    if (line)
        lineerror(line);
    perror("Error parsing config file");
    exit(1);
}

iface_t *parse_arg(char *arg)
{
    iface_t *ifp;
    char *ptr;
    char *var,*val;
    int ret,done=0;
    struct kopts **opt;

    if ((ifp = (iface_t *) malloc(sizeof(iface_t))) == NULL)
        return(NULL);

    memset((void *) ifp,0,sizeof(iface_t));

    ifp->direction = BOTH;
    ifp->checksum=-1;

    for(ptr=arg;*ptr && *ptr != ':';ptr++);
    if (!*ptr) {
        free(ifp);
        return(NULL);
    } else
        *ptr++='\0';

    if (!strcasecmp(arg,"file"))
        ifp->type = FILEIO;
    else if (!strcasecmp(arg,"serial"))
        ifp->type = SERIAL;
    else if (!strcasecmp(arg,"tcp"))
        ifp->type = TCP;
    else if (!strcasecmp(arg,"broadcast"))
        ifp->type = BCAST;
    else if (!strcasecmp(arg,"pty"))
        ifp->type = PTY;
    else if (!strcasecmp(arg,"mcast"))
        ifp->type = MCAST;
    else if (!strcasecmp(arg,"seatalk"))
        ifp->type = ST;
    else {
        fprintf(stderr,"Unrecognised interface type %s\n",arg);
        free(ifp);
        return(NULL);
    }

    for (opt=&ifp->options,var=ptr,val=NULL;;ptr++) {
        if (*ptr == '=') {
            if (val)
                break;
            *ptr='\0';
            val=ptr+1;
        } else if (*ptr == ARGDELIM || *ptr == '\0') {
            if (!val) {
                if (ptr == var) {
                    if (*ptr) {
                        ++var;
                        continue;
                    }
                    done=1;
                }
                break;
            }
            if (*ptr)
                *ptr='\0';
            else
                done=1;
            if ((ret = add_common_opt(var,val,ifp)) < 0) {
                done=0;
                break;
            }
            if (ret) {
                if (((*opt) = add_option(var,val)) == NULL) {
                    done=0;
                    break;
                }
                opt=&(*opt)->next;
            }
            if (done)
                break;
            var=ptr+1;
            val=NULL;
        }
    }
    if (done)
        return(ifp);

    free_options(ifp->options);
    free(ifp);
    return(NULL);
}

int cmdlineopt(struct kopts **options, char *arg)
{
    char *val,*ptr;
    struct kopts *optr;

    for (val=arg;*val && *val != '=';val++);
    if (*val != '=') {
        logerr(0,"Badly formatted option %s\n",arg);
        return(-1);
    }

    for(*val++ = '\0',ptr=val;*ptr;ptr++);

    if ((optr=add_option(arg,val)) == NULL) {
        logerr(errno,"Failed to add option");
        return(-1);
    }

    optr->next=*options;
    *options=optr;
    return(0);
} 

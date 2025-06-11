/*
 * started writing (limited) CGI support for webfsd
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "httpd.h"

/* ---------------------------------------------------------------------- */

extern char **environ;

static char *env_wlist[] = {
    "PATH", "HOME",
    NULL
};

static void env_add(struct strlist **list, char *name, char *value)
{
    char *line;

    line = malloc(strlen(name) + strlen(value) + 2);
    sprintf(line,"%s=%s",name,value);
    if (debug)
	fprintf(stderr,"cgi: env %s\n",line);
    list_add(list,line,1);
}

static char**
env_convert(struct strlist *list)
{
    struct strlist *elem;
    char **env;
    int i;

    for (i = 2, elem = list; NULL != elem; elem = elem->next)
	i++;
    env = malloc(sizeof(char*)*i);
    for (i = 0, elem = list; NULL != elem; elem = elem->next)
	env[i++] = elem->line;
    env[i++] = NULL;
    return env;
}

static void env_copy(struct strlist **list)
{
    int i,j,l;

    for (i = 0; environ[i] != NULL; i++) {
	for (j = 0; env_wlist[j] != NULL; j++) {
	    l = strlen(env_wlist[j]);
	    if (0 == strncmp(environ[i],env_wlist[j],l) &&
		environ[i][l] == '=') {
		env_add(list,env_wlist[j],environ[i]+l+1);
		break;
	    }
	}
    }
}

/* ---------------------------------------------------------------------- */

void
cgi_request(struct REQUEST *req)
{
    struct sockaddr_storage addr;
    struct strlist *env = NULL, *item;
    char host[65],serv[9];
    char filename[1024], *h, *argv[2], envname[128];
    int pid,p[2],i,length;

    if (debug)
	fprintf(stderr,"%03d: is cgi request\n",req->fd);
    if (-1 == pipe(p)) {
	mkerror(req,500,0);
	return;
    }
    pid = fork();
    switch (pid) {
    case -1:
	/* error */
	if (debug)
	    perror("fork");
	mkerror(req,500,0);
	return;
    case 0:
	break;
    default:
	/* parent - webfsd */
	close(p[1]);
	req->cgipid  = pid;
	req->cgipipe = p[0];
	req->state   = STATE_CGI_HEADER;
	close_on_exec(req->cgipipe);
	fcntl(req->cgipipe,F_SETFL,O_NONBLOCK);
	return;
    }

    /* -------- below is the child process (cgi) code -------- */

    /* lookup local socket (before it gets closed) */
    socklen_t addr_length = sizeof(addr);
    getsockname(req->fd,(struct sockaddr*)&addr,&addr_length);
    getnameinfo((struct sockaddr*)&addr,addr_length,host,64,serv,8,
		NI_NUMERICHOST | NI_NUMERICSERV);
    
    /* setup file descriptors */
    dup2(p[1],1); /* pipe -> stdout */
    if (have_tty) {
	int devnull = open("/dev/null",O_RDWR);
	dup2(devnull,0); /* stdin  */
	dup2(devnull,2); /* stderr */
	close(devnull);
    } else {
	/* nothing -- already attached to /dev/null */
    }
    close_on_exec(p[0]);
    close_on_exec(p[1]);

    /* setup environment */
    env_copy(&env);

    env_add(&env,"DOCUMENT_ROOT",doc_root);
    env_add(&env,"GATEWAY_INTERFACE","CGI/1.1");
    env_add(&env,"QUERY_STRING",req->query);
    env_add(&env,"REQUEST_URI",req->uri);
    env_add(&env,"REMOTE_ADDR",req->peerhost);
    env_add(&env,"REMOTE_PORT",req->peerserv);
    env_add(&env,"REQUEST_METHOD",req->type);
    env_add(&env,"SERVER_ADMIN","root@localhost");
    env_add(&env,"SERVER_NAME",server_host);
    env_add(&env,"SERVER_PROTOCOL","HTTP/1.1");
    env_add(&env,"SERVER_SOFTWARE",server_name);
    env_add(&env,"SERVER_ADDR",host);
    env_add(&env,"SERVER_PORT",serv);

    for (item = req->header; NULL != item; item = item->next) {
	strcpy(envname,"HTTP_");
	if (1 != sscanf(item->line,"%120[-A-Za-z]: %n",envname+5,&length))
	    continue;
	for (i = 0; envname[i]; i++) {
	    if (isalpha(envname[i]))
		envname[i] = toupper(envname[i]);
	    if ('-' == envname[i])
		envname[i] = '_';
	}
	env_add(&env,envname,item->line+length);
    }

    h = req->path + strlen(cgipath);
    h = strchr(h,'/');
    if (h) {
	env_add(&env,"PATH_INFO",h);
	*h = 0;
    } else {
	env_add(&env,"PATH_INFO","");
    }
    env_add(&env,"SCRIPT_NAME",req->path);
    snprintf(filename,sizeof(filename)-1,"%s%s",doc_root,req->path);
    env_add(&env,"SCRIPT_FILENAME",filename);

    /* start cgi app */
    argv[0] = filename;
    argv[1] = NULL;
    execve(filename,argv,env_convert(env));

    /* exec failed ... */
    printf("Content-Type: text/plain\n"
	   "\n"
	   "execve %s: %s\n",
	   filename,strerror(errno));
    exit(1);
}

/* ---------------------------------------------------------------------- */

void
cgi_read_header(struct REQUEST *req)
{
    struct strlist  *list = NULL;
    char            *h,*next,*status = NULL;
    int             rc;

 restart:
    rc = read(req->cgipipe, req->cgibuf+req->cgilen, MAX_HEADER-req->cgilen);
    switch (rc) {
    case -1:
	if (errno == EAGAIN)
	    return;
	if (errno == EINTR)
	    goto restart;
	/* fall through */
    case 0:
	mkerror(req,500,0);
	return;
    default:
	req->cgilen += rc;
	req->cgibuf[req->cgilen] = 0;
    }

    /* header complete ?? */
    if (NULL != (h = strstr(req->cgibuf,"\r\n\r\n")) ||
	NULL != (h = strstr(req->cgibuf,"\n\n"))) {

	/* parse cgi header */
	for (h = req->cgibuf;; h = next) {
	    next = strstr(h,"\n");
	    next[0] = 0;
	    if (next[-1] == '\r')
		next[-1] = 0;
	    next++;
	    
	    if (0 == strlen(h))
		break;
	    if (debug)
		fprintf(stderr,"%03d: cgi: hdr %s\n",req->fd,h);
	    if (0 == strncasecmp(h,"Status: ",8)) {
		status = h+8;
		if (debug)
		    fprintf(stderr,"%03d: cgi: status %s\n",req->fd,status);
		continue;
	    }
	    if (0 == strncasecmp(h,"Server:",7)         ||
		0 == strncasecmp(h,"Connection:",11)    ||
		0 == strncasecmp(h,"Accept-Ranges:",14) ||
		0 == strncasecmp(h,"Date:",5))
		/* webfsd adds them -- filter out */
		continue;
	    list_add(&list,h,0);
	}
	mkcgi(req, status ? status : "200 OK", list);
	list_free(&list);
	req->cgipos = next - req->cgibuf;
	if (debug)
	    fprintf(stderr,"%03d: cgi: pos=%d len=%d\n",req->fd,
		    req->cgipos, req->cgilen);
	return;
    }

    if (req->cgilen == MAX_HEADER) {
	mkerror(req,400,0);
	return;
    }
    return;
}

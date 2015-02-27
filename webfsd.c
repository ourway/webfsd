#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/signal.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "httpd.h"

/* ---------------------------------------------------------------------- */
/* public variables - server configuration                                */

char    *server_name   = "webfs/" WEBFS_VERSION;

int     debug          = 0;
int     dontdetach     = 0;
int     timeout        = 60;
int     keepalive_time = 5;
int     tcp_port       = 0;
int     max_dircache   = 128;
char    *doc_root      = ".";
char    *indexhtml     = NULL;
char    *cgipath       = NULL;
char    *listen_ip     = NULL;
char    *listen_port   = "8000";
int     virtualhosts   = 0;
int     canonicalhost  = 0;
char    server_host[256];
char    user[17];
char    group[17];
char    *mimetypes     = MIMEFILE;
char    *pidfile       = NULL;
char    *logfile       = NULL;
FILE    *logfh         = NULL;
char    *userpass      = NULL;
char    *userdir       = NULL;
int     flushlog       = 0;
int     do_chroot      = 0;
int     usesyslog      = 0;
int     have_tty       = 1;
int     max_conn       = 32;
int     lifespan       = -1;
int     no_listing     = 0;

time_t  now;
int     slisten;

#ifdef USE_THREADS
pthread_mutex_t lock_logfile = PTHREAD_MUTEX_INITIALIZER;
int       nthreads = 1;
pthread_t *threads;
#endif

#ifdef USE_SSL
char	*certificate   = "server.pem";
char	*password;
int	with_ssl       = 0;
SSL_CTX *ctx;
BIO	*sbio, *ssl_bio;
#endif

/* ---------------------------------------------------------------------- */

static int termsig,got_sighup;

static void catchsig(int sig)
{
    if (SIGTERM == sig || SIGINT == sig)
	termsig = sig;
    if (SIGHUP == sig)
	got_sighup = 1;
}

/* ---------------------------------------------------------------------- */

static void
usage(char *name)
{
    char           *h;
    struct passwd  *pw;
    struct group   *gr;

    h = strrchr(name,'/');
    fprintf(stderr,
	    "This is a lightweight http server for static content\n"
	    "\n"
	    "usage: %s [ options ]\n"
	    "\n"
	    "Options:\n"
	    "  -h       print this text\n"
	    "  -4       use ipv4\n"
	    "  -6       use ipv6\n"
	    "  -d       enable debug output                 [%s]\n"
	    "  -F       do not fork into background         [%s]\n"
	    "  -s       enable syslog (start/stop/errors)   [%s]\n"
	    "  -t sec   set network timeout                 [%i]\n"
	    "  -c n     set max. allowed connections        [%i]\n"
	    "  -a n     set max. cached dirs                [%i]\n"
	    "  -j       disable directory listings          [%s]\n"
#ifdef USE_THREADS
	    "  -y n     startup n threads                   [%i]\n"
#endif
	    "  -p port  use tcp-port >port<                 [%s]\n"
	    "  -r dir   document root is >dir<              [%s]\n"
	    "  -R dir   same as above + chroot to >dir<\n"
	    "  -f file  look for >file< as directory index  [%s]\n"
	    "  -n host  server hostname is >host<           [%s]\n"
	    "  -N host  same as above + UseCanonicalName\n"
	    "  -i ip    bind to IP-address >ip<             [%s]\n"
	    "  -v       enable virtual hosts                [%s]\n"
	    "  -l log   write access log to file >log<      [%s]\n"
	    "  -L log   same as above + flush every line\n"
	    "  -m file  read mime types from >file<         [%s]\n"
	    "  -k file  use >file< as pidfile               [%s]\n"
	    "  -b user:pass  password protect the exported\n"
	    "           files (basic authentication)\n"
	    "  -e sec   limit live span of files to sec\n"
	    "           seconds (using expires header)\n"
#ifdef USE_SSL
	    "  -S       enable SSL mode\n"
	    "  -C file  SSL-Certificate file                [%s]\n"
	    "  -P pass  SSL-Certificate password\n"
#endif
	    "  -x dir   CGI script directory (relative to\n"
	    "           document root)                      [%s]\n"
	    "  -~ dir   user home directory (will expand\n"
	    "           /~user/path to $HOME/dir/path\n",
	    h ? h+1 : name,
 	    debug     ?  "on" : "off",
 	    dontdetach ?  "on" : "off",
	    usesyslog ?  "on" : "off",
	    timeout, max_conn, max_dircache,
	    no_listing ? "on" : "off",
#ifdef USE_THREADS
	    nthreads,
#endif
	    listen_port, doc_root,
	    indexhtml ? indexhtml : "none",
	    server_host,
	    listen_ip ? listen_ip : "any",
	    virtualhosts ? "on" : "off",
	    logfile ? logfile : "none",
	    mimetypes,
	    pidfile ? pidfile : "none",
#ifdef USE_SSL
	    certificate,
#endif
	    cgipath ? cgipath : "none");
    if (getuid() == 0) {
	pw = getpwuid(0);
	gr = getgrgid(getgid());
	fprintf(stderr,
		"  -u user  run as user >user<                  [%s]\n"
		"  -g group run as group >group<                [%s]\n",
		pw ? pw->pw_name : "???",
		gr ? gr->gr_name : "???");
    }
    exit(1);
}

static void run_as(int id)
{
    if (-1 == seteuid(id)) {
	fprintf(stderr,"seteuid(%d): %s\n",id,strerror(errno));
	exit(1);
    }
    if (debug)
	fprintf(stderr,"run_as: uid=%d euid=%d\n",getuid(),geteuid());
}

static void
fix_ug(void)
{
    struct passwd  *pw = NULL;
    struct group   *gr = NULL;
    
    /* root is allowed to use any uid/gid,
     * others will get their real uid/gid */
    if (0 == getuid() && strlen(user) > 0) {
	if (NULL == (pw = getpwnam(user)))
	    pw = getpwuid(atoi(user));
    } else {
	pw = getpwuid(getuid());
    }
    if (0 == getuid() && strlen(group) > 0) {
	if (NULL == (gr = getgrnam(group)))
	    gr = getgrgid(atoi(group));
    } else {
	gr = getgrgid(getgid());
    }

    if (NULL == pw) {
	xerror(LOG_ERR,"user unknown",NULL);
	exit(1);
    }
    if (NULL == gr) {
	xerror(LOG_ERR,"group unknown",NULL);
	exit(1);
    }

    /* chroot to $DOCUMENT_ROOT (must be done here as getpwuid needs
       /etc and chroot works as root only) */
    if (do_chroot) {
	chdir(doc_root);
	if (-1 == chroot(doc_root)) {
	    xperror(LOG_ERR,"chroot",NULL);
	    exit(1);
	}
    }

    /* set group */
    if (getegid() != gr->gr_gid || getgid() != gr->gr_gid) {
	setgid(gr->gr_gid);
	setgroups(0, NULL);
    }
    if (getegid() != gr->gr_gid || getgid() != gr->gr_gid) {
	xerror(LOG_ERR,"setgid failed",NULL);
	exit(1);
    }
    strncpy(group,gr->gr_name,16);

    /* set user */
    if (geteuid() != pw->pw_uid || getuid() != pw->pw_uid)
	setuid(pw->pw_uid);
    if (geteuid() != pw->pw_uid || getuid() != pw->pw_uid) {
	xerror(LOG_ERR,"setuid failed",NULL);
	exit(1);
    }
    strncpy(user,pw->pw_name,16);

    if (debug)
	fprintf(stderr,"fix_ug: uid=%d euid=%d / gid=%d egid=%d\n",
		getuid(),geteuid(),getgid(),getegid());
}

/* ---------------------------------------------------------------------- */

static void
access_log(struct REQUEST *req, time_t now)
{
    char timestamp[32];

    DO_LOCK(lock_logfile);
    if (NULL == logfh) {
	DO_UNLOCK(lock_logfile);
	return;
    }

    /* common log format: host ident authuser date request status bytes */
    strftime(timestamp,31,"[%d/%b/%Y:%H:%M:%S +0000]",localtime(&now));
    if (0 == req->status)
	req->status = 400; /* bad request */
    if (400 == req->status) {
	fprintf(logfh,"%s - - %s \"-\" 400 %d\n",
		req->peerhost,
		timestamp,
		req->bc);
    } else {
	fprintf(logfh,"%s - - %s \"%s %s HTTP/%d.%d\" %d %d\n",
		req->peerhost,
		timestamp,
		req->type,
		req->uri,
		req->major,
		req->minor,
		req->status,
		req->bc);
    }
    if (flushlog)
	fflush(logfh);
    DO_UNLOCK(lock_logfile);
}

/*
 * loglevel usage
 *   ERR    : fatal errors (which are followed by exit(1))
 *   WARNING: this should'nt happen error (oom, ...)
 *   NOTICE : start/stop of the daemon
 *   INFO   : "normal" errors (canceled downloads, timeouts,
 *            stuff what happens all the time)
 */

static void
syslog_init(void)
{
    openlog("webfsd",LOG_PID, LOG_DAEMON);
}

static void
syslog_start(void)
{
    syslog(LOG_NOTICE,
	   "started (listen on %s:%d, root=%s, user=%s, group=%s)\n",
	   listen_ip ? listen_ip : "*",
	   tcp_port,doc_root,user,group);
}

static void
syslog_stop(void)
{
    if (termsig)
	syslog(LOG_NOTICE,"stopped on signal %d (%s)\n",
	       termsig,strsignal(termsig));
    else
	syslog(LOG_NOTICE,"stopped\n");
    closelog();
}

void
xperror(int loglevel, char *txt, char *peerhost)
{
    if (LOG_INFO == loglevel && usesyslog < 2 && !debug)
	return;
    if (have_tty) {
	if (NULL == peerhost)
	    perror(txt);
	else
	    fprintf(stderr,"%s: %s (peer=%s)\n",txt,strerror(errno),
		    peerhost);
    }
    if (usesyslog) {
	if (NULL == peerhost)
	    syslog(loglevel,"%s: %s\n",txt,strerror(errno));
	else
	    syslog(loglevel,"%s: %s (peer=%s)\n",txt,strerror(errno),
		   peerhost);
    }
}

void
xerror(int loglevel, char *txt, char *peerhost)
{
    if (LOG_INFO == loglevel && usesyslog < 2 && !debug)
	return;
    if (have_tty) {
	if (NULL == peerhost)
	    fprintf(stderr,"%s\n",txt);
	else
	    fprintf(stderr,"%s (peer=%s)\n",txt,peerhost);
    }
    if (usesyslog) {
	if (NULL == peerhost)
	    syslog(loglevel,"%s\n",txt);
	else
	    syslog(loglevel,"%s (peer=%s)\n",txt,peerhost);
    }	
}

/* ---------------------------------------------------------------------- */
/* main loop                                                              */

static void*
mainloop(void *thread_arg)
{
    struct REQUEST *conns = NULL;
    int curr_conn = 0;

    struct REQUEST      *req,*prev,*tmp;
    struct timeval      tv;
    int                 max,length;
    fd_set              rd,wr;

    for (;!termsig;) {
	if (got_sighup) {
	    if (NULL != logfile && 0 != strcmp(logfile,"-")) {
		if (debug)
		    fprintf(stderr,"got SIGHUP, reopen logfile %s\n",logfile);
		DO_LOCK(lock_logfile);
		if (logfh)
		    fclose(logfh);
		if (NULL == (logfh = fopen(logfile,"a")))
		    xperror(LOG_WARNING,"reopen access log",NULL);
		else
		    close_on_exec(fileno(logfh));
		DO_UNLOCK(lock_logfile);
	    }
	    got_sighup = 0;
	}
	FD_ZERO(&rd);
	FD_ZERO(&wr);
	max = 0;
	/* add listening socket */
	if (curr_conn < max_conn) {
	    FD_SET(slisten,&rd);
	    max = slisten;
	}
	/* add connection sockets */
	for (req = conns; req != NULL; req = req->next) {
	    switch (req->state) {
	    case STATE_KEEPALIVE:
	    case STATE_READ_HEADER:
		FD_SET(req->fd,&rd);
		if (req->fd > max)
		    max = req->fd;
		break;
	    case STATE_WRITE_HEADER:
	    case STATE_WRITE_BODY:
	    case STATE_WRITE_FILE:
	    case STATE_WRITE_RANGES:
	    case STATE_CGI_BODY_OUT:
		FD_SET(req->fd,&wr);
#ifdef USE_SSL
		if (with_ssl)
		    FD_SET(req->fd,&rd);
#endif
		if (req->fd > max)
		    max = req->fd;
		break;
	    case STATE_CGI_HEADER:
	    case STATE_CGI_BODY_IN:
		FD_SET(req->cgipipe,&rd);
		if (req->cgipipe > max)
		    max = req->cgipipe;
		break;
	    }
	}
	/* go! */
	tv.tv_sec  = keepalive_time;
	tv.tv_usec = 0;
	if (-1 == select(max+1,&rd,&wr,NULL,(curr_conn > 0) ? &tv : NULL)) {
	    if (debug)
		perror("select");
	    continue;
	}
	now = time(NULL);

	/* new connection ? */
	if (FD_ISSET(slisten,&rd)) {
	    req = malloc(sizeof(struct REQUEST));
	    if (NULL == req) {
		/* oom: let the request sit in the listen queue */
		if (debug)
		    fprintf(stderr,"oom\n");
	    } else {
		memset(req,0,sizeof(struct REQUEST));
		if (-1 == (req->fd = accept(slisten,NULL,NULL))) {
		    if (EAGAIN != errno)
			xperror(LOG_WARNING,"accept",NULL);
		    free(req);
		} else {
		    close_on_exec(req->fd);
		    fcntl(req->fd,F_SETFL,O_NONBLOCK);
		    req->bfd = -1;
		    req->cgipipe = -1;
		    req->state = STATE_READ_HEADER;
		    req->ping = now;
		    req->next = conns;
		    conns = req;
		    curr_conn++;
		    if (debug)
			fprintf(stderr,"%03d: new request (%d)\n",req->fd,curr_conn);
#ifdef USE_SSL
		    if (with_ssl)
			open_ssl_session(req);
#endif
		    length = sizeof(req->peer);
		    if (-1 == getpeername(req->fd,(struct sockaddr*)&(req->peer),&length)) {
			xperror(LOG_WARNING,"getpeername",NULL);
			req->state = STATE_CLOSE;
		    }
		    getnameinfo((struct sockaddr*)&req->peer,length,
				req->peerhost,64,req->peerserv,8,
				NI_NUMERICHOST | NI_NUMERICSERV);
		    if (debug)
			fprintf(stderr,"%03d: connect from (%s)\n",
				req->fd,req->peerhost);
		}
	    }
	}

	/* check active connections */
	for (req = conns, prev = NULL; req != NULL;) {
	    /* handle I/O */
	    switch (req->state) {
	    case STATE_KEEPALIVE:
	    case STATE_READ_HEADER:
		if (FD_ISSET(req->fd,&rd)) {
		    req->state = STATE_READ_HEADER;
		    read_request(req,0);
		    req->ping = now;
		}
		break;
	    case STATE_WRITE_HEADER:
	    case STATE_WRITE_BODY:
	    case STATE_WRITE_FILE:
	    case STATE_WRITE_RANGES:
	    case STATE_CGI_BODY_OUT:
		if (FD_ISSET(req->fd,&wr)) {
		    write_request(req);
		    req->ping = now;
		}
#ifdef USE_SSL
		if (with_ssl && FD_ISSET(req->fd,&rd)) {
		    write_request(req);
		    req->ping = now;
		}
#endif
		break;
	    case STATE_CGI_HEADER:
		if (FD_ISSET(req->cgipipe,&rd)) {
		    cgi_read_header(req);
		    req->ping = now;
		}
		break;
	    case STATE_CGI_BODY_IN:
		if (FD_ISSET(req->cgipipe,&rd)) {
		    write_request(req);
		    req->ping = now;
		}
		break;
	    }

	    /* check timeouts */
	    if (req->state == STATE_KEEPALIVE) {
		if (now > req->ping + keepalive_time ||
		    curr_conn > max_conn * 9 / 10) {
		    if (debug)
			fprintf(stderr,"%03d: keepalive timeout\n",req->fd);
		    req->state = STATE_CLOSE;
		}
	    } else {
		if (now > req->ping + timeout) {
		    if (req->state == STATE_READ_HEADER) {
			mkerror(req,408,0);
		    } else {
			xerror(LOG_INFO,"network timeout",req->peerhost);
			req->state = STATE_CLOSE;
		    }
		}
	    }

	    /* header parsing */
header_parsing:
	    if (req->state == STATE_PARSE_HEADER) {
		parse_request(req);
		if (req->state == STATE_WRITE_HEADER)
		    write_request(req);
	    }

	    /* handle finished requests */
	    if (req->state == STATE_FINISHED && !req->keep_alive)
		req->state = STATE_CLOSE;
	    if (req->state == STATE_FINISHED) {
		if (logfh)
		    access_log(req,now);
		/* cleanup */
		req->auth[0]       = 0;
		req->if_modified   = NULL;
		req->if_unmodified = NULL;
		req->if_range      = NULL;
		req->range_hdr     = NULL;
		req->ranges        = 0;
		if (req->r_start) { free(req->r_start); req->r_start = NULL; }
		if (req->r_end)   { free(req->r_end);   req->r_end   = NULL; }
		if (req->r_head)  { free(req->r_head);  req->r_head  = NULL; }
		if (req->r_hlen)  { free(req->r_hlen);  req->r_hlen  = NULL; }
		list_free(&req->header);
		memset(req->mtime,   0, sizeof(req->mtime));

		if (req->bfd != -1) {
		    close(req->bfd);
		    req->bfd  = -1;
		}
		if (req->cgipipe != -1) {
		    close(req->cgipipe);
		    req->cgipipe  = -1;
		}
		if (req->cgipid) {
		    kill(req->cgipid,SIGTERM);
		    req->cgipid = 0;
		}
		req->body      = NULL;
		req->written   = 0;
		req->head_only = 0;
		req->rh        = 0;
		req->rb        = 0;
		if (req->dir) {
		    free_dir(req->dir);
		    req->dir = NULL;
		}
		req->hostname[0] = 0;
		req->path[0]     = 0;
		req->query[0]    = 0;

		if (req->hdata == req->lreq) {
		    /* ok, wait for the next one ... */
		    if (debug)
			fprintf(stderr,"%03d: keepalive wait\n",req->fd);
		    req->state = STATE_KEEPALIVE;
		    req->hdata = 0;
		    req->lreq  = 0;
#ifdef TCP_CORK
		    if (1 == req->tcp_cork) {
			req->tcp_cork = 0;
			if (debug)
			    fprintf(stderr,"%03d: tcp_cork=%d\n",req->fd,req->tcp_cork);
			setsockopt(req->fd,SOL_TCP,TCP_CORK,&req->tcp_cork,sizeof(int));
		    }
#endif
		} else {
		    /* there is a pipelined request in the queue ... */
		    if (debug)
			fprintf(stderr,"%03d: keepalive pipeline\n",req->fd);
		    req->state = STATE_READ_HEADER;
		    memmove(req->hreq,req->hreq+req->lreq,
			    req->hdata-req->lreq);
		    req->hdata -= req->lreq;
		    req->lreq  =  0;
		    read_request(req,1);
		    goto header_parsing;
		}
	    }

	    /* connections to close */
	    if (req->state == STATE_CLOSE) {
		if (logfh)
		    access_log(req,now);
		/* cleanup */
		close(req->fd);
#ifdef USE_SSL
		if (with_ssl)
		    SSL_free(req->ssl_s);
#endif
		if (req->bfd != -1)
		    close(req->bfd);
		if (req->cgipipe != -1)
		    close(req->cgipipe);
		if (req->cgipid)
		    kill(req->cgipid,SIGTERM);
		if (req->dir)
		    free_dir(req->dir);
		curr_conn--;
		if (debug)
		    fprintf(stderr,"%03d: done (%d)\n",req->fd,curr_conn);
		/* unlink from list */
		tmp = req;
		if (prev == NULL) {
		    conns = req->next;
		    req = conns;
		} else {
		    prev->next = req->next;
		    req = req->next;
		}
		/* free memory  */
		if (tmp->r_start) free(tmp->r_start);
		if (tmp->r_end)   free(tmp->r_end);
		if (tmp->r_head)  free(tmp->r_head);
		if (tmp->r_hlen)  free(tmp->r_hlen);
		list_free(&tmp->header);
		free(tmp);
	    } else {
		prev = req;
		req = req->next;
	    }
	}
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
    struct sigaction         act,old;
    struct addrinfo          ask,*res;
    struct sockaddr_storage  ss;
    int c, opt, rc, ss_len, pid=0, v4 = 1, v6 = 1;
    int uid,euid;
    char host[INET6_ADDRSTRLEN+1];
    char serv[16];
    char mypid[12];

    uid  = getuid();
    euid = geteuid();
    if (uid != euid)
	run_as(uid);
    gethostname(server_host,255);
    memset(&ask,0,sizeof(ask));
    ask.ai_flags = AI_CANONNAME;
    if (0 == (rc = getaddrinfo(server_host, NULL, &ask, &res))) {
	if (res->ai_canonname)
	    strcpy(server_host,res->ai_canonname);
    }
    
    /* parse options */
    for (;;) {
	if (-1 == (c = getopt(argc,argv,"hvsdF46jS"
			      "r:R:f:p:n:N:i:t:c:a:u:g:l:L:m:y:b:k:e:x:C:P:~:")))
	    break;
	switch (c) {
	case 'h':
	    usage(argv[0]);
	    break;
	case '4':
	    v4 = 1;
	    v6 = 0;
	    break;
	case '6':
	    v4 = 0;
	    v6 = 1;
	    break;
	case 's':
	    usesyslog++;
	    break;
	case 'd':
	    debug++;
	    break;
	case 'v':
	    virtualhosts++;
	    break;
	case 'F':
	    dontdetach++;
	    break;
	case 'R':
	    do_chroot = 1;
	    /* fall through */
	case 'r':
	    doc_root = optarg;
	    break;
	case 'f':
	    indexhtml = optarg;
	    break;
	case 'N':
	    canonicalhost = 1;
	    /* fall through */
	case 'n':
	    strncpy(server_host,optarg,64);
	    break;
	case 'i':
	    listen_ip = optarg;
	    break;
	case 'p':
	    listen_port = optarg;
	    break;
	case 't':
	    timeout = atoi(optarg);
	    break;
	case 'c':
	    max_conn = atoi(optarg);
	    break;
	case 'a':
	    max_dircache = atoi(optarg);
	    break;
	case 'u':
	    strncpy(user,optarg,16);
	    break;
	case 'g':
	    strncpy(group,optarg,16);
	    break;
	case 'L':
	    flushlog = 1;
	    /* fall through */
	case 'l':
	    logfile = optarg;
	    break;
	case 'm':
	    mimetypes = optarg;
	    break;
	case 'k':
	    pidfile = optarg;
	    break;
	case 'b':
	    userpass = strdup(optarg);
	    memset(optarg,'x',strlen(optarg));
	    break;
	case 'e':
	    lifespan = atoi(optarg);
	    break;
	case 'x':
	    if (optarg[strlen(optarg)-1] == '/') {
		cgipath = optarg;
	    } else {
		cgipath = malloc(strlen(optarg)+2);
		sprintf(cgipath,"%s/",optarg);
	    }
	    break;
#ifdef USE_THREADS
	case 'y':
	    nthreads = atoi(optarg);
	    break;
#endif
#ifdef USE_SSL
	case 'S':
	    with_ssl++;
	    break;
	case 'C':
	    certificate = optarg;
	    break;
	case 'P':
	    password = strdup(optarg);
	    memset(optarg,'x',strlen(optarg));
	    break;
#endif
	case 'j':
	    no_listing = 1;
	    break;
	case '~':
	    userdir = optarg;
	    break;
	default:
	    exit(1);
	}
    }
    if (usesyslog)
	syslog_init();

    /* bind to socket */
    slisten = -1;
    memset(&ask,0,sizeof(ask));
    ask.ai_flags = AI_PASSIVE;
    if (listen_ip)
	ask.ai_flags |= AI_CANONNAME;
    ask.ai_socktype = SOCK_STREAM;

    /* try ipv6 first ... */
    if (-1 == slisten  &&  v6) {
	ask.ai_family = PF_INET6;
	if (0 != (rc = getaddrinfo(listen_ip, listen_port, &ask, &res))) {
	    if (debug)
		fprintf(stderr,"getaddrinfo (ipv6): %s\n",gai_strerror(rc));
	} else {
	    if (-1 == (slisten = socket(res->ai_family, res->ai_socktype,
					res->ai_protocol)) && debug)
		xperror(LOG_ERR,"socket (ipv6)",NULL);
	}
    }
	
    /* ... failing that try ipv4 */
    if (-1 == slisten  &&  v4) {
	ask.ai_family = PF_INET;
	if (0 != (rc = getaddrinfo(listen_ip, listen_port, &ask, &res))) {
	    fprintf(stderr,"getaddrinfo (ipv4): %s\n",gai_strerror(rc));
	    exit(1);
	}
	if (-1 == (slisten = socket(res->ai_family, res->ai_socktype,
				    res->ai_protocol))) {
	    xperror(LOG_ERR,"socket (ipv4)",NULL);
	    exit(1);
	}
    }

    if (-1 == slisten)
	exit(1);
    close_on_exec(slisten);

    memcpy(&ss,res->ai_addr,res->ai_addrlen);
    ss_len = res->ai_addrlen;
    if (res->ai_canonname)
	strcpy(server_host,res->ai_canonname);
    if (0 != (rc = getnameinfo((struct sockaddr*)&ss,ss_len,
			       host,INET6_ADDRSTRLEN,serv,15,
			       NI_NUMERICHOST | NI_NUMERICSERV))) {
	fprintf(stderr,"getnameinfo: %s\n",gai_strerror(rc));
	exit(1);
    }

    tcp_port = atoi(serv);
    opt = 1;
    setsockopt(slisten,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    fcntl(slisten,F_SETFL,O_NONBLOCK);

    /* Use accept filtering, if available. */
#ifdef SO_ACCEPTFILTER
    {
	struct accept_filter_arg af;
	memset(&af,0,sizeof(af));
	strcpy(af.af_name,"httpready");
	setsockopt(slisten, SOL_SOCKET, SO_ACCEPTFILTER, (char*)&af, sizeof(af));
    }
#endif /* SO_ACCEPTFILTER */

    if (uid != euid)
	run_as (euid);
    if (-1 == bind(slisten, (struct sockaddr*) &ss, ss_len)) {
	xperror(LOG_ERR,"bind",NULL);
        exit(1);
    }
    if (uid != euid)
	run_as (uid);
    if (-1 == listen(slisten, 2*max_conn)) {
	xperror(LOG_ERR,"listen",NULL);
        exit(1);
    }

    /* init misc stuff */
    init_mime(mimetypes,"text/plain");
    init_quote();
#ifdef USE_SSL
    if (with_ssl)
	init_ssl();
#endif

    /* change user/group - also does chroot */
    if (uid != euid)
	run_as (euid);
    fix_ug();

    if (logfile) {
	if (0 == strcmp(logfile,"-")) {
	    logfh = stdout;
	} else {
	    if (NULL == (logfh = fopen(logfile,"a")))
		xperror(LOG_WARNING,"open access log",NULL);
	    else
		close_on_exec(fileno(logfh));
	}
    }

    if (pidfile) {
	if (-1 == (pid = open(pidfile,O_WRONLY | O_CREAT | O_EXCL, 0600))) {
	    fprintf(stderr,"open %s: %s\n",pidfile,strerror(errno));
	    exit(1);
	}
	close_on_exec(pid);
    }

    if (debug) {
	fprintf(stderr,
		"http server started\n"
		"  ipv6  : %s\n"
#ifdef USE_SSL
	        "  ssl   : %s\n"
#endif
		"  node  : %s\n"
		"  ipaddr: %s\n"
		"  port  : %d\n"
		"  export: %s\n"
		"  user  : %s\n"
		"  group : %s\n",
		res->ai_family == PF_INET6 ? "yes" : "no",
#ifdef USE_SSL
		with_ssl ? "yes" : "no",
#endif
		server_host,host,tcp_port,doc_root,user,group);
    }

    /* run as daemon - detach from terminal */
    if ((!debug) && (!dontdetach)) {
        switch (fork()) {
        case -1:
	    xperror(LOG_ERR,"fork",NULL);
	    exit(1);
        case 0:
            close(0); close(1); close(2); setsid();
	    have_tty = 0;
            break;
        default:
            exit(0);
        }
    }
    if (usesyslog) {
	syslog_start();
	atexit(syslog_stop);
    }
    if (pidfile) {
	sprintf(mypid,"%d",getpid());
	write(pid,mypid,strlen(mypid));
	close(pid);
    }

    /* setup signal handler */
    memset(&act,0,sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE,&act,&old);
    sigaction(SIGCHLD,&act,&old);
    act.sa_handler = catchsig;
    sigaction(SIGHUP,&act,&old);
    sigaction(SIGTERM,&act,&old);
    if (debug)
	sigaction(SIGINT,&act,&old);

    /* go! */
#ifdef USE_THREADS
    if (nthreads > 1) {
	int i;
	threads = malloc(sizeof(pthread_t) * nthreads);
	for (i = 1; i < nthreads; i++) {
	    pthread_create(threads+i,NULL,mainloop,threads+i);
	    pthread_detach(threads[i]);
	}
    }
#endif
    mainloop(NULL);
    
#ifdef USE_SSL
    if (with_ssl)
	SSL_CTX_free(ctx);
#endif
    if (logfh)
	fclose(logfh);
    if (pidfile)
	unlink(pidfile);
    if (debug)
	fprintf(stderr,"bye...\n");
    exit(0);
}

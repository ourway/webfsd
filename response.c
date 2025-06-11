#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <time.h>
#include <limits.h>
#include <inttypes.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "httpd.h"

/* ---------------------------------------------------------------------- */
/* os-specific sendfile() wrapper                                         */

/*
 * int xsendfile(out,in,offset,bytes)
 *
 *	out    - outgoing filedescriptor (i.e. the socket)
 *	in     - incoming filedescriptor (i.e. the file to send out)
 *	offset - file offset (where to start)
 *      bytes  - number of bytes to send
 *
 * return value
 *	on error:   -1 and errno set.
 *	on success: the number of successfully written bytes (which might
 *		    be smaller than bytes, we are doing nonblocking I/O).
 *	extra hint: much like write(2) works.
 *
 */

static inline size_t off_to_size(off_t off_bytes)
{
    if (off_bytes > SSIZE_MAX)
	return SSIZE_MAX;
    return off_bytes;
}

#if defined(__linux__) && !defined(NO_SENDFILE)

# include <sys/sendfile.h>
static ssize_t xsendfile(int out, int in, off_t offset, off_t off_bytes)
{
    size_t bytes = off_to_size(off_bytes);
    return sendfile(out, in, &offset, bytes);
}

#elif defined(__FreeBSD__) && !defined(NO_SENDFILE)

static ssize_t xsendfile(int out, int in, off_t offset, off_t off_bytes)
{
    size_t bytes = off_to_size(off_bytes);
    off_t nbytes = 0;

    if (-1 == sendfile(in, out, offset, bytes, NULL, &nbytes, 0)) {
	/* Why the heck FreeBSD returns an /error/ if it has done a partial
	   write?  With non-blocking I/O this absolutely normal behavoir and
	   no error at all.  Stupid. */
	if (errno == EAGAIN && nbytes > 0)
	    return nbytes;
	return -1;
    }
    return nbytes;
}
#else

/* Note: using sendfile() emulation on this platform */

/* Poor man's sendfile() implementation. Performance sucks, but it works. */
# define BUFSIZE 16384

static ssize_t xsendfile(int out, int in, off_t offset, off_t off_bytes)
{
    char buf[BUFSIZE];
    ssize_t nread;
    ssize_t nsent, nsent_total;
    size_t bytes = off_to_size(off_bytes);

    if (lseek(in, offset, SEEK_SET) == -1) {
	if (debug)
	    perror("lseek");
	return -1;
    }

    nsent = nsent_total = 0;
    for (;bytes > 0;) {
	/* read a block */
	nread = read(in, buf, (bytes < BUFSIZE) ? bytes : BUFSIZE);
	if (-1 == nread) {
	    if (debug)
		perror("read");
	    return nsent_total ? nsent_total : -1;
	}
	if (0 == nread)
	    break;

	/* write it out */
	nsent = write(out, buf, nread);
	if (-1 == nsent)
	    return nsent_total ? nsent_total : -1;

	nsent_total += nsent;
	if (nsent < nread)
	    /* that was a partial write only.  Queue full.  Bailout here,
	       the next write would return EAGAIN anyway... */
	    break;

	bytes -= nread;
    }
    return nsent_total;
}

#endif

/* ---------------------------------------------------------------------- */

#ifdef USE_SSL

static inline int wrap_xsendfile(struct REQUEST *req, off_t off, off_t bytes)
{
    if (with_ssl)
	return ssl_blk_write(req, off, off_to_size(bytes));
    else
	return xsendfile(req->fd, req->bfd, off, bytes);
}

static inline int wrap_write(struct REQUEST *req, void *buf, off_t bytes)
{
    if (with_ssl)
	return ssl_write(req, buf, off_to_size(bytes));
    else
	return write(req->fd, buf, off_to_size(bytes));
}

#else
# define wrap_xsendfile(req,off,bytes)  xsendfile(req->fd,req->bfd,off,bytes)
# define wrap_write(req,buf,bytes)      write(req->fd,buf,bytes);
#endif

/* ---------------------------------------------------------------------- */

static struct HTTP_STATUS {
    int   status;
    char *head;
    char *body;
} http[] = {
    { 200, "200 OK",                       NULL },
    { 206, "206 Partial Content",          NULL },
    { 304, "304 Not Modified",             NULL },
    { 400, "400 Bad Request",              "*PLONK*\n" },
    { 401, "401 Authentication required",  "Authentication required\n" },
    { 403, "403 Forbidden",                "Access denied\n" },
    { 404, "404 Not Found",                "File or directory not found\n" },
    { 408, "408 Request Timeout",          "Request Timeout\n" },
    { 412, "412 Precondition failed.",     "Precondition failed\n" },
    { 500, "500 Internal Server Error",    "Sorry folks\n" },
    { 501, "501 Not Implemented",          "Sorry folks\n" },
    {   0, NULL,                        NULL }
};

/* ---------------------------------------------------------------------- */

#define RESPONSE_START			\
	"HTTP/1.1 %s\r\n"		\
	"Server: %s\r\n"		\
	"Connection: %s\r\n"		\
	"Accept-Ranges: bytes\r\n"
#define BOUNDARY			\
	"XXX_CUT_HERE_%ld_XXX"

static void
mkcors(struct REQUEST *req) {
    if (NULL != req->cors) {
        req->lres += sprintf(req->hres+req->lres,
                     "Access-Control-Allow-Origin: %s\r\n",
                     req->cors);
        if(debug)
        	fprintf(stderr, "%03d: CORS added: CORS=%s\n",
        		req->fd, req->cors);
    }

}
void
mkerror(struct REQUEST *req, int status, int ka)
{
    int i;
    for (i = 0; http[i].status != 0; i++)
	if (http[i].status == status)
	    break;
    req->status = status;
    req->body   = http[i].body;
    req->lbody  = strlen(req->body);
    if (!ka)
	req->keep_alive = 0;
    req->lres = sprintf(req->hres,
			RESPONSE_START
			"Content-Type: text/plain\r\n"
			"Content-Length: %" PRId64 "\r\n",
			http[i].head,server_name,
			req->keep_alive ? "Keep-Alive" : "Close",
			(int64_t)req->lbody);
    if (401 == status)
	req->lres += sprintf(req->hres+req->lres,
			     "WWW-Authenticate: Basic realm=\"webfs\"\r\n");
    mkcors(req);
    req->lres += strftime(req->hres+req->lres,80,
			  "Date: " RFC1123 "\r\n\r\n",
			  gmtime(&now));
    req->state = STATE_WRITE_HEADER;
    if (debug)
	fprintf(stderr,"%03d: error: %d, connection=%s\n",
		req->fd, status, req->keep_alive ? "Keep-Alive" : "Close");
}

void
mkredirect(struct REQUEST *req)
{
    req->status = 302;
    req->body   = req->path;
    req->lbody  = strlen(req->body);
    req->lres = sprintf(req->hres,
			RESPONSE_START
			"Location: http://%s:%d%s\r\n"
			"Content-Type: text/plain\r\n"
			"Content-Length: %" PRId64 "\r\n",
			"302 Redirect",server_name,
			req->keep_alive ? "Keep-Alive" : "Close",
			req->hostname,tcp_port,quote((unsigned char *)req->path,9999),
			(int64_t)req->lbody);
    mkcors(req);
    req->lres += strftime(req->hres+req->lres,80,
			  "Date: " RFC1123 "\r\n\r\n",
			  gmtime(&now));
    req->state = STATE_WRITE_HEADER;
    if (debug)
	fprintf(stderr,"%03d: 302 redirect: %s, connection=%s\n",
		req->fd, req->path, req->keep_alive ? "Keep-Alive" : "Close");
}

static int
mkmulti(struct REQUEST *req, int i)
{
    req->r_hlen[i] = sprintf(req->r_head+i*BR_HEADER,
			     "\r\n--" BOUNDARY "\r\n"
			     "Content-type: %s\r\n"
			     "Content-range: bytes %" PRId64 "-%" PRId64 "/%" PRId64 "\r\n"
			     "\r\n",
			     now, req->mime,
			     (int64_t)req->r_start[i],
			     (int64_t)req->r_end[i]-1,
			     (int64_t)req->bst.st_size);
    if (debug > 1)
	fprintf(stderr,"%03d: send range: %" PRId64 "-%" PRId64 "/%" PRId64 " (%" PRId64 " byte)\n",
		req->fd,
		(int64_t)req->r_start[i],
		(int64_t)req->r_end[i],
		(int64_t)req->bst.st_size,
		(int64_t)(req->r_end[i]-req->r_start[i]));
    return req->r_hlen[i];
}

void
mkheader(struct REQUEST *req, int status)
{
    int    i;
    off_t  len;
    time_t expires;

    for (i = 0; http[i].status != 0; i++)
	if (http[i].status == status)
	    break;
    req->status = status;
    req->lres = sprintf(req->hres,
			RESPONSE_START,
			http[i].head,server_name,
			req->keep_alive ? "Keep-Alive" : "Close");
    if (req->ranges == 0) {
	req->lres += sprintf(req->hres+req->lres,
			     "Content-Type: %s\r\n"
			     "Content-Length: %" PRId64 "\r\n",
			     req->mime,
			     (int64_t)(req->body ? req->lbody : req->bst.st_size));
    } else if (req->ranges == 1) {
	req->lres += sprintf(req->hres+req->lres,
			     "Content-Type: %s\r\n"
			     "Content-Range: bytes %" PRId64 "-%" PRId64 "/%" PRId64 "\r\n"
			     "Content-Length: %" PRId64 "\r\n",
			     req->mime,
			     (int64_t)req->r_start[0],
			     (int64_t)req->r_end[0]-1,
			     (int64_t)req->bst.st_size,
			     (int64_t)(req->r_end[0]-req->r_start[0]));
    } else {
	for (i = 0, len = 0; i < req->ranges; i++) {
	    len += mkmulti(req,i);
	    len += req->r_end[i]-req->r_start[i];
	}
	req->r_hlen[i] = sprintf(req->r_head+i*BR_HEADER,
				 "\r\n--" BOUNDARY "--\r\n",
				 now);
	len += req->r_hlen[i];
	req->lres += sprintf(req->hres+req->lres,
			     "Content-Type: multipart/byteranges;"
			     " boundary=" BOUNDARY "\r\n"
			     "Content-Length: %" PRId64 "\r\n",
			     now, (int64_t)len);
    }
    if (req->mtime[0] != '\0') {
	req->lres += sprintf(req->hres+req->lres,
			     "Last-Modified: %s\r\n",
			     req->mtime);
	if (-1 != lifespan) {
	    expires = req->bst.st_mtime + lifespan;
	    req->lres += strftime(req->hres+req->lres,80,
				  "Expires: " RFC1123 "\r\n",
				  gmtime(&expires));
	}
    }
    mkcors(req);
    req->lres += strftime(req->hres+req->lres,80,
			  "Date: " RFC1123 "\r\n\r\n",
			  gmtime(&now));
    req->state = STATE_WRITE_HEADER;
    if (debug)
	fprintf(stderr,"%03d: %d, connection=%s\n",
		req->fd, status, req->keep_alive ? "Keep-Alive" : "Close");
}

void
mkcgi(struct REQUEST *req, char *status, struct strlist *header)
{
    req->status = atoi(status);
    req->keep_alive = 0;
    req->lres = sprintf(req->hres,
			RESPONSE_START,
			status, server_name,"Close");
    for (; NULL != header; header = header->next)
	req->lres += sprintf(req->hres+req->lres,"%s\r\n",header->line);
    req->lres += strftime(req->hres+req->lres,80,
			  "Date: " RFC1123 "\r\n\r\n",
			  gmtime(&now));
    mkcors(req);
    req->state = STATE_WRITE_HEADER;
}

/* ---------------------------------------------------------------------- */

void write_request(struct REQUEST *req)
{
    int rc;

    for (;;) {
	switch (req->state) {
	case STATE_WRITE_HEADER:
#ifdef TCP_CORK
	    if (0 == req->tcp_cork && !req->head_only) {
		req->tcp_cork = 1;
		if (debug)
		    fprintf(stderr,"%03d: tcp_cork=%d\n",req->fd,req->tcp_cork);
		setsockopt(req->fd,SOL_TCP,TCP_CORK,&req->tcp_cork,sizeof(int));
	    }
#endif
	    rc = wrap_write(req,req->hres + req->written,
			    req->lres - req->written);
	    switch (rc) {
	    case -1:
		if (errno == EAGAIN)
		    return;
		if (errno == EINTR)
		    continue;
		xperror(LOG_INFO,"write",req->peerhost);
		/* fall through */
	    case 0:
		req->state = STATE_CLOSE;
		return;
	    default:
		req->written += rc;
		req->bc += rc;
		if (req->written != req->lres)
		    return;
	    }
	    req->written = 0;
	    if (req->head_only) {
		req->state = STATE_FINISHED;
		return;
	    } else if (req->cgipid) {
		req->state = (req->cgipos != req->cgilen) ?
		    STATE_CGI_BODY_OUT : STATE_CGI_BODY_IN;
	    } else if (req->body) {
		req->state = STATE_WRITE_BODY;
	    } else if (req->ranges == 1) {
		req->state = STATE_WRITE_RANGES;
		req->rh = -1;
		req->rb = 0;
		req->written = req->r_start[0];
	    } else if (req->ranges > 1) {
		req->state = STATE_WRITE_RANGES;
		req->rh = 0;
		req->rb = -1;
	    } else {
		req->state = STATE_WRITE_FILE;
	    }
	    break;
	case STATE_WRITE_BODY:
	    rc = wrap_write(req,req->body + req->written,
			    req->lbody - req->written);
	    switch (rc) {
	    case -1:
		if (errno == EAGAIN)
		    return;
		if (errno == EINTR)
		    continue;
		xperror(LOG_INFO,"write",req->peerhost);
		/* fall through */
	    case 0:
		req->state = STATE_CLOSE;
		return;
	    default:
		req->written += rc;
		req->bc += rc;
		if (req->written != req->lbody)
		    return;
	    }
	    req->state = STATE_FINISHED;
	    return;
	case STATE_WRITE_FILE:
	    rc = wrap_xsendfile(req, req->written,
				req->bst.st_size - req->written);
	    switch (rc) {
	    case -1:
		if (errno == EAGAIN)
		    return;
		if (errno == EINTR)
		    continue;
		xperror(LOG_INFO,"sendfile",req->peerhost);
		/* fall through */
	    case 0:
		req->state = STATE_CLOSE;
		return;
	    default:
		if (debug > 1)
		    fprintf(stderr,"%03d: %" PRId64 "/%" PRId64 " (%d%%)\r",req->fd,
			    (int64_t)req->written,(int64_t)req->bst.st_size,
			    (int)(req->written*100/req->bst.st_size));
		req->written += rc;
		req->bc += rc;
		if (req->written != req->bst.st_size)
		    return;
	    }
	    req->state = STATE_FINISHED;
	    return;
	case STATE_WRITE_RANGES:
	    if (-1 != req->rh) {
		/* write header */
		rc = wrap_write(req,
				req->r_head + req->rh*BR_HEADER + req->written,
				req->r_hlen[req->rh] - req->written);
		switch (rc) {
		case -1:
		    if (errno == EAGAIN)
			return;
		    if (errno == EINTR)
			continue;
		    xperror(LOG_INFO,"write",req->peerhost);
		    /* fall through */
		case 0:
		    req->state = STATE_CLOSE;
		    return;
		default:
		    req->written += rc;
		    req->bc += rc;
		    if (req->written != req->r_hlen[req->rh])
			return;
		}
		if (req->rh == req->ranges) {
		    /* done -- no more ranges */
		    req->state = STATE_FINISHED;
		    return;
		}
		/* prepare for body writeout */
		req->rb      = req->rh;
		req->rh      = -1;
		req->written = req->r_start[req->rb];
	    }
	    if (-1 != req->rb) {
		/* write body */
		rc = wrap_xsendfile(req, req->written,
				    req->r_end[req->rb] - req->written);
		switch (rc) {
		case -1:
		    if (errno == EAGAIN)
			return;
		    if (errno == EINTR)
			continue;
		    xperror(LOG_INFO,"sendfile",req->peerhost);
		    /* fall through */
		case 0:
		    req->state = STATE_CLOSE;
		    return;
		default:
		    req->written += rc;
		    req->bc += rc;
		    if (req->written != req->r_end[req->rb])
			return;
		}
		/* prepare for next subheader writeout */
		req->rh      = req->rb+1;
		req->rb      = -1;
		req->written = 0;
		if (req->ranges == 1) {
		    /* single range only */
		    req->state = STATE_FINISHED;
		    return;
		}
	    }
	    break;
	case STATE_CGI_BODY_IN:
	    rc = read(req->cgipipe, req->cgibuf, MAX_HEADER);
	    switch (rc) {
	    case -1:
		if (errno == EAGAIN)
		    return;
		if (errno == EINTR)
		    continue;
		xperror(LOG_INFO,"cgi read",req->peerhost);
		/* fall through */
	    case 0:
		req->state = STATE_FINISHED;
		return;
	    default:
		if (debug)
		    fprintf(stderr,"%03d: cgi: in %d\n",req->fd,rc);
		req->cgipos = 0;
		req->cgilen = rc;
		break;
	    }
	    req->state = STATE_CGI_BODY_OUT;
	    break;
	case STATE_CGI_BODY_OUT:
    	    rc = wrap_write(req,req->cgibuf + req->cgipos,
			    req->cgilen - req->cgipos);
	    switch (rc) {
	    case -1:
		if (errno == EAGAIN)
		    return;
		if (errno == EINTR)
		    continue;
		xperror(LOG_INFO,"write",req->peerhost);
		/* fall through */
	    case 0:
		req->state = STATE_CLOSE;
		return;
	    default:
		if (debug)
		    fprintf(stderr,"%03d: cgi: out %d\n",req->fd,rc);
		req->cgipos += rc;
		req->bc += rc;
		if (req->cgipos != req->cgilen)
		    return;
	    }
	    req->state = STATE_CGI_BODY_IN;
	    break;
	} /* switch(state) */
    } /* for (;;) */
}

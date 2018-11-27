#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

#include <openssl/err.h>
#include <openssl/evp.h>

#include "httpd.h"

#ifdef USE_THREADS
static pthread_mutex_t lock_ssl = PTHREAD_MUTEX_INITIALIZER;
#endif

int ssl_read(struct REQUEST *req, char *buf, int len)
{
    int rc;

    ERR_clear_error();
    rc = SSL_read(req->ssl_s, buf, len);
    if (rc < 0  &&  SSL_get_error(req->ssl_s, rc) == SSL_ERROR_WANT_READ) {
	errno = EAGAIN;
	return -1;
    }

    if (debug) {
	unsigned long err;
	while (0 != (err = ERR_get_error()))
	    fprintf(stderr, "%03d: ssl read error: %s\n", req->fd,
		    ERR_error_string(err, NULL));
    }

    if (rc < 0) {
	errno = EIO;
	return -1;
    }
    return rc;
}

int ssl_write(struct REQUEST *req, char *buf, int len)
{
    int rc;

    ERR_clear_error();
    rc = SSL_write(req->ssl_s, buf, len);
    if (rc < 0 && SSL_get_error(req->ssl_s, rc) == SSL_ERROR_WANT_WRITE) {
	errno = EAGAIN;
	return -1;
    }

    if (debug) {
	unsigned long err;
	while (0 != (err = ERR_get_error()))
	    fprintf(stderr, "%03d: ssl read error: %s\n", req->fd,
		    ERR_error_string(err, NULL));
    }

    if (rc < 0) {
	errno = EIO;
	return -1;
    }
    return rc;
}

int ssl_blk_write(struct REQUEST *req, off_t offset, size_t len)
{
    int  rc;
    char buf[4096];

    if (lseek(req->bfd, offset, SEEK_SET) == -1) {
        if (debug) perror("lseek");
        return -1;
    }

    if (len > sizeof(buf))
	len = sizeof(buf);
    rc = read(req->bfd, buf, len);
    if (rc <= 0) {
	/* shouldn't happen ... */
	req->state = STATE_CLOSE;
	return rc;
    }
    return ssl_write(req, buf, rc);
}

static int password_cb(char *buf, int num, int rwflag, void *userdata)
{
    if (NULL == password)
	return 0;
    if (num < strlen(password)+1)
	return 0;
    
    strcpy(buf,password);
    return(strlen(buf));
}

void init_ssl(void)
{
    int rc;

    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    SSL_library_init();
    ctx = SSL_CTX_new(SSLv23_server_method());
    if (NULL == ctx) {
        fprintf(stderr, "SSL init error [%s]",strerror(errno));
	exit (1);
    }

    rc = SSL_CTX_use_certificate_chain_file(ctx, certificate);
    switch (rc) {
    case 1:
	if (debug)
	    fprintf(stderr, "SSL certificate load ok\n");
	break;
    default:
	fprintf(stderr, "SSL cert load error [%s]\n",
		ERR_error_string(ERR_get_error(), NULL));
	break;
    }

    SSL_CTX_set_default_passwd_cb(ctx, password_cb);
    SSL_CTX_use_PrivateKey_file(ctx, certificate, SSL_FILETYPE_PEM);
    switch (rc) {
    case 1:
	if (debug)
	    fprintf(stderr, "SSL private key load ok\n");
	break;
    default:
	fprintf(stderr, "SSL privkey load error [%s]\n",
		ERR_error_string(ERR_get_error(), NULL));
	break;
    }

    SSL_CTX_set_options(ctx, SSL_OP_ALL | SSL_OP_NO_SSLv2);
}

void open_ssl_session(struct REQUEST *req)
{
    DO_LOCK(lock_ssl);
    req->ssl_s = SSL_new(ctx);
    if (req->ssl_s == NULL) {
	if (debug)
	    fprintf(stderr,"%03d: SSL session init error [%s]\n",
		    req->fd, strerror(errno));
	/* FIXME: how to handle that one? */
    }
    SSL_set_fd(req->ssl_s, req->fd);
    SSL_set_accept_state(req->ssl_s);
    SSL_set_read_ahead(req->ssl_s, 0); /* to prevent unwanted buffering in ssl layer */
    DO_UNLOCK(lock_ssl);
}

#include <sys/stat.h>
#ifdef USE_THREADS
# include <pthread.h>
#endif

#define STATE_READ_HEADER   1
#define STATE_PARSE_HEADER  2
#define STATE_WRITE_HEADER  3
#define STATE_WRITE_BODY    4
#define STATE_WRITE_FILE    5
#define STATE_WRITE_RANGES  6
#define STATE_FINISHED      7

#define STATE_KEEPALIVE     8
#define STATE_CLOSE         9

#define STATE_CGI_HEADER   10
#define STATE_CGI_BODY_IN  11
#define STATE_CGI_BODY_OUT 12

#ifdef USE_SSL
# include <openssl/ssl.h>
#endif

#define MAX_HEADER 4096
#define MAX_PATH   2048
#define MAX_HOST     64
#define MAX_MISC     16
#define BR_HEADER   512

#define S1(str) #str
#define S(str)  S1(str)

#define RFC1123	"%a, %d %b %Y %H:%M:%S GMT"

struct DIRCACHE {
    char             path[1024];
    char             mtime[40];
    time_t           add;
    char             *html;
    int              length;

#ifdef USE_THREADS
    pthread_mutex_t  lock_refcount;
    pthread_mutex_t  lock_reading;
    pthread_cond_t   wait_reading;
#endif
    int              refcount;
    int              reading;

    struct DIRCACHE  *next;
};

struct REQUEST {
    int	        fd;		     /* socket handle */
    int	        state;	             /* what to to ??? */
    time_t      ping;                /* last read/write (for timeouts) */
    int         keep_alive;
    int		tcp_cork;

    struct sockaddr_storage peer;         /* client (log) */
    char        peerhost[MAX_HOST+1];
    char        peerserv[MAX_MISC+1];
    
    /* request */
    char	hreq[MAX_HEADER+1];   /* request header */
    int 	lreq;		      /* request length */
    int         hdata;                /* data in hreq */
    char        type[MAX_MISC+1];     /* req type */
    char        hostname[MAX_HOST+1]; /* hostname */
    char	uri[MAX_PATH+1];      /* req uri */
    char	path[MAX_PATH+1];     /* file path */
    char	query[MAX_PATH+1];    /* query string */
    int         major,minor;          /* http version */
    char        auth[64];
    struct strlist *header;
    char        *if_modified;
    char        *if_unmodified;
    char        *if_range;
    char        *range_hdr;
    int         ranges;
    off_t       *r_start;
    off_t       *r_end;
    char        *r_head;
    int         *r_hlen;
    char        *cors;
    
    /* response */
    int         status;              /* status code (log) */
    int         bc;                  /* byte counter (log) */
    char	hres[MAX_HEADER+1];  /* response header */
    int	        lres;		     /* header length */
    char        *mime;               /* mime type */
    char	*body;
    off_t       lbody;
    int         bfd;                 /* file descriptor */
    struct stat bst;                 /* file info */
    char        mtime[40];           /* RFC 1123 */
    off_t       written;
    int         head_only;
    int         rh,rb;
    struct DIRCACHE *dir;

    /* CGI */
    int         cgipid;
    int         cgipipe;
    char        cgibuf[MAX_HEADER+1];
    int         cgilen,cgipos;

#ifdef USE_SSL
    /* SSL */
    SSL		*ssl_s;
#endif

    /* linked list */
    struct REQUEST *next;
};

/* --- string lists --------------------------------------------- */

struct strlist {
    struct strlist *next;
    char *line;
    int free_the_mallocs;
};

/* add element (list head) */
static void inline
list_add(struct strlist **list, char *line, int free_the_mallocs)
{
    struct strlist *elem = malloc(sizeof(struct strlist));
    memset(elem,0,sizeof(struct strlist));
    elem->next = *list;
    elem->line = line;
    elem->free_the_mallocs = free_the_mallocs;
    *list = elem;
}

/* free whole list */
static void inline
list_free(struct strlist **list)
{
    struct strlist *elem,*next;

    for (elem = *list; NULL != elem; elem = next) {
	next = elem->next;
	if (elem->free_the_mallocs)
	    free(elem->line);
	free(elem);
    }
    *list = NULL;
}

/* --- main.c --------------------------------------------------- */

extern int    debug;
extern int    tcp_port;
extern int    max_dircache;
extern int    virtualhosts;
extern int    canonicalhost;
extern int    do_chroot;
extern char   *server_name;
extern char   *indexhtml;
extern char   *cgipath;
extern char   *doc_root;
extern char   server_host[];
extern char   *userpass;
extern char   *userdir;
extern int    lifespan;
extern int    no_listing;
extern time_t now;
extern int     have_tty;

#ifdef USE_SSL
extern int      with_ssl;
extern SSL_CTX  *ctx;
extern BIO	*sbio, *ssl_bio;
extern char     *certificate;
extern char     *password;
#endif

void xperror(int loglevel, char *txt, char *peerhost);
void xerror(int loglevel, char *txt, char *peerhost);

static void inline close_on_exec(int fd)
{
    if (cgipath)
	fcntl(fd,F_SETFD,FD_CLOEXEC);
}

/* --- ssl.c ---------------------------------------------------- */

#ifdef USE_SSL
extern int ssl_read(struct REQUEST *req, char *buf, int len);
extern int ssl_write(struct REQUEST *req, char *buf, int len);
extern int ssl_blk_write(struct REQUEST *req, off_t offset, size_t len);
extern void init_ssl(void);
extern void open_ssl_session(struct REQUEST *req);
#endif

/* --- request.c ------------------------------------------------ */

void read_request(struct REQUEST *req, int pipelined);
void parse_request(struct REQUEST *req);

/* --- response.c ----------------------------------------------- */

extern char *h200,*h206,*h302,*h304;

extern char *h403,*b403;
extern char *h404,*b404;
extern char *h500,*b500;
extern char *h501,*b501;

void mkerror(struct REQUEST *req, int status, int ka);
void mkredirect(struct REQUEST *req);
void mkheader(struct REQUEST *req, int status);
void mkcgi(struct REQUEST *req, char *status, struct strlist *header);
void write_request(struct REQUEST *req);

/* --- ls.c ----------------------------------------------------- */

void init_quote(void);
char*  quote(unsigned char *path, int maxlength);
struct DIRCACHE *get_dir(struct REQUEST *req, char *filename);
void free_dir(struct DIRCACHE *dir);

/* --- mime.c --------------------------------------------------- */

char* get_mime(char *file);
void  init_mime(char *file, char *def);

/* --- cgi.c ---------------------------------------------------- */

void cgi_request(struct REQUEST *req);
void cgi_read_header(struct REQUEST *req);

/* -------------------------------------------------------------- */

#ifdef USE_THREADS
# define INIT_LOCK(mutex)	pthread_mutex_init(&mutex,NULL)
# define FREE_LOCK(mutex)	pthread_mutex_destroy(&mutex)
# define DO_LOCK(mutex)		pthread_mutex_lock(&mutex)
# define DO_UNLOCK(mutex)	pthread_mutex_unlock(&mutex)
# define INIT_COND(cond)	pthread_cond_init(&cond,NULL)
# define FREE_COND(cond)	pthread_cond_destroy(&cond)
# define BCAST_COND(cond)	pthread_cond_broadcast(&cond);
# define WAIT_COND(cond,mutex)	pthread_cond_wait(&cond,&mutex);
#else
# define INIT_LOCK(mutex)	/* nothing */
# define FREE_LOCK(mutex)	/* nothing */
# define DO_LOCK(mutex)		/* nothing */
# define DO_UNLOCK(mutex)	/* nothing */
# define INIT_COND(cond)	/* nothing */
# define FREE_COND(cond)	/* nothing */
# define BCAST_COND(cond)	/* nothing */
# define WAIT_COND(cond,mutex)	/* nothing */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <inttypes.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "httpd.h"

#define LS_ALLOC_SIZE (4 * 4096)
#define HOMEPAGE "http://bytesex.org/webfs.html"

#ifdef USE_THREADS
static pthread_mutex_t lock_dircache = PTHREAD_MUTEX_INITIALIZER;
#endif

/* --------------------------------------------------------- */

#define CACHE_SIZE 32

static char*
xgetpwuid(uid_t uid)
{
    static char           *cache[CACHE_SIZE];
    static uid_t          uids[CACHE_SIZE];
    static unsigned int   used,next;

    struct passwd *pw;
    int i;

    if (do_chroot)
	return NULL; /* would'nt work anyway .. */

    for (i = 0; i < used; i++) {
	if (uids[i] == uid)
	    return cache[i];
    }

    /* 404 */
    pw = getpwuid(uid);
    if (NULL != cache[next]) {
	free(cache[next]);
	cache[next] = NULL;
    }
    if (NULL != pw)
	cache[next] = strdup(pw->pw_name);
    uids[next] = uid;
    if (debug)
	fprintf(stderr,"uid: %3d  n=%2d, name=%s\n",
		(int)uid, next, cache[next] ? cache[next] : "?");

    next++;
    if (CACHE_SIZE == next) next = 0;
    if (used < CACHE_SIZE) used++;
    
    return pw ? pw->pw_name : NULL;
}

static char*
xgetgrgid(gid_t gid)
{
    static char           *cache[CACHE_SIZE];
    static gid_t          gids[CACHE_SIZE];
    static unsigned int   used,next;

    struct group *gr;
    int i;

    if (do_chroot)
	return NULL; /* would'nt work anyway .. */

    for (i = 0; i < used; i++) {
	if (gids[i] == gid)
	    return cache[i];
    }

    /* 404 */
    gr = getgrgid(gid);
    if (NULL != cache[next]) {
	free(cache[next]);
	cache[next] = NULL;
    }
    if (NULL != gr)
	cache[next] = strdup(gr->gr_name);
    gids[next] = gid;
    if (debug)
	fprintf(stderr,"gid: %3d  n=%2d, name=%s\n",
		(int)gid,next,cache[next] ? cache[next] : "?");

    next++;
    if (CACHE_SIZE == next) next = 0;
    if (used < CACHE_SIZE) used++;
    
    return gr ? gr->gr_name : NULL;
}

/* --------------------------------------------------------- */

struct myfile {
    int           r;
    struct stat   s;
    char          n[1];
};

static int
compare_files(const void *a, const void *b)
{
    const struct myfile *aa = *(struct myfile**)a;
    const struct myfile *bb = *(struct myfile**)b;

    if (S_ISDIR(aa->s.st_mode) !=  S_ISDIR(bb->s.st_mode))
	return S_ISDIR(aa->s.st_mode) ? -1 : 1;
    return strcmp(aa->n,bb->n);
}

static char do_quote[256];

void
init_quote(void)
{
    int i;

    for (i = 0; i < 256; i++)
	do_quote[i] = (isalnum(i) || ispunct(i)) ? 0 : 1;
    do_quote['+'] = 1;
    do_quote['#'] = 1;
    do_quote['%'] = 1;
    do_quote['"'] = 1;
    do_quote['?'] = 1;
}

char*
quote(unsigned char *path, int maxlength)
{
    static unsigned char buf[2048]; /* FIXME: threads break this... */
    int i,j,n=strlen(path);

    if (n > maxlength)
	n = maxlength;

    for (i=0, j=0; i<n && j<sizeof(buf)-4; i++, j++) {
	if (!do_quote[path[i]]) {
	    buf[j] = path[i];
	    continue;
	}
	sprintf(buf+j,"%%%02x",path[i]);
	j += 2;
    }
    buf[j] = 0;
    return buf;
}

#if !defined(__FreeBSD__) && !defined(__OpenBSD__)
static void strmode(mode_t mode, char *dest)
{
    static const char *rwx[] = {
	"---","--x","-w-","-wx","r--","r-x","rw-","rwx" };

    /* file type */
    switch (mode & S_IFMT) {
    case S_IFIFO:  dest[0] = 'p'; break;
    case S_IFCHR:  dest[0] = 'c'; break;
    case S_IFDIR:  dest[0] = 'd'; break;
    case S_IFBLK:  dest[0] = 'b'; break;
    case S_IFREG:  dest[0] = '-'; break;
    case S_IFLNK:  dest[0] = 'l'; break;
    case S_IFSOCK: dest[0] = '='; break;
    default:       dest[0] = '?'; break;
    }
    
    /* access rights */
    sprintf(dest+1,"%s%s%s",
	    rwx[(mode >> 6) & 0x7],
	    rwx[(mode >> 3) & 0x7],
	    rwx[mode        & 0x7]);
}
#endif

static char* 
ls(time_t now, char *hostname, char *filename, char *path, int *length)
{
    DIR            *dir;
    struct dirent  *file;
    struct myfile  **files = NULL;
    struct myfile  **re1;
    char           *h1,*h2,*re2,*buf = NULL;
    int            count,len,size,i,uid,gid;
    char           line[1024];
    char           *pw = NULL, *gr = NULL;

    if (debug)
	fprintf(stderr,"dir: reading %s\n",filename);
    if (NULL == (dir = opendir(filename)))
	return NULL;

    /* read dir */
    uid = getuid();
    gid = getgid();
    for (count = 0;; count++) {
	if (NULL == (file = readdir(dir)))
	    break;
	if (0 == strcmp(file->d_name,".")) {
	    /* skip the the "." directory */
	    count--;
	    continue;
	}
	if (0 == strcmp(path,"/") && 0 == strcmp(file->d_name,"..")) {
	    /* skip the ".." directory in root dir */
	    count--;
	    continue;
	}

	if (0 == (count % 64)) {
	    re1 = realloc(files,(count+64)*sizeof(struct myfile*));
	    if (NULL == re1)
		goto oom;
	    files = re1;
	}

	files[count] = malloc(strlen(file->d_name)+sizeof(struct myfile));
	if (NULL == files[count])
	    goto oom;
	strcpy(files[count]->n,file->d_name);
	sprintf(line,"%s/%s",filename,file->d_name);
	if (-1 == stat(line,&files[count]->s)) {
	    free(files[count]);
	    count--;
	    continue;
	}
	
	files[count]->r = 0;
	if (S_ISDIR(files[count]->s.st_mode) ||
	    S_ISREG(files[count]->s.st_mode)) {
	    if (files[count]->s.st_uid == uid &&
		files[count]->s.st_mode & 0400)
		files[count]->r = 1;
	    else if (files[count]->s.st_uid == gid &&
		     files[count]->s.st_mode & 0040)
		files[count]->r = 1; /* FIXME: check additional groups */
	    else if (files[count]->s.st_mode & 0004)
		files[count]->r = 1;
	}
    }
    closedir(dir);
    
    /* sort */
    if (count)
	qsort(files,count,sizeof(struct myfile*),compare_files);

    /* output */
    size = LS_ALLOC_SIZE;
    buf  = malloc(size);
    if (NULL == buf)
	goto oom;
    len  = 0;

    len += sprintf(buf+len,
		   "<head><title>%s:%d%s</title></head>\n"
		   "<body bgcolor=white text=black link=darkblue vlink=firebrick alink=red>\n"
		   "<h1>listing: \n",
		   hostname,tcp_port,path);

    h1 = path, h2 = path+1;
    for (;;) {
	if (len > size)
	    abort();
	if (len+(LS_ALLOC_SIZE>>2) > size) {
	    size += LS_ALLOC_SIZE;
	    re2 = realloc(buf,size);
	    if (NULL == re2)
		goto oom;
	    buf = re2;
	}
	len += sprintf(buf+len,"<a href=\"%s\">%*.*s</a>",
		       quote(path,h2-path),
		       (int)(h2-h1),
		       (int)(h2-h1),
		       h1);
	h1 = h2;
	h2 = strchr(h2,'/');
	if (NULL == h2)
	    break;
	h2++;
    }

    len += sprintf(buf+len,
		   "</h1><hr noshade size=1><pre>\n"
		   "<b>access      user      group     date             "
		   "size  name</b>\n\n");

    for (i = 0; i < count; i++) {
	if (len > size)
	    abort();
	if (len+(LS_ALLOC_SIZE>>2) > size) {
	    size += LS_ALLOC_SIZE;
	    re2 = realloc(buf,size);
	    if (NULL == re2)
		goto oom;
	    buf = re2;
	}

	/* mode */
	strmode(files[i]->s.st_mode, buf+len);
	len += 10;
	buf[len++] = ' ';
	buf[len++] = ' ';
	
	/* user */
	pw = xgetpwuid(files[i]->s.st_uid);
	if (NULL != pw)
	    len += sprintf(buf+len,"%-8.8s  ",pw);
	else
	    len += sprintf(buf+len,"%8d  ",(int)files[i]->s.st_uid);
		
	/* group */
	gr = xgetgrgid(files[i]->s.st_gid);
	if (NULL != gr)
	    len += sprintf(buf+len,"%-8.8s  ",gr);
	else
	    len += sprintf(buf+len,"%8d  ",(int)files[i]->s.st_gid);
	
	/* mtime */
	if (now - files[i]->s.st_mtime > 60*60*24*30*6)
	    len += strftime(buf+len,255,"%b %d  %Y  ",
			    gmtime(&files[i]->s.st_mtime));
	else
	    len += strftime(buf+len,255,"%b %d %H:%M  ",
			    gmtime(&files[i]->s.st_mtime));
	
	/* size */
	if (S_ISDIR(files[i]->s.st_mode)) {
	    len += sprintf(buf+len,"  &lt;DIR&gt;  ");
	} else if (!S_ISREG(files[i]->s.st_mode)) {
	    len += sprintf(buf+len,"     --  ");
	} else if (files[i]->s.st_size < 1024*9) {
	    len += sprintf(buf+len,"%4d  B  ",
			   (int)files[i]->s.st_size);
	} else if (files[i]->s.st_size < 1024*1024*9) {
	    len += sprintf(buf+len,"%4d kB  ",
			   (int)(files[i]->s.st_size>>10));
	} else if ((int64_t)(files[i]->s.st_size) < (int64_t)1024*1024*1024*9) {
	    len += sprintf(buf+len,"%4d MB  ",
			   (int)(files[i]->s.st_size>>20));
	} else if ((int64_t)(files[i]->s.st_size) < (int64_t)1024*1024*1024*1024*9) {
	    len += sprintf(buf+len,"%4d GB  ",
			   (int)(files[i]->s.st_size>>30));
	} else {
	    len += sprintf(buf+len,"%4d TB  ",
			   (int)(files[i]->s.st_size>>40));
	}
	
	/* filename */
	if (files[i]->r) {
	    len += sprintf(buf+len,"<a href=\"%s%s\">%s</a>\n",
			   quote(files[i]->n,9999),
			   S_ISDIR(files[i]->s.st_mode) ? "/" : "",
			   files[i]->n);
	} else {
	    len += sprintf(buf+len,"%s\n",files[i]->n);
	}
    }
    strftime(line,32,"%d/%b/%Y %H:%M:%S GMT",gmtime(&now));
    len += sprintf(buf+len,
		   "</pre><hr noshade size=1>\n"
		   "<small><a href=\"%s\">%s</a> &nbsp; %s</small>\n"
		   "</body>\n",
		   HOMEPAGE,server_name,line);
    for (i = 0; i < count; i++)
	free(files[i]);
    if (count)
	free(files);

    /* return results */
    *length = len;
    return buf;

 oom:
    fprintf(stderr,"oom\n");
    if (files) {
	for (i = 0; i < count; i++)
	    if (files[i])
		free(files[i]);
	free(files);
    }
    if (buf)
	free(buf);
    return NULL;
}

/* --------------------------------------------------------- */

#define MAX_CACHE_AGE   3600   /* seconds */

struct DIRCACHE *dirs = NULL;

void free_dir(struct DIRCACHE *dir)
{
    DO_LOCK(dir->lock_refcount);
    dir->refcount--;
    if (dir->refcount > 0) {
	DO_UNLOCK(dir->lock_refcount);
	return;
    }
    DO_UNLOCK(dir->lock_refcount);
    if (debug)
	fprintf(stderr,"dir: delete %s\n",dir->path);
    FREE_LOCK(dir->lock_refcount);
    FREE_LOCK(dir->lock_reading);
    FREE_COND(dir->wait_reading);
    if (NULL != dir->html)
	free(dir->html);
    free(dir);
}

struct DIRCACHE*
get_dir(struct REQUEST *req, char *filename)
{
    struct DIRCACHE  *this,*prev;
    int              i;

    DO_LOCK(lock_dircache);
    for (prev = NULL, this = dirs, i=0; this != NULL;
	 prev = this, this = this->next, i++) {
	if (0 == strcmp(filename,this->path)) {
	    /* remove from list */
	    if (NULL == prev)
		dirs = this->next;
	    else
		prev->next = this->next;
	    if (debug)
		fprintf(stderr,"dir: found %s\n",this->path);
	    break;
	}
	if (i > max_dircache) {
	    /* reached cache size limit -> free last element */
#if 0
	    if (this->next != NULL) {
		fprintf(stderr,"panic: this should'nt happen (%s:%d)\n",
			__FILE__, __LINE__);
		exit(1);
	    }
#endif
	    free_dir(this);
	    this = NULL;
	    prev->next = NULL;
	    break;
	}
    }
    if (this) {
	/* check mtime and cache entry age */
	if (now - this->add > MAX_CACHE_AGE ||
	    0 != strcmp(this->mtime, req->mtime)) {
	    free_dir(this);
	    this = NULL;
	}
    }
    if (!this) {
	/* add a new cache entry to the list */
	this = malloc(sizeof(struct DIRCACHE));
	this->refcount = 2;
	this->reading = 1;
	INIT_LOCK(this->lock_refcount);
	INIT_LOCK(this->lock_reading);
	INIT_COND(this->wait_reading);
	this->next = dirs;
	dirs = this;
	DO_UNLOCK(lock_dircache);

	strcpy(this->path,  filename);
	strcpy(this->mtime, req->mtime);
	this->add   = now;
	this->html  = ls(now,req->hostname,filename,req->path,&(this->length));

	DO_LOCK(this->lock_reading);
	this->reading = 0;
	BCAST_COND(this->wait_reading);
	DO_UNLOCK(this->lock_reading);
    } else {
	/* add back to the list */
	this->next = dirs;
	dirs = this;
	this->refcount++;
	DO_UNLOCK(lock_dircache);

	DO_LOCK(this->lock_reading);
	if (this->reading)
	    WAIT_COND(this->wait_reading,this->lock_reading);
	DO_UNLOCK(this->lock_reading);
    }

    req->body  = this->html;
    req->lbody = this->length;
    return this;
}

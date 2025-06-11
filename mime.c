#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "httpd.h"

/* ----------------------------------------------------------------- */

struct MIME {
    char  ext[8];
    char  type[64];
};

static char         *mime_default;
static struct MIME  *mime_types;
static int           mime_count;

/* ----------------------------------------------------------------- */

static void
add_mime(char *ext, char *type)
{
    if (0 == (mime_count % 64))
	mime_types = realloc(mime_types,(mime_count+64)*sizeof(struct MIME));
    strcpy(mime_types[mime_count].ext, ext);
    strcpy(mime_types[mime_count].type,type);
    mime_count++;
}

char*
get_mime(char *file)
{
    int i;
    char *ext;

    ext = strrchr(file,'.');
    if (NULL == ext)
	return mime_default;
    ext++;
    for (i = 0; i < mime_count; i++) {
	if (0 == strcasecmp(ext,mime_types[i].ext))
	    return mime_types[i].type;
    }
    return mime_default;
}

void
init_mime(char *file,char *def)
{
    FILE *fp;
    char line[128], type[64], ext[8];
    int  len,off;

    mime_default = strdup(def);
    if (NULL == (fp = fopen(file,"r"))) {
	/* Add basic mime types as fallback when file doesn't exist */
	add_mime("html", "text/html");
	add_mime("htm",  "text/html");
	add_mime("css",  "text/css");
	add_mime("js",   "application/javascript");
	add_mime("json", "application/json");
	add_mime("txt",  "text/plain");
	add_mime("xml",  "text/xml");
	add_mime("jpg",  "image/jpeg");
	add_mime("jpeg", "image/jpeg");
	add_mime("png",  "image/png");
	add_mime("gif",  "image/gif");
	add_mime("ico",  "image/x-icon");
	add_mime("pdf",  "application/pdf");
	add_mime("zip",  "application/zip");
	add_mime("gz",   "application/gzip");
	add_mime("mp3",  "audio/mpeg");
	add_mime("mp4",  "video/mp4");
	add_mime("webm", "video/webm");
	add_mime("svg",  "image/svg+xml");
	add_mime("woff", "font/woff");
	add_mime("woff2","font/woff2");
	if (debug)
	    fprintf(stderr,"warning: %s not found, using built-in mime types\n",file);
	return;
    }
    while (NULL != fgets(line,127,fp)) {
	if (line[0] == '#')
	    continue;
	if (1 != sscanf(line,"%63s%n",type,&len))
	    continue;
	off = len;
	for (;;) {
	    if (1 != sscanf(line+off,"%7s%n",ext,&len))
		break;
	    off += len;
	    add_mime(ext,type);
	}
    }
    fclose(fp);
}

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
	fprintf(stderr,"open %s: %s\n",file,strerror(errno));
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

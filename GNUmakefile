# config
-include Make.config
include mk/Variables.mk

TARGET	:= webfsd
OBJS	:= webfsd.o request.o response.o ls.o mime.o cgi.o

# Set mime.types path based on OS
ifeq ($(SYSTEM),darwin)
mimefile := "/usr/share/cups/mime/mime.types"
else
mimefile := "/etc/mime.types"
endif
CFLAGS	+= -DMIMEFILE=\"$(mimefile)\"
CFLAGS	+= -DWEBFS_VERSION=\"$(VERSION)\"
CFLAGS	+= -D_GNU_SOURCE
CFLAGS	+= -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64

# default target
all: build


############################################################################

include mk/Autoconf.mk

ifeq ($(filter config,$(MAKECMDGOALS)),config)
define make-config
LIB          := $(LIB)
SYSTEM       := $(call ac_uname)
USE_SENDFILE := yes
USE_THREADS  := no
USE_SSL      := $(call ac_header,openssl/ssl.h)
USE_DIET     := $(call ac_binary,diet)
endef
endif

# sendfile yes/no
ifneq ($(USE_SENDFILE),yes)
CFLAGS	+= -DNO_SENDFILE
endif

# threads yes/no
ifeq ($(USE_THREADS)-$(SYSTEM),yes-linux)
CFLAGS	+= -DUSE_THREADS=1 -D_REENTRANT
LDLIBS	+= -lpthread
endif
ifeq ($(USE_THREADS)-$(SYSTEM),yes-freebsd)
CFLAGS	+= -DUSE_THREADS=1 -D_REENTRANT -pthread
endif
ifeq ($(USE_THREADS)-$(SYSTEM),yes-darwin)
CFLAGS	+= -DUSE_THREADS=1 -D_REENTRANT -pthread
endif


# OpenSSL yes/no
ifeq ($(USE_SSL),yes)
CFLAGS	+= -DUSE_SSL=1
OBJS	+= ssl.o
LDLIBS	+= -lssl -lcrypto
endif

# dietlibc yes/no
ifeq ($(USE_DIET),yes)
CC	:= diet $(CC)
endif

# solaris tweaks
ifeq ($(SYSTEM),sunos)
LDFLAGS += -L/usr/local/ssl/lib
LDLIBS  += -lresolv -lsocket -lnsl
endif


#################################################################
# rules

build: $(TARGET)

$(TARGET): $(OBJS)

install: $(TARGET)
	$(INSTALL_DIR) $(bindir)
	$(INSTALL_BINARY) $(TARGET) $(bindir)
	$(INSTALL_DIR) $(mandir)/man1
	$(INSTALL_DATA) webfsd.man $(mandir)/man1/webfsd.1

clean:
	rm -f *~ debian/*~ *.o $(depfiles)

realclean distclean: clean
	rm -f $(TARGET) Make.config

include mk/Compile.mk
include mk/Maintainer.mk
-include mk/*.dep

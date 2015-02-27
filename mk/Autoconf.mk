#
# simple autoconf system for GNU make
#
# (c) 2002-2004 Gerd Knorr <kraxel@bytesex.org>
#
# credits for creating this one go to the autotools people because
# they managed it to annoy lots of developers and users (including
# me) with version incompatibilities.
#
# This file is public domain.  No warranty.  If it breaks you keep
# both pieces.
#
########################################################################

# verbose yes/no
verbose		?= no

# some stuff used by the tests
ifneq ($(verbose),no)
  # verbose (for debug)
  ac_init	= echo "checking $(1) ... " >&2; rc=no
  ac_b_cmd	= echo "run: $(1)" >&2; $(1) >/dev/null && rc=yes
  ac_s_cmd	= echo "run: $(1)" >&2; rc=`$(1)`
  ac_fini	= echo "... result is $${rc}" >&2; echo >&2; echo "$${rc}"
else
  # normal
  ac_init	= echo -n "checking $(1) ... " >&2; rc=no
  ac_b_cmd	= $(1) >/dev/null 2>&1 && rc=yes
  ac_s_cmd	= rc=`$(1) 2>/dev/null`
  ac_fini	= echo "$${rc}" >&2; echo "$${rc}"
endif

# some helpers to build cflags and related variables
ac_def_cflags_1 = $(if $(filter yes,$($(1))),-D$(1))
ac_lib_cflags	= $(foreach lib,$(1),$(call ac_def_cflags_1,HAVE_LIB$(lib)))
ac_inc_cflags	= $(foreach inc,$(1),$(call ac_def_cflags_1,HAVE_$(inc)))
ac_lib_mkvar_1	= $(if $(filter yes,$(HAVE_LIB$(1))),$($(1)_$(2)))
ac_lib_mkvar	= $(foreach lib,$(1),$(call ac_lib_mkvar_1,$(lib),$(2)))


########################################################################
# the tests ...

# get uname
ac_uname = $(shell \
	$(call ac_init,for system);\
	$(call ac_s_cmd,uname -s | tr 'A-Z' 'a-z');\
	$(call ac_fini))

# check for some header file
# args: header file
ac_header = $(shell \
	$(call ac_init,for $(1));\
	$(call ac_b_cmd,echo '\#include <$(1)>' |\
		$(CC) $(CFLAGS) -E -);\
	$(call ac_fini))

# check for some function
# args: function [, additional libs ]
ac_func = $(shell \
	$(call ac_init,for $(1));\
	echo 'void $(1)(void); int main(void) {$(1)();return 0;}' \
		> __actest.c;\
	$(call ac_b_cmd,$(CC) $(CFLAGS) $(LDFLAGS) -o \
		__actest __actest.c $(2));\
	rm -f __actest __actest.c;\
	$(call ac_fini))

# check for some library
# args: function, library [, additional libs ]
ac_lib = $(shell \
	$(call ac_init,for $(1) in $(2));\
	echo 'void $(1)(void); int main(void) {$(1)();return 0;}' \
		> __actest.c;\
	$(call ac_b_cmd,$(CC) $(CFLAGS) $(LDFLAGS) -o \
		__actest __actest.c -l$(2) $(3));\
	rm -f __actest __actest.c;\
	$(call ac_fini))

# check if some compiler flag works
# args: compiler flag
ac_cflag = $(shell \
	$(call ac_init,if $(CC) supports $(1));\
	echo 'int main() {return 0;}' > __actest.c;\
	$(call ac_b_cmd,$(CC) $(CFLAGS) $(1) $(LDFLAGS) -o \
		__actest __actest.c);\
	rm -f __actest __actest.c;\
	$(call ac_fini))

# check for some binary
# args: binary name
ac_binary = $(shell \
	$(call ac_init,for $(1));\
	$(call ac_s_cmd,which $(1));\
	bin="$$rc";rc="no";\
	$(call ac_b_cmd,test -x "$$$$bin");\
	$(call ac_fini))

# check if lib64 is used
ac_lib64 = $(shell \
	$(call ac_init,for libdir name);\
	$(call ac_s_cmd,$(CC) -print-search-dirs | grep -q lib64 &&\
		echo "lib64" || echo "lib");\
	$(call ac_fini))

# check for x11 ressource dir prefix
ac_resdir = $(shell \
	$(call ac_init,for X11 app-defaults prefix);\
	$(call ac_s_cmd, test -d /etc/X11/app-defaults &&\
		echo "/etc/X11" || echo "/usr/X11R6/lib/X11");\
	$(call ac_fini))


########################################################################
# build Make.config

define newline


endef
make-config-q	= $(subst $(newline),\n,$(make-config))

ifeq ($(filter config,$(MAKECMDGOALS)),config)
.PHONY: Make.config
  LIB := $(call ac_lib64)
else
  LIB ?= $(call ac_lib64)
  LIB := $(LIB)
endif
.PHONY: config
config: Make.config
	@true

Make.config: $(srcdir)/GNUmakefile
	@echo -e "$(make-config-q)" > $@
	@echo
	@echo "Make.config written, edit if needed"
	@echo

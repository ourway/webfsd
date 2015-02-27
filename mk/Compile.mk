#
# some rules to compile stuff ...
#
# (c) 2002-2004 Gerd Knorr <kraxel@bytesex.org>
#
# main features:
#  * autodependencies via "cpp -MD"
#  * fancy, non-verbose output
#
# This file is public domain.  No warranty.  If it breaks you keep
# both pieces.
#
########################################################################

# verbose yes/no
verbose		?= no

# dependency files
tmpdep		= mk/$(subst /,_,$*).tmp
depfile		= mk/$(subst /,_,$*).dep
depfiles	= mk/*.dep

compile_c	= $(CC) $(CFLAGS) -Wp,-MD,$(tmpdep) -c -o $@ $<
compile_cc	= $(CXX) $(CXXFLAGS) -Wp,-MD,$(tmpdep) -c -o $@ $<
fixup_deps	= sed -e "s|.*\.o:|$@:|" < $(tmpdep) > $(depfile) && rm -f $(tmpdep)
cc_makedirs	= mkdir -p $(dir $@) $(dir $(depfile))

link_app	= $(CC) $(LDFLAGS) -o $@  $^ $(LDLIBS)
link_so		= $(CC) $(LDFLAGS) -shared -Wl,-soname,$(@F) -o $@ $^ $(LDLIBS)
ar_lib		= rm -f $@ && ar -r $@ $^ && ranlib $@

moc_h		= $(MOC) $< -o $@
msgfmt_po	= msgfmt -o $@ $<

# non-verbose output
ifeq ($(verbose),no)
  echo_compile_c	= echo "  CC	 " $@
  echo_compile_cc	= echo "  CXX	 " $@
  echo_link_app		= echo "  LD	 " $@
  echo_link_so		= echo "  LD	 " $@
  echo_ar_lib		= echo "  AR	 " $@
  echo_moc_h		= echo "  MOC    " $@
  echo_msgfmt_po        = echo "  MSGFMT " $@
else
  echo_compile_c	= echo $(compile_c)
  echo_compile_cc	= echo $(compile_cc)
  echo_link_app		= echo $(link_app)
  echo_link_so		= echo $(link_so)
  echo_ar_lib		= echo $(ar_lib)
  echo_moc_h		= echo $(moc_h)
  echo_msgfmt_po	= echo $(msgfmt_po)
endif

%.o: %.c
	@$(cc_makedirs)
	@$(echo_compile_c)
	@$(compile_c)
	@$(fixup_deps)

%.o: %.cc
	@$(cc_makedirs)
	@$(echo_compile_cc)
	@$(compile_cc)
	@$(fixup_deps)

%.o: %.cpp
	@$(cc_makedirs)
	@$(echo_compile_cc)
	@$(compile_cc)
	@$(fixup_deps)


%.so: %.o
	@$(echo_link_so)
	@$(link_so)

%: %.o
	@$(echo_link_app)
	@$(link_app)

%.moc : %.h
	@$(echo_moc_h)
	@$(moc_h)

%.mo : %.po
	@$(echo_msgfmt_po)
	@$(msgfmt_po)


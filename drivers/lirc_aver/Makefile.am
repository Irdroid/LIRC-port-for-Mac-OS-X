## $Id: Makefile.am,v 1.5 1999/10/19 14:29:56 denis Exp $

## Process this file with automake to produce Makefile.in
EXTRA_DIST = patches/lirc_aver-kernel-2.2.10.diff patches/lirc_aver-kernel-2.2.12.diff
	     patches/lirc_aver-bttv-0.6.4.diff

## this is so that Automake includes the C compiling definitions, and
## includes the source files in the distribution.
EXTRA_PROGRAMS = automake_dummy
automake_dummy_SOURCES = lirc_aver.c

## there is no *just* object file support in automake.  This is close enough
module_DATA = lirc_aver.o

# where the kernel sources are located
KERNEL_LOCATION=@kerneldir@

# some magic for using linux kernel settings
# when compiling module(s)
CC           = @kernelcc@
MI_OBJS      = $(module_DATA)
EXTRA_CFLAGS = $(DEFS) -Wno-comment

$(module_DATA): $(automake_dummy_SOURCES) $(top_srcdir)/config.h ../lirc.h
  ifndef MAKING_MODULES
	DIR=`pwd`; (cd $(KERNEL_LOCATION); make SUBDIRS=$$DIR modules)
  endif
# the spaces above are necessary because automake otherwise will complain
# and even worse, it will delete the line

install-exec-local: mkdev
uninstall-local: rmdev

mkdev:
	test -c @devdir@/lirc || @mknod@ @devdir@/lirc c @lirc_major@ 0

rmdev:
	test -c @devdir@/lirc && $(RM) @devdir@/lirc

install-data-local:
	-@depmod@ -a

CLEANFILES = $(module_DATA) .$(module_DATA).flags *~

include $(KERNEL_LOCATION)/Rules.make

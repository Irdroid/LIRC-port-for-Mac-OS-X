## $Id: Makefile.am,v 1.1 1999/04/29 21:16:51 columbus Exp $

## Process this file with automake to produce Makefile.in

M_CFLAGS = -fomit-frame-pointer -funroll-loops
M_CPPFLAGS = -DLINUX -D__KERNEL__ -DMODULE
# M_CPPFLAGS = -DLINUX -D__KERNEL__ -DMODULE -DMODVERSIONS

## this is so that Automake includes the C compiling definitions, and
## includes the source files in the distribution.
EXTRA_PROGRAMS = dummy
dummy_SOURCES = lirc_parallel.c lirc_parallel.h

## there is no *just* object file support in automake.  This is close enough
module_SCRIPTS = lirc_parallel.o

lirc_parallel.o: lirc_parallel.c
	$(COMPILE) $(M_CPPFLAGS) $(M_CFLAGS) -c lirc_parallel.c

install-exec-local: mkdev
uninstall-local: rmdev

mkdev:
	test -c @devdir@/lirc || @mknod@ @devdir@/lirc @lirc_major@ c 0

rmdev:
	test -c @devdir@/lirc && $(RM) @devdir@/lirc


CLEANFILES = lirc_parallel.o *~

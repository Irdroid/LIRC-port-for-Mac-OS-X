## $Id: Makefile.am,v 1.2 1999/05/05 14:51:02 columbus Exp $

## Process this file with automake to produce Makefile.in

M_CFLAGS = -fomit-frame-pointer -funroll-loops
M_CPPFLAGS = -DLINUX -D__KERNEL__ -DMODULE
# M_CPPFLAGS = -DLINUX -D__KERNEL__ -DMODULE -DMODVERSIONS

## this is so that Automake includes the C compiling definitions
EXTRA_PROGRAMS = dummy
dummy_SOURCES = lirc_haup.c

## there is no *just* object file support in automake.  This is close enough
module_SCRIPTS = lirc_haup.o

lirc_haup.o: lirc_haup.c
	$(COMPILE) $(M_CPPFLAGS) $(M_CFLAGS) -c lirc_haup.c

install-exec-local: mkdev
uninstall-local: rmdev

mkdev:
	test -c @devdir@/lirc || @mknod@ @devdir@/lirc c @lirc_major@ 0

rmdev:
	test -c @devdir@/lirc && $(RM) @devdir@/lirc


CLEANFILES = lirc_haup.o *~

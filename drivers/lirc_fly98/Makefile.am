## $Id: Makefile.am,v 1.4 1999/07/27 09:19:27 jochym Exp $

## Process this file with automake to produce Makefile.in

KERNEL_SRC=/usr/local/src/linux-2.2/linux
KERNEL_INCLUDES=$(KERNEL_SRC)/include
KERNEL_CHARDEV=$(KERNEL_SRC)/drivers/char

M_CFLAGS = -fomit-frame-pointer -funroll-loops
M_CPPFLAGS = -DLINUX -D__KERNEL__ -DMODULE -I$(KERNEL_INCLUDES) -I$(KERNEL_CHARDEV)
# M_CPPFLAGS = -DLINUX -D__KERNEL__ -DMODULE -DMODVERSIONS

## this is so that Automake includes the C compiling definitions
EXTRA_PROGRAMS = dummy
dummy_SOURCES = lirc_fly98.c

## there is no *just* object file support in automake.  This is close enough
module_DATA = lirc_fly98.o

lirc_fly98.o: lirc_fly98.c
	$(COMPILE) $(M_CPPFLAGS) $(M_CFLAGS) -c lirc_fly98.c

install-exec-local: mkdev
uninstall-local: rmdev

mkdev:
	test -c @devdir@/lirc || @mknod@ @devdir@/lirc c @lirc_major@ 0

rmdev:
	test -c @devdir@/lirc && $(RM) @devdir@/lirc

install-data-local:
	-@depmod@ -a

CLEANFILES = lirc_fly98.o *~ \#*\#

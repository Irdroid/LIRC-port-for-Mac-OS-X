## $Id: Makefile.am,v 1.5 2004/01/18 21:20:26 ranty Exp $

## Process this file with automake to produce Makefile.in

EXTRA_DIST = 

## this is so that Automake includes the C compiling definitions, and
## includes the source files in the distribution.
EXTRA_PROGRAMS = automake_dummy
automake_dummy_SOURCES = lirc_i2c.c

## there is no *just* object file support in automake.  This is close enough
module_DATA = lirc_i2c.o

# where the kernel sources are located
KERNEL_LOCATION=@kerneldir@

LIRC_DEVDIR = $(shell pwd)

# some magic for using linux kernel settings
# when compiling module(s)
KBUILD_VERBOSE = 1
LIRC_EXTRA_CFLAGS = -DIRCTL_DEV_MAJOR=$(lirc_major) -DEXPORT_SYMTAB $(DEFS) \
		$(DEFAULT_INCLUDES) -I $(LIRC_DEVDIR)/../.. \
		-I $(KERNEL_LOCATION)/include/
export LIRC_EXTRA_CFLAGS KERNEL_LOCATION module_DATA

$(module_DATA): ${automake_dummy_SOURCES} $(top_srcdir)/config.h ../lirc.h
	mv Makefile Makefile.automake
	cp ../Makefile.kernel Makefile
	$(MAKE) -C $(KERNEL_LOCATION) SUBDIRS=$(LIRC_DEVDIR) modules \
		KBUILD_VERBOSE=$(KBUILD_VERBOSE)
	mv Makefile.automake Makefile

CLEANFILES = $(module_DATA) *~ \#*\#


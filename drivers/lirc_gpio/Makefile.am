## $Id: Makefile.am,v 1.7 2001/11/20 15:26:03 ranty Exp $

## Process this file with automake to produce Makefile.in

## this is so that Automake includes the C compiling definitions, and
## includes the source files in the distribution.
EXTRA_PROGRAMS = automake_dummy
automake_dummy_SOURCES = lirc_gpio.c

## there is no *just* object file support in automake.  This is close enough
module_DATA = lirc_gpio.o

# where the kernel sources are located
KERNEL_LOCATION=@kerneldir@

# some magic for using linux kernel settings
# when compiling module(s)
CC           = @kernelcc@
MI_OBJS      = $(module_DATA)
EXTRA_CFLAGS = $(DEFS) $(DEFAULT_INCLUDES)

$(module_DATA): $(automake_dummy_SOURCES) $(top_srcdir)/config.h ../lirc.h
  ifndef MAKING_MODULES
	DIR=`pwd`; (cd $(KERNEL_LOCATION); make SUBDIRS=$$DIR obj-m=$(module_DATA) modules)
  endif
# the spaces above are necessary because automake otherwise will complain
# and even worse, it will delete the line

CLEANFILES = $(module_DATA) .$(module_DATA).flags *~

include $(KERNEL_LOCATION)/Rules.make

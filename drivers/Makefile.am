## $Id: Makefile.am,v 5.6 1999/09/06 14:56:04 columbus Exp $

## Process this file with automake to produce Makefile.in 

## here the correct driver is substituted in by the configure script
## (or empty for none)

EXTRA_DIST = lirc.h

DIST_SUBDIRS = lirc_serial lirc_parallel lirc_aver lirc_haup lirc_fly98 \
	lirc_pixview
SUBDIRS = @lirc_driver@

CLEANFILES = *~

install-exec-local: mklink
uninstall-local: rmlink

mklink:
	test "@irtty@" = "" || test -L @devdir@/lirc || (cd `dirname @irtty@` && $(LN_S) `basename @irtty@` lirc)

rmlink:
	test "@irtty@" = "" || $(RM) @devdir@/lirc

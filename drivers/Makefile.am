## $Id: Makefile.am,v 5.5 1999/09/02 20:03:53 columbus Exp $

## Process this file with automake to produce Makefile.in 

## here the correct driver is substituted in by the configure script
## (or empty for none)

EXTRA_DIST = lirc.h

DIST_SUBDIRS = lirc_serial lirc_parallel lirc_aver lirc_haup lirc_fly98 \
	lirc_pixview irman
SUBDIRS = @lirc_driver@

CLEANFILES = *~

install-exec-local: mklink
uninstall-local: rmlink

mklink:
	test "@irtty@" = "" || test "@lirc_driver@" = "irman" || test -L @devdir@/lirc || (cd `dirname @irtty@` && $(LN_S) `basename @irtty@` lirc)

rmlink:
	test "@irtty@" = "" || test "@lirc_driver@" = "irman" || $(RM) @devdir@/lirc

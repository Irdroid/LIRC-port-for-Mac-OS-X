## $Id: Makefile.am,v 5.10 2000/04/02 13:07:21 columbus Exp $

## Process this file with automake to produce Makefile.in 

## here the correct driver is substituted in by the configure script
## (or empty for none)

EXTRA_DIST = lirc.h

DIST_SUBDIRS = lirc_serial lirc_parallel lirc_haup lirc_fly98 lirc_sir \
               lirc_dev lirc_gpio
SUBDIRS = @lirc_driver@

CLEANFILES = *~

install-exec-local: mklink
uninstall-local: rmlink

mklink:
	test "@irtty@" = "" || test -L @devdir@/lirc || (cd `dirname @irtty@` && $(LN_S) `basename @irtty@` lirc)

rmlink:
	test "@irtty@" = "" || $(RM) @devdir@/lirc

## $Id: Makefile.am,v 5.2 1999/05/14 22:22:06 wheeley Exp $

## Process this file with automake to produce Makefile.in 

## here the correct driver is substituted in by the configure script
## (or empty for none)

EXTRA_DIST = lirc.h

DIST_SUBDIRS = lirc_serial lirc_parallel lirc_aver lirc_haup lirc_fly98 irman
SUBDIRS = @lirc_driver@

CLEANFILES = *~

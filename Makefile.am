## $Id: Makefile.am,v 5.2 1999/07/21 18:23:37 columbus Exp $

## Process this file with automake to produce Makefile.in 
## Makefile.am, (c)1999 Tom Wheeley <tw104@york.ac.uk>

## we need version 1.4 of automake for DIST_SUBDIRS to work.
AUTOMAKE_OPTIONS = 1.4 check-news

EXTRA_DIST = setup.sh ANNOUNCE 
#TECHNICAL TODO USAGE

SUBDIRS = drivers daemons tools

## we need a way to install the remotes in /usr/local/share/lirc/remotes/
## without having a million Makefile.am's.  As I see it, for uninstall to
## work we will have to specify every remote. (otherwise uninstall would
## also remove files put in locally.  not good)

## From automake documentation:
## Note that EXTRA_DIST can only handle files in the current
## directory; files in other directories will cause make dist runtime
## failures.

dist-hook:
	for f in contrib remotes; do \
		cp -pr $(srcdir)/$$f $(distdir)/$$f; done
	-rcs2log >$(distdir)/ChangeLog

DISTCLEANFILES = configure.sh .setup.config

CLEANFILES = *~ remotes/*~ contrib/*~

## $Id: Makefile.am,v 5.9 2003/01/26 18:49:59 lirc Exp $

## Process this file with automake to produce Makefile.in 
## Makefile.am, (c)1999 Tom Wheeley <tw104@york.ac.uk>

## we need version 1.5 of automake for DIST_SUBDIRS to work and dist-bzip2.
AUTOMAKE_OPTIONS = 1.5 check-news dist-bzip2

EXTRA_DIST = setup.sh ANNOUNCE TODO

## daemons/ and tools/ should appear before doc/ for help2man to work.
SUBDIRS = drivers daemons tools doc

## From automake documentation:
## Note that EXTRA_DIST can only handle files in the current
## directory; files in other directories will cause make dist runtime
## failures.

dist-hook:
	for f in contrib remotes; do \
		cp -pr $(srcdir)/$$f $(distdir)/$$f; done
	rm -r `find $(distdir) -name "CVS"`
	-rcs2log >$(distdir)/ChangeLog

DISTCLEANFILES = configure.sh .setup.config

CLEANFILES = *~ remotes/*~ contrib/*~

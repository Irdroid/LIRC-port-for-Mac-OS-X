## $Id: Makefile.am,v 5.0 1999/04/29 21:30:59 columbus Exp $

## Process this file with automake to produce Makefile.in 
## Makefile.am, (c)1999 Tom Wheeley <tw104@york.ac.uk>

## we need version 1.4 of automake for DIST_SUBDIRS to work.
AUTOMAKE_OPTIONS = 1.4

EXTRA_DIST = ANNOUNCE 
#TECHNICAL TODO USAGE

SUBDIRS = drivers daemons tools

## we need a way to install the remotes in /usr/local/share/lirc/remotes/
## without having a million Makefile.am's.  As I see it, for uninstall to
## work we will have to specify every remote. (otherwise uninstall would
## also remove files put in locally.  not good)

## according to the documentation, I should be able to list these dirs
## in EXTRA_DIST, but that doesn't seem to work.
dist-hook:
	for f in contrib remotes; do \
		cp -pr $(srcdir)/$$f $(distdir)/$$f; done

CLEANFILES = *~ remotes/*~ contrib/*~

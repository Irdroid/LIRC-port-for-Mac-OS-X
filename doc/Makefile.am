## $Id: Makefile.am,v 1.5 2002/01/06 13:39:40 lirc Exp $

## Process this file with automake to produce Makefile.in 

EXTRA_DIST=lirc.css irxevent.keys

SUBDIRS = man

noinst_PROGRAMS = man2html

release: man2html
	./release-man.sh
	./release-html.sh

dist-hook: release
	cp -a html images $(distdir)

maintainer-clean-local:
	rm -rf html man-html

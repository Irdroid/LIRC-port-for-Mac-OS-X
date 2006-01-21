## $Id: Makefile.am,v 1.6 2006/01/21 17:17:29 lirc Exp $

## Process this file with automake to produce Makefile.in 

EXTRA_DIST=lirc.css irxevent.keys

SUBDIRS = man

noinst_PROGRAMS = man2html

release-man: man2html
	./release-man.sh

release-html:
	./release-html.sh

release: release-man release-html

dist-hook: release
	cp -a html images $(distdir)

maintainer-clean-local:
	rm -rf html man-html

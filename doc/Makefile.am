## $Id: Makefile.am,v 1.8 2007/06/09 19:05:11 lirc Exp $

## Process this file with automake to produce Makefile.in 

EXTRA_DIST=lirc.css irxevent.keys lirc.hwdb

SUBDIRS = man

lirc.hwdb: release

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

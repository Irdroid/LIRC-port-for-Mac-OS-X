## $Id: Makefile.am,v 1.11 2007/11/04 09:29:02 lirc Exp $

## Process this file with automake to produce Makefile.in 

EXTRA_DIST=lirc.css irxevent.keys lirc.hwdb

SUBDIRS = man

lirc.hwdb:
	touch $@

noinst_PROGRAMS = man2html

release-man: man2html
	top_srcdir="$(top_srcdir)" srcdir="$(srcdir)" $(srcdir)/release-man.sh

release-html:
	top_srcdir="$(top_srcdir)" srcdir="$(srcdir)" $(srcdir)/release-html.sh

release: release-man release-html

dist-hook: release
	cp -a html $(srcdir)/images $(distdir)

maintainer-clean-local:
	rm -rf html man-html

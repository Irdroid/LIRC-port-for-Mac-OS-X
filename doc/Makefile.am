## $Id: Makefile.am,v 1.4 2001/12/17 12:50:03 lirc Exp $

## Process this file with automake to produce Makefile.in 

EXTRA_DIST=lirc.css irxevent.keys

man_MANS= man/irexec.1 man/irpty.1 man/irrecord.1 man/irw.1 man/irxevent.1 \
	man/lircd.8 man/lircmd.8 man/mode2.1 man/smode2.1 man/xmode2.1

noinst_PROGRAMS = man2html

%.1 %.8: man2html
	./release-man.sh

release: man2html
	./release-man.sh
	./release-html.sh

dist-hook: release
	cp -a html images man $(distdir)

maintainer-clean-local:
	rm -rf html man man-html

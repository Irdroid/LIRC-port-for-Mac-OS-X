## $Id: Makefile.am,v 1.3 2001/11/20 15:12:32 ranty Exp $

## Process this file with automake to produce Makefile.in 

man_MANS= man/irexec.1 man/irpty.1 man/irrecord.1 man/irw.1 man/irxevent.1 \
	man/lircd.8 man/lircmd.8 man/mode2.1 man/smode2.1 man/xmode2.1

noinst_PROGRAMS = man2html

%.1 %.8: man2html
	./release-man.sh

release: man2html
	./release-man.sh
	./release-html.sh

dist-hook: release
	cp -a doc.html irxevent.keys html images man $(distdir)

maintainer-clean-local:
	rm -rf html man man-html

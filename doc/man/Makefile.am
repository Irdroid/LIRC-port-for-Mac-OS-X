## $Id: Makefile.am,v 1.2 2002/01/18 21:14:13 lirc Exp $

## Process this file with automake to produce Makefile.in 

man_MANS= irexec.1 irpty.1 irrecord.1 irw.1 irxevent.1 \
	lircd.8 lircmd.8 mode2.1 smode2.1 xmode2.1

EXTRA_DIST = $(man_MANS)

##noinst_PROGRAMS = man2html

all: $(man_MANS)

%.1 %.8:
	(cd .. && make release)

maintainer-clean-local:
	rm -f $(man_MANS)

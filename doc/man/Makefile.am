## $Id: Makefile.am,v 1.1 2002/01/06 13:39:40 lirc Exp $

## Process this file with automake to produce Makefile.in 

man_MANS= irexec.1 irpty.1 irrecord.1 irw.1 irxevent.1 \
	lircd.8 lircmd.8 mode2.1 smode2.1 xmode2.1

##noinst_PROGRAMS = man2html

all: $(man_MANS)

%.1 %.8:
	(cd .. && make release)

maintainer-clean-local:
	rm -f $(man_MANS)

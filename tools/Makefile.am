## $Id: Makefile.am,v 5.0 1999/04/29 21:30:59 columbus Exp $

## Process this file with automake to produce Makefile.in 

EXTRA_PROGRAMS = smode2 xmode2 irxevent
bin_PROGRAMS = irw irpty irexec irrecord @vga_progs@ @x_progs@

## simple programs
irw_SOURCES = irw.c
irpty_SOURCES = irpty.c lirc_client.c lirc_client.h
irexec_SOURCES = irexec.c lirc_client.c lirc_client.h

## irrecord
irrecord_SOURCES = irrecord.c
irrecord_LDADD = $(top_srcdir)/daemons/dump_config.o \
                 $(top_srcdir)/daemons/config_file.o \
                 $(top_srcdir)/daemons/ir_remote.o

$(top_srcdir)/daemons/dump_config.o: $(top_srcdir)/daemons/dump_config.c
	$(COMPILE) -c $(top_srcdir)/daemons/dump_config.c -o $@
$(top_srcdir)/daemons/config_file.o: $(top_srcdir)/daemons/config_file.c
	$(COMPILE) -c $(top_srcdir)/daemons/config_file.c -o $@
$(top_srcdir)/daemons/ir_remote.o: $(top_srcdir)/daemons/ir_remote.c
	$(COMPILE) -c $(top_srcdir)/daemons/ir_remote.c -o $@

## vga programs
smode2_SOURCES = smode2.c
smode2_LDADD = -lvga -lvgagl

## x programs
irxevent_SOURCES = irxevent.c lirc_client.c lirc_client.h
irxevent_CFLAGS = @X_CFLAGS@
irxevent_LDADD = @X_LIBS@ @X_PRE_LIBS@ -lX11 @X_EXTRA_LIBS@
xmode2_SOURCES = xmode2.c
xmode2_CFLAGS = @X_CFLAGS@
xmode2_LDADD = @X_LIBS@ @X_PRE_LIBS@ -lX11 @X_EXTRA_LIBS@

CLEANFILES = *~

## $Id: Makefile.am,v 1.1 1999/04/29 21:16:52 columbus Exp $

## Process this file with automake to produce Makefile.in 

sbin_PROGRAMS = lirmand
lirmand_SOURCES = lirmand.c

# this can be either -lirman or -lirman_sw
lirmand_LDADD = @irman_lib@

install-exec-local: mkdev
uninstall-local: rmdev

mkdev:
	test -p @devdir@/lirc || @mknod@ @devdir@/lirc p

rmdev:
	test -p @devdir@/lirc && $(RM) @devdir@/lirc

CLEANFILES = *~

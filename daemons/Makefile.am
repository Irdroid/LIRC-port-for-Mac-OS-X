## $Id: Makefile.am,v 1.1 1999/04/29 21:16:54 columbus Exp $

## Process this file with automake to produce Makefile.in 

## these are used by programs in the tools directory, but not by
## programs in this directory.  So we must make sure they get distributed

EXTRA_DIST = dump_config.c dump_config.h

sbin_PROGRAMS = lircd lircmd

lircd_SOURCES = lircd.c ir_remote.c config_file.c \
                lircd.h ir_remote.h config_file.h
lircmd_SOURCES = lircmd.c

install-exec-local: mkdev
uninstall-local: rmdev

mkdev:
	test -e @devdir@/lircd || @mknod@ @devdir@/lircd p
	test -e @devdir@/lircm || @mknod@ @devdir@/lircm p

rmdev:
	$(RM) @devdir@/lircd
	$(RM) @devdir@/lircm



CLEANFILES = *~


## $Id: Makefile.am,v 5.2 1999/05/15 15:34:04 columbus Exp $

## Process this file with automake to produce Makefile.in 

## these are used by programs in the tools directory, but not by
## programs in this directory.  So we must make sure they get distributed

EXTRA_DIST = dump_config.c dump_config.h

sbin_PROGRAMS = lircd lircmd


lircd_SOURCES = lircd.c ir_remote.c config_file.c \
                lircd.h ir_remote.h config_file.h
lircmd_SOURCES = lircmd.c

## maintainer mode stuff
EXTRA_PROGRAMS = lircd.simsend lircd.simrec
noinst_PROGRAMS = @maintmode_daemons_extra@
lircd_simsend_SOURCES = lircd_simsend.c ir_remote_simsend.c config_file.c
lircd_simsend.o:
	$(COMPILE) -DSIM_SEND -c lircd.c -o lircd_simsend.o
ir_remote_simsend.o:
	$(COMPILE) -DSIM_SEND -c ir_remote.c -o ir_remote_simsend.o
lircd_simrec_SOURCES = lircd_simrec.c ir_remote.c config_file.c
lircd_simrec.o:
	$(COMPILE) -DSIM_REC -c lircd.c -o lircd_simrec.o


install-exec-local: mkdev
uninstall-local: rmdev

mkdev:
	test -e @devdir@/lircd || @mknod@ @devdir@/lircd p
	test -e @devdir@/lircm || @mknod@ @devdir@/lircm p

rmdev:
	$(RM) @devdir@/lircd
	$(RM) @devdir@/lircm



CLEANFILES = *~








## $Id: Makefile.am,v 5.23 2002/04/22 19:07:47 lirc Exp $

## Process this file with automake to produce Makefile.in 

## these are used by programs in the tools directory, but not by
## programs in this directory.  So we must make sure they get distributed

EXTRA_DIST = dump_config.c dump_config.h

sbin_PROGRAMS = lircd lircmd

lircd_SOURCES = lircd.c ir_remote.c config_file.c \
		lircd.h ir_remote.h config_file.h \
		hw-types.c hw-types.h hardware.h
lircd_LDADD = @hw_module@ @irman_lib@ @caraca_lib@ @daemon@
lircd_DEPENDENCIES = @hw_module@
EXTRA_lircd_SOURCES = hw_default.c hw_default.h \
			hw_pixelview.c hw_pixelview.h \
			hw_pinsys.c hw_pinsys.h \
			hw_logitech.c hw_logitech.h \
			hw_slinke.c hw_slinke.h \
			hw_irman.c hw_irman.h \
			hw_creative.c hw_creative.h \
			hw_silitek.c hw_silitek.h \
		        hw_dsp.c \
			receive.c receive.h \
			transmit.c transmit.h \
			serial.c serial.h

lircmd_SOURCES = lircmd.c
lircmd_LDADD = @daemon@

bin_PROGRAMS = irrecord

irrecord_SOURCES = irrecord.c dump_config.c dump_config.h \
		config_file.c config_file.h hardware.h \
		hw-types.c hw-types.h ir_remote.c ir_remote.h
irrecord_LDADD = @hw_module@ @irman_lib@ @caraca_lib@
irrecord_DEPENDENCIES = @hw_module@
EXTRA_irrecord_SOURCES = hw_default.c hw_default.h \
			hw_pixelview.c hw_pixelview.h \
			hw_pinsys.c hw_pinsys.h \
			hw_logitech.c hw_logitech.h \
			hw_slinke.c hw_slinke.h \
			hw_irman.c hw_irman.h \
			hw_creative.c hw_creative.h \
			hw_silitek.c hw_silitek.h \
			hw_dsp.c \
			receive.c receive.h \
			transmit.c transmit.h \
			serial.c serial.h

## maintainer mode stuff
EXTRA_PROGRAMS = lircd.simsend lircd.simrec slinke
noinst_PROGRAMS = @maintmode_daemons_extra@
lircd_simsend_SOURCES = lircd.c ir_remote.c config_file.c \
		lircd.h ir_remote.h config_file.h \
		hw-types.c hw-types.h hardware.h \
		hw_default.c hw_default.h \
		receive.c receive.h \
		transmit.c transmit.h
lircd_simsend_CFLAGS = -DSIM_SEND
lircd_simrec_SOURCES = lircd.c ir_remote.c config_file.c \
		lircd.h ir_remote.h config_file.h \
		hw-types.c hw-types.h hardware.h \
		hw_default.c hw_default.h \
		receive.c receive.h \
		transmit.c transmit.h
lircd_simrec_CFLAGS = -DSIM_REC

slinke_SOURCES = slinke.c slinke.h config_file.c config_file.h ir_remote.h \
		dump_config.c dump_config.h

if MANAGE_DEVICES
install-exec-local: mkdev
uninstall-local: rmdev
endif

install-data-local:
	test "$(lircd_conf)" = ""  || test -e $(DESTDIR)$(sysconfdir)/lircd.conf  || ($(mkinstalldirs) $(DESTDIR)$(sysconfdir) && $(INSTALL_DATA) $(top_srcdir)/remotes/$(lircd_conf) $(DESTDIR)$(sysconfdir)/lircd.conf)
	test "$(lircmd_conf)" = "" || test -e $(DESTDIR)$(sysconfdir)/lircmd.conf || ($(mkinstalldirs) $(DESTDIR)$(sysconfdir) && $(INSTALL_DATA) $(top_srcdir)/remotes/$(lircmd_conf) $(DESTDIR)$(sysconfdir)/lircmd.conf)

mkdev:
	test -e $(DESTDIR)$(devdir)/lircd || ($(mkinstalldirs) $(DESTDIR)$(devdir) && @mknod@ $(DESTDIR)$(devdir)/lircd p)
	test -e $(DESTDIR)$(devdir)/lircm || ($(mkinstalldirs) $(DESTDIR)$(devdir) && @mknod@ $(DESTDIR)$(devdir)/lircm p)

rmdev:
	-$(RM) $(DESTDIR)$(devdir)/lircd
	-$(RM) $(DESTDIR)$(devdir)/lircm

CLEANFILES = *~

## $Id: Makefile.am,v 5.17 2002/03/28 21:26:54 lirc Exp $

## Process this file with automake to produce Makefile.in 

## here the correct driver is substituted in by the configure script
## (or empty for none)

EXTRA_DIST = lirc.h

DIST_SUBDIRS = lirc_serial lirc_parallel lirc_i2c lirc_sir lirc_dev lirc_gpio \
	lirc_it87 lirc_bt829
SUBDIRS = @lirc_driver@

CLEANFILES = *~

if MANAGE_DEVICES
install-exec-local: mklink mkdev
uninstall-local: rmlink mkdev
endif

mklink:
	test "@irtty@" = "" || test -L $(DESTDIR)$(devdir)/lirc || ($(mkinstalldirs) $(DESTDIR)$(devdir) && cd `dirname $(DESTDIR)@irtty@` && $(LN_S) `basename @irtty@` lirc)

rmlink:
	-test "@irtty@" = "" || $(RM) $(DESTDIR)$(devdir)/lirc

mkdev:
	test "@driver@" != "mediafocusI" || test -c $(DESTDIR)$(devdir)/lirc || ($(mkinstalldirs) $(DESTDIR)$(devdir) && @mknod@ $(DESTDIR)$(devdir)/lirc c @lirc_major@ 0)

rmdev:
	-test "@driver@" != "mediafocusI" || test -c $(DESTDIR)$(devdir)/lirc || $(RM) $(DESTDIR)$(devdir)/lirc

## $Id: Makefile.am,v 5.22 2004/02/29 12:10:15 lirc Exp $

## Process this file with automake to produce Makefile.in 

## here the correct driver is substituted in by the configure script
## (or empty for none)

EXTRA_DIST = lirc.h kcompat.h Makefile.kernel

DIST_SUBDIRS = lirc_serial lirc_parallel lirc_i2c lirc_sir lirc_dev lirc_gpio \
	lirc_it87 lirc_bt829 lirc_atiusb lirc_mceusb
SUBDIRS = @lirc_driver@

CLEANFILES = *~

if SANDBOXED
else
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

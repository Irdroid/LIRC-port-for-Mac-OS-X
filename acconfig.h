/*      $Id: acconfig.h,v 5.29 2004/09/11 19:56:51 lirc Exp $      */

/*
 *  are you editing the correct file?
 *  
 *  acconfig.h  - changes for distribution
 *                you must run autoheader / configure
 *  config.h.in - changes specific to your installation
 *                these will be lost if you run autoheader
 *  config.h    - changes to this configuration
 *                these will be lost if you run configure
 */

/* note.
 * if you want to change silly things like the device file names or the
 * configuration file names then remember you may also need to change
 * the Makefile.am files.
 */

/* device file names - beneath DEVDIR (default /dev) */
#define DEV_LIRC	"lirc"
#define DEV_LIRCD	"lircd"
#define DEV_LIRCM	"lircm"

/* config file names - beneath SYSCONFDIR (default /etc) */
#define CFG_LIRCD	"lircd.conf"
#define CFG_LIRCM	"lircmd.conf"

/* config file names - beneath $HOME or SYSCONFDIR */
#define CFG_LIRCRC	"lircrc"

/* log files */
#define LOG_LIRCD	"lircd"
#define LOG_LIRMAND	"lirmand"

/* pid file */
#define PID_LIRCD       "lircd.pid"

/* default port number */
#define	LIRC_INET_PORT	8765

/*
 * below here are defines managed by autoheader / autoconf
 */

@TOP@

/* define in maintainer mode */
#undef MAINTAINER_MODE

/* Define to use long long IR codes */
#undef LONG_IR_CODE

/* Define to use dynamic IR codes */
#undef DYNCODES

/* Define to enable debugging output */
#undef DEBUG

/* Define to run daemons as daemons */
#undef DAEMONIZE

/* Define if forkpty is available */
#undef HAVE_FORKPTY

/* Define if the caraca library is installed */
#undef HAVE_LIBCARACA

/* Define if the libirman library is installed */
#undef HAVE_LIBIRMAN

/* Define if the software only test version of libirman is installed */
#undef HAVE_LIBIRMAN_SW

/* Define if the portaudio library is installed */
#undef HAVE_LIBPORTAUDIO

/* Define if the ALSA library is installed */
#undef HAVE_LIBALSA

/* Define if libusb is installed */
#undef HAVE_LIBUSB

/* Define if the complete vga libraries (vga, vgagl) are installed */
#undef HAVE_LIBVGA

/* define if you have vsyslog( prio, fmt, va_arg ) */
#undef HAVE_VSYSLOG

/* define if you want to log to syslog instead of logfile */
#undef USE_SYSLOG

/* Text string signifying which driver is configured */
#define LIRC_DRIVER		"unknown"

/* Set the device major for the lirc driver */
#define LIRC_MAJOR		61

/* Set the IRQ for the lirc driver */
#define LIRC_IRQ		4

/* Set the port address for the lirc driver */
#define LIRC_PORT		0x3f8

/* Set the timer for the parallel port driver */
#define LIRC_TIMER		65536

/* Define if you have an animax serial port receiver */
#undef LIRC_SERIAL_ANIMAX

/* Define if you have a IR diode connected to the serial port */
#undef LIRC_SERIAL_TRANSMITTER

/* Define if the software needs to generate the carrier frequency */
#undef LIRC_SERIAL_SOFTCARRIER

/* Define if you have an IRdeo serial port receiver */
#undef LIRC_SERIAL_IRDEO

/* Define if you have an IRdeo remote transmitter */
#undef LIRC_SERIAL_IRDEO_REMOTE

/* Define if you have an Igor Cesko receiver */
#undef LIRC_SERIAL_IGOR

/* Define if you want to cross-compile for the SA1100 */
#undef LIRC_ON_SA1100

/* Define if you want to use a Tekram Irmate 210 */
#undef LIRC_SIR_TEKRAM

/* Define if you want to use a Actisys Act200L */
#undef LIRC_SIR_ACTISYS_ACT200L

/* Define if devfs support is present in current kernel */
#undef LIRC_HAVE_DEVFS

/* syslog facility to use */
#define LIRC_SYSLOG		LOG_DAEMON

/* modifiable single-machine data */
#define LOCALSTATEDIR           "/var"

/* Define to include most drivers */
#undef LIRC_DRIVER_ANY

/* The name of the hw_* structure to use by default */
#undef HW_DEFAULT

/* system configuration directory */
#define SYSCONFDIR		"/etc"

/* device files directory */
#define DEVDIR			"/dev"

/* This should only be set by configure */
#define PACKAGE			"unset"

/* This should only be set by configure */
#define VERSION			"0.0.0"

@BOTTOM@

/*
 * compatibility and useability defines
 */

/* FIXME */
#ifdef LIRC_HAVE_DEVFS
#define LIRC_DRIVER_DEVICE	DEVDIR "/" DEV_LIRC "/0"
#else
#define LIRC_DRIVER_DEVICE      DEVDIR "/" DEV_LIRC
#endif /* LIRC_HAVE_DEVFS */

/* Set the default tty used by the irman/remotemaster driver */
#define LIRC_IRTTY		DEVDIR "/" "ttyS0"

#define LIRCD			DEVDIR "/" DEV_LIRCD
#define LIRCM			DEVDIR "/" DEV_LIRCM

#define LIRCDCFGFILE		SYSCONFDIR "/" CFG_LIRCD
#define LIRCMDCFGFILE		SYSCONFDIR "/" CFG_LIRCM

#define LIRCRC_USER_FILE	"." CFG_LIRCRC
#define LIRCRC_ROOT_FILE	SYSCONFDIR "/" CFG_LIRCRC

#define LOGFILE			LOCALSTATEDIR "/log/" LOG_LIRCD
#define LIRMAND_LOGFILE		LOCALSTATEDIR "/log/" LOG_LIRMAND

#define PIDFILE                 LOCALSTATEDIR "/run/" PID_LIRCD

/* end of acconfig.h */

/*      $Id: lirmand.c,v 1.1 1999/04/29 21:16:53 columbus Exp $      */

/* lirmand.c v0.5.5 (c) 1998-1999 Tom Wheeley <tomw@tsys.demon.co.uk> */
/* This program is free software.  See file COPYING for details       */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>

#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
  
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <irman.h>

/* default bit order is MSB_TO_LSB */
#ifndef LSB_TO_MSB
# ifndef MSB_TO_LSB	/* neither defined */
#  define MSB_TO_LSB 1
# endif
#else
# ifdef MSB_TO_LSB	/* both defined */
#  error "both LSB_TO_MSB and MSB_TO_LSB are defined. cannot cope"
# endif
#endif

/* how lazy... */
#define STRERR	strerror(errno)

unsigned long gap 	=  0x00010000;
unsigned long pulse	=  0x01000400;
unsigned long one_space =  0x00000c00;
unsigned long zero_space = 0x00000800;

char *progname = "lirmand";

/* these should be set by config.h (via acconfig.h) */
#ifndef LIRC_DRIVER_DEVICE
# define LIRC_DRIVER_DEVICE	"/tmp/lirc"
#endif
#ifndef LIRMAND_LOGFILE
# define LIRMAND_LOGFILE	"/tmp/lirmand-log"
#endif

int lirc = 0;	/* file descriptor */
int is_daemon = 0;
FILE *log = NULL;

FILE *open_log(void)
{
  time_t t=time(NULL);

  if (!log) {
    log = fopen(LIRMAND_LOGFILE, "a");
    if (log) {
      fprintf(log, "%s started at %s\n", progname, ctime(&t));
    }
  }
  return log;
}

void close_log(void)
{
  time_t t = time(NULL);
  
  if (log) {
    fprintf(log, "%s stopped at %s\n", progname, ctime(&t));
    fclose(log);
  }
  log = NULL;
}


int debug_printf(char *format, ...)
{
  va_list va;
  int ret = 0;
  
  if (!open_log())
    return 0;
  
  va_start(va, format);
  ret = vfprintf(log, format, va);
  if (!is_daemon) {
    fprintf(stderr, "%s: ", progname);
    vfprintf(stderr, format, va);
  }
  va_end(va);

  return ret;
}


/* print errors */
int eprintf(char *format, ...)
{
  va_list va;
  int ret = 0;

  va_start(va, format);
  if (open_log()) {
    ret  = fprintf(log, "error: ");
    ret += vfprintf(log, format, va);
  }
  if (!is_daemon) {
    ret  = fprintf(stderr, "%s: ", progname);
    ret += vfprintf(stderr, format, va);
  }
  va_end(va);

  return ret;
}


/* Here I have attempted to implement REC-80 encoding, but it is quite likely
 * I've not done it right as I wrote this without the specs in front of me
 */

void write_encoded_bit(int value)
{
  if (value != 0) {
    write(lirc, &one_space, sizeof one_space);
  } else {
    write(lirc, &zero_space, sizeof zero_space);
  }
  write(lirc, &pulse, sizeof pulse);
}


/* brrrr */
#ifdef MSB_TO_LSB
#  define ORDER(X)	(bit)
#else
#  define ORDER(x)	(numbits - bit - 1)
#endif

void write_encoded_uchar(unsigned char value, int numbits)
{
  int bit;
  int max = 1 << (numbits - 1);
  
  for (bit=0; bit<numbits; bit++) {
    write_encoded_bit((value & (max >> ORDER(bit))) ? 1 : 0);
  } 

}


RETSIGTYPE sigterm(int sig)
{
  ir_finish();
  close(lirc);
  close_log();
  raise(sig);
}


#ifdef DAEMONIZE
void daemonize(void)
{
  pid_t pid;
	
  if ((pid = fork()) < 0) {		/* error */

    eprintf("fork() failed: `%s'\n", STRERR);
    exit(1);

  } else if (pid) {			/* parent */

    printf("%s installed (%d)\n", progname, (int)pid);
    exit(0);

  } else {				/* child */

    is_daemon = 1;
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);
    setsid();
    chdir("/");
    umask(0);
  }
}
#endif
 

void loop()
{
  unsigned char *code;
  char *text = NULL;
  int i;

  errno = 0;
  for(;;) {

    code = ir_get_code();

    if (code) {
      text = ir_code_to_text(code);
      debug_printf("rx `%s'\n", text);

      write(lirc, &gap, sizeof gap);
      write(lirc, &pulse, sizeof pulse);

      /* irrecord expects >20% of the bits to be '1'.
	 The percentage in the code the Irman sends is too low.
	 We'll fix that by sending a dummy 'pre_data' consisting of 0xffff */

      write_encoded_uchar(0xff, 8);
      write_encoded_uchar(0xff, 8);

      for(i = 0; i < IR_CODE_LEN; i++) {
        write_encoded_uchar(code[i], 8);
      }

    /* error handling dullness */
    } else {
      if (errno == IR_EDUPCODE) {
        debug_printf("rx `%s' (dup - ignored)\n", text?text:"(null - bug)");
      } else {
        if (errno == IR_EDISABLED) {
          eprintf("irman not initialised (this is a bug)\n");
        } else {
          eprintf("error reading code: `%s'\n", ir_strerror(errno));
        }
        return;
      }
    }
  }

}

int main(int argc, char **argv)
{
  char *filename;

  if (ir_init_commands(NULL, 1) < 0) {
    fprintf(stderr, "error initialising irman software: %s\n", strerror(errno));
    exit(1);
  }

  filename = ir_default_portname();
  
  if (argc == 2) {
    filename = argv[1];
  } else {
    if (!filename) {
      fprintf(stderr, "usage: lirmand device\n");
      exit(0);
    }
  }

  errno = 0;  
  if (ir_init(filename) < 0) {
    eprintf("error initialising irman hardware: `%s'\n", STRERR);
    exit(1);
  }

  /* try and create the fifo we write to lircd through.  If it exists then
   * we either get EEXIST or EACCES, so we can ignore these errors.  We get
   * EACCES when it exists and we are not root.  open() will throw up an error
   * if we don't have permission to write, so we can ignore EACCES for now.
   */

  if (mkfifo(LIRC_DRIVER_DEVICE, 0644) == -1) {
    if(errno != EEXIST && errno != EACCES) {
      eprintf("could not create fifo: `%s'\n", STRERR);
      exit(1);
    }
  }
  
  lirc = open(LIRC_DRIVER_DEVICE, O_RDWR|O_NONBLOCK);
  if (lirc == -1) {
    eprintf("could not open fifo: `%s'\n", STRERR);
    exit(1);
  }

  /* borrowed from lircmd */
  signal(SIGPIPE,SIG_IGN);
  signal(SIGTERM,sigterm);
  signal(SIGINT,sigterm);

#ifdef DAEMONIZE
  daemonize();
#else
  fprintf(stderr, "%s running (non daemon)\n", progname);
#endif

  loop();

  ir_finish();
  close(lirc);
  return 0;
}

/* end of lirmand.c */

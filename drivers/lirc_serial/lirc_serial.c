/*      $Id: lirc_serial.c,v 5.39 2002/10/02 18:35:32 lirc Exp $      */

/****************************************************************************
 ** lirc_serial.c ***********************************************************
 ****************************************************************************
 *
 * lirc_serial - Device driver that records pulse- and pause-lengths
 *               (space-lengths) between DDCD event on a serial port.
 *
 * Copyright (C) 1996,97 Ralph Metzler <rjkm@thp.uni-koeln.de>
 * Copyright (C) 1998 Trent Piepho <xyzzy@u.washington.edu>
 * Copyright (C) 1998 Ben Pfaff <blp@gnu.org>
 * Copyright (C) 1999 Christoph Bartelmus <lirc@bartelmus.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/* Steve's changes to improve transmission fidelity:
     - for systems with the rdtsc instruction and the clock counter, a 
       send_pule that times the pulses directly using the counter.
       This means that the LIRC_SERIAL_TRANSMITTER_LATENCY fudge is
       not needed. Measurement shows very stable waveform, even where
       PCI activity slows the access to the UART, which trips up other
       versions.
     - For other system, non-integer-microsecond pulse/space lengths,
       done using fixed point binary. So, much more accurate carrier
       frequency.
     - fine tuned transmitter latency, taking advantage of fractional
       microseconds in previous change
     - Fixed bug in the way transmitter latency was accounted for by
       tuning the pulse lengths down - the send_pulse routine ignored
       this overhead as it timed the overall pulse length - so the
       pulse frequency was right but overall pulse length was too
       long. Fixed by accounting for latency on each pulse/space
       iteration.

   Steve Davies <steve@daviesfam.org>  July 2001
*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
 
#include <linux/version.h>
#if LINUX_VERSION_CODE >= 0x020100
#define KERNEL_2_1
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,3,0)
#define KERNEL_2_3
#endif
#endif

#if LINUX_VERSION_CODE >= 0x020212
#define LIRC_LOOPS_PER_JIFFY
#endif

#include <linux/config.h>

#ifndef CONFIG_SERIAL_MODULE
#warning "******************************************"
#warning " Your serial port driver is compiled into "
#warning " the kernel. You will have to release the "
#warning " port you want to use for LIRC with:      "
#warning "    setserial /dev/ttySx uart none        "
#warning "******************************************"
#endif

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/serial_reg.h>
#include <linux/time.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/delay.h>
#ifdef KERNEL_2_1
#include <linux/poll.h>
#endif

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/fcntl.h>

#include "drivers/lirc.h"

#if defined(LIRC_SERIAL_SOFTCARRIER) && !defined(LIRC_SERIAL_TRANSMITTER)
#warning "Software carrier only affects transmitting"
#endif

#if defined(rdtsc) && defined(KERNEL_2_1)

#define USE_RDTSC
#warning "Note: using rdtsc instruction"

#endif

#ifdef LIRC_SERIAL_ANIMAX
#ifdef LIRC_SERIAL_TRANSMITTER
#warning "******************************************"
#warning " This receiver does not have a            "
#warning " transmitter diode                        "
#warning "******************************************"
#endif
#endif

struct lirc_serial
{
	int type;
	int signal_pin;
	int signal_pin_change;
	int on;
	int off;
	long (*send_pulse)(unsigned long length);
	void (*send_space)(long length);
	int features;
};

#define LIRC_HOMEBREW        0
#define LIRC_IRDEO           1
#define LIRC_IRDEO_REMOTE    2
#define LIRC_ANIMAX          3

#ifdef LIRC_SERIAL_IRDEO
int type=LIRC_IRDEO;
#elif defined(LIRC_SERIAL_IRDEO_REMOTE)
int type=LIRC_IRDEO_REMOTE;
#elif defined(LIRC_SERIAL_ANIMAX)
int type=LIRC_ANIMAX;
#else
int type=LIRC_HOMEBREW;
#endif

#ifdef LIRC_SERIAL_SOFTCARRIER
int softcarrier=1;
#else
int softcarrier=0;
#endif

/* forward declarations */
long send_pulse_irdeo(unsigned long length);
long send_pulse_homebrew(unsigned long length);
void send_space_irdeo(long length);
void send_space_homebrew(long length);

struct lirc_serial hardware[]=
{
	/* home-brew receiver/transmitter */
	{
		LIRC_HOMEBREW,
		UART_MSR_DCD,
		UART_MSR_DDCD,
		UART_MCR_RTS|UART_MCR_OUT2|UART_MCR_DTR,
		UART_MCR_RTS|UART_MCR_OUT2,
		send_pulse_homebrew,
		send_space_homebrew,
		(
#ifdef LIRC_SERIAL_TRANSMITTER
		 LIRC_CAN_SET_SEND_DUTY_CYCLE|
		 LIRC_CAN_SET_SEND_CARRIER|
		 LIRC_CAN_SEND_PULSE|
#endif
		 LIRC_CAN_REC_MODE2)
	},
	
	/* IRdeo classic */
	{
		LIRC_IRDEO,
		UART_MSR_DSR,
		UART_MSR_DDSR,
		UART_MCR_OUT2,
		UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2,
		send_pulse_irdeo,
		send_space_irdeo,
		(LIRC_CAN_SET_SEND_DUTY_CYCLE|
		 LIRC_CAN_SEND_PULSE|
		 LIRC_CAN_REC_MODE2)
	},
	
	/* IRdeo remote */
	{
		LIRC_IRDEO_REMOTE,
		UART_MSR_DSR,
		UART_MSR_DDSR,
		UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2,
		UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2,
		send_pulse_irdeo,
		send_space_irdeo,
		(LIRC_CAN_SET_SEND_DUTY_CYCLE|
		 LIRC_CAN_SEND_PULSE|
		 LIRC_CAN_REC_MODE2)
	},
	
	/* AnimaX */
	{
		LIRC_ANIMAX,
		UART_MSR_DCD,
		UART_MSR_DDCD,
		0,
		UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2,
		NULL,
		NULL,
		LIRC_CAN_REC_MODE2
	}
};

#define LIRC_DRIVER_NAME "lirc_serial"

#define RS_ISR_PASS_LIMIT 256

/* A long pulse code from a remote might take upto 300 bytes.  The
   daemon should read the bytes as soon as they are generated, so take
   the number of keys you think you can push before the daemon runs
   and multiply by 300.  The driver will warn you if you overrun this
   buffer.  If you have a slow computer or non-busmastering IDE disks,
   maybe you will need to increase this.  */

/* This MUST be a power of two!  It has to be larger than 1 as well. */

#define RBUF_LEN 256
#define WBUF_LEN 256

static int major = LIRC_MAJOR;
static int sense = -1;   /* -1 = auto, 0 = active high, 1 = active low */

#ifdef KERNEL_2_3
static DECLARE_WAIT_QUEUE_HEAD(lirc_wait_in);
#else
static struct wait_queue *lirc_wait_in = NULL;
#endif

#ifdef KERNEL_2_1
static spinlock_t lirc_lock = SPIN_LOCK_UNLOCKED;
#endif

static int io = LIRC_PORT;
static int irq = LIRC_IRQ;

static struct timeval lasttv = {0, 0};

static lirc_t rbuf[RBUF_LEN];
static int rbh, rbt;

static lirc_t wbuf[WBUF_LEN];

unsigned int freq = 38000;
unsigned int duty_cycle = 50;

#ifdef USE_RDTSC

/* This version does sub-microsecond timing using rdtsc instruction,
 * and does away with the fudged LIRC_SERIAL_TRANSMITTER_LATENCY
 * Implicitly i586 architecture...  - Steve
 */

/* When we use the rdtsc instruction to measure clocks, we keep the
 * pulse and space widths as clock cycles.  As this is CPU speed
 * dependent, the widths must be calculated in init_port and ioctl
 * time
 */

unsigned long period = 0;
unsigned long pulse_width = 0;
unsigned long space_width = 0;

/* So send_pulse can quickly convert microseconds to clocks */
unsigned long conv_us_to_clocks = 0;

#else /* USE_RDTSC */

/* Version using udelay() */

/* period, pulse/space width are kept with 8 binary places -
   IE multiplied by 256. */

unsigned long period = 6736; /* 256*1000000L/freq; */
unsigned long pulse_width = 3368; /* (period*duty_cycle/100) */
unsigned long space_width = 3368; /* (period - pulse_width) */

#if defined(__i386__)
/*
  From:
  Linux I/O port programming mini-HOWTO
  Author: Riku Saikkonen <Riku.Saikkonen@hut.fi>
  v, 28 December 1997
  
  [...]
  Actually, a port I/O instruction on most ports in the 0-0x3ff range
  takes almost exactly 1 microsecond, so if you're, for example, using
  the parallel port directly, just do additional inb()s from that port
  to delay.
  [...]
*/
/* transmitter latency 1.5625us 0x1.90 - this figure arrived at from
 * comment above plus trimming to match actual measured frequency.
 * This will be sensitive to cpu speed, though hopefully most of the 1.5us
 * is spent in the uart access.  Still - for reference test machine was a
 * 1.13GHz Athlon system - Steve
 */

/* changed from 400 to 450 as this works better on slower machines;
   faster machines will use the rdtsc code anyway */

#define LIRC_SERIAL_TRANSMITTER_LATENCY 450

#else

/* does anybody have information on other platforms ? */
/* 256 = 1<<8 */
#define LIRC_SERIAL_TRANSMITTER_LATENCY 256

#endif  /* __i386__ */

#endif /* USE_RDTSC */

static inline unsigned int sinp(int offset)
{
	return inb(io + offset);
}

static inline void soutp(int offset, int value)
{
	outb(value, io + offset);
}

void on(void)
{
	soutp(UART_MCR,hardware[type].on);
}
  
void off(void)
{
	soutp(UART_MCR,hardware[type].off);
}

#ifndef MAX_UDELAY_MS
#define MAX_UDELAY_US 5000
#else
#define MAX_UDELAY_US (MAX_UDELAY_MS*1000)
#endif

static inline void safe_udelay(unsigned long usecs)
{
	while(usecs>MAX_UDELAY_US)
	{
		udelay(MAX_UDELAY_US);
		usecs-=MAX_UDELAY_US;
	}
	udelay(usecs);
}

#ifdef USE_RDTSC

/* This is an overflow/precision juggle, complicated in that we can't
   do long long divide in the kernel */

void calc_pulse_lengths_in_clocks(void)
{
	unsigned long long loops_per_sec,work;

#ifdef LIRC_LOOPS_PER_JIFFY
	loops_per_sec=current_cpu_data.loops_per_jiffy;
	loops_per_sec*=HZ;
#else
	loops_per_sec=current_cpu_data.loops_per_sec;
#endif
	
	/* How many clocks in a microsecond?, avoiding long long divide */
	work=loops_per_sec;
	work*=4295;  /* 4295 = 2^32 / 1e6 */
	conv_us_to_clocks=(work>>32);
	
	/* Carrier period in clocks, approach good up to 32GHz clock,
           gets carrier frequency within 8Hz */
	period=loops_per_sec>>3;
	period/=(freq>>3);

	/* Derive pulse and space from the period */

	pulse_width = period*duty_cycle/100;
	space_width = period - pulse_width;
#ifdef DEBUG
#ifdef LIRC_LOOPS_PER_JIFFY
	printk(KERN_INFO LIRC_DRIVER_NAME
	       ": in calc_pulse_lengths_in_clocks, freq=%d, duty_cycle=%d, "
	       "clk/jiffy=%ld, pulse=%ld, space=%ld, conv_us_to_clocks=%ld\n",
	       freq, duty_cycle, current_cpu_data.loops_per_jiffy,
	       pulse_width, space_width, conv_us_to_clocks);
#else
	printk(KERN_INFO LIRC_DRIVER_NAME
	       ": in calc_pulse_lengths_in_clocks, freq=%d, duty_cycle=%d, "
	       "clk/sec=%ld, pulse=%ld, space=%ld, conv_us_to_clocks=%ld\n",
	       freq, duty_cycle, current_cpu_data.loops_per_sec,
	       pulse_width, space_width, conv_us_to_clocks);
#endif
#endif
}

#endif


/* return value: space length delta */

long send_pulse_irdeo(unsigned long length)
{
	long rawbits;
	int i;
	unsigned char output;
	unsigned char chunk,shifted;
	
	/* how many bits have to be sent ? */
	rawbits=length*1152/10000;
	if(duty_cycle>50) chunk=3;
	else chunk=1;
	for(i=0,output=0x7f;rawbits>0;rawbits-=3)
	{
		shifted=chunk<<(i*3);
		shifted>>=1;
		output&=(~shifted);
		i++;
		if(i==3)
		{
			soutp(UART_TX,output);
			while(!(sinp(UART_LSR) & UART_LSR_THRE));
			output=0x7f;
			i=0;
		}
	}
	if(i!=0)
	{
		soutp(UART_TX,output);
		while(!(sinp(UART_LSR) & UART_LSR_TEMT));
	}

	if(i==0)
	{
		return((-rawbits)*10000/1152);
	}
	else
	{
		return((3-i)*3*10000/1152+(-rawbits)*10000/1152);
	}
}

long send_pulse_homebrew(unsigned long length)
{
#       ifdef USE_RDTSC
	unsigned long target, start, now;
#       else
	unsigned long actual, target, d;
#       endif
	int flag;

	if(length<=0) return 0;
	if(softcarrier)
	{
#               ifdef USE_RDTSC
		
		/* Version that uses Pentium rdtsc instruction to
                   measure clocks */

		/* Get going quick as we can */
		rdtscl(start);on();
		/* Convert length from microseconds to clocks */
		length*=conv_us_to_clocks;
		/* And loop till time is up - flipping at right intervals */
		now=start;
		target=pulse_width;
		flag=1;
		while((now-start)<length)
		{
			/* Delay till flip time */
			do
			{
				rdtscl(now);
			}
			while ((now-start)<target);
			/* flip */
			if(flag)
			{
				rdtscl(now);off();
				target+=space_width;
			}
			else
			{
				rdtscl(now);on();
				target+=pulse_width;
			}
			flag=!flag;
		}
		rdtscl(now);
		return(((now-start)-length)/conv_us_to_clocks);
		
#               else /* ! USE_RDTSC */
		
		/* here we use fixed point arithmetic, with 8
		   fractional bits.  that gets us within 0.1% or so of
		   the right average frequency, albeit with some
		   jitter in pulse length - Steve */

		/* To match 8 fractional bits used for pulse/space
                   length */
		length<<=8;
	
		actual=target=0; flag=0;
		while(actual<length)
		{
			if(flag)
			{
				off();
				target+=space_width;
			}
			else
			{
				on();
				target+=pulse_width;
			}
			d=(target-actual-LIRC_SERIAL_TRANSMITTER_LATENCY+128)>>8;
			/* Note - we've checked in ioctl that the pulse/space
			   widths are big enough so that d is > 0 */
			udelay(d);
			actual+=(d<<8)+LIRC_SERIAL_TRANSMITTER_LATENCY;
			flag=!flag;
		}
		return((actual-length)>>8);
#               endif /* USE_RDTSC */
	}
	else
	{
		on();
		safe_udelay(length);
		return(0);
	}
}

void send_space_irdeo(long length)
{
	if(length<=0) return;
	safe_udelay(length);
}

void send_space_homebrew(long length)
{
        off();
	if(length<=0) return;
	safe_udelay(length);
}

static void inline rbwrite(lirc_t l)
{
	unsigned int nrbt;

	nrbt=(rbt+1) & (RBUF_LEN-1);
	if(nrbt==rbh)      /* no new signals will be accepted */
	{
#               ifdef DEBUG
		printk(KERN_WARNING  LIRC_DRIVER_NAME  ": Buffer overrun\n");
#               endif
		return;
	}
	rbuf[rbt]=l;
	rbt=nrbt;
}

static void inline frbwrite(lirc_t l)
{
	/* simple noise filter */
	static lirc_t pulse=0L,space=0L;
	static unsigned int ptr=0;
	
	if(ptr>0 && (l&PULSE_BIT))
	{
		pulse+=l&PULSE_MASK;
		if(pulse>250)
		{
			rbwrite(space);
			rbwrite(pulse|PULSE_BIT);
			ptr=0;
			pulse=0;
		}
		return;
	}
	if(!(l&PULSE_BIT))
	{
		if(ptr==0)
		{
			if(l>20000)
			{
				space=l;
				ptr++;
				return;
			}
		}
		else
		{
			if(l>20000)
			{
				space+=pulse;
				if(space>PULSE_MASK) space=PULSE_MASK;
				space+=l;
				if(space>PULSE_MASK) space=PULSE_MASK;
				pulse=0;
				return;
			}
			rbwrite(space);
			rbwrite(pulse|PULSE_BIT);
			ptr=0;
			pulse=0;
		}
	}
	rbwrite(l);
}

void irq_handler(int i, void *blah, struct pt_regs *regs)
{
	struct timeval tv;
	int status,counter,dcd;
	long deltv;
	lirc_t data;
	
	counter=0;
	do{
		counter++;
		status=sinp(UART_MSR);
		if(counter>RS_ISR_PASS_LIMIT)
		{
			printk(KERN_WARNING LIRC_DRIVER_NAME ": AIEEEE: "
			       "We're caught!\n");
			break;
		}
		if((status&hardware[type].signal_pin_change) && sense!=-1)
		{
			/* get current time */
			do_gettimeofday(&tv);
			
			/* New mode, written by Trent Piepho 
			   <xyzzy@u.washington.edu>. */
			
			/* The old format was not very portable.
			   We now use the type lirc_t to pass pulses
			   and spaces to user space.
			   
			   If PULSE_BIT is set a pulse has been
			   received, otherwise a space has been
			   received.  The driver needs to know if your
			   receiver is active high or active low, or
			   the space/pulse sense could be
			   inverted. The bits denoted by PULSE_MASK are
			   the length in microseconds. Lengths greater
			   than or equal to 16 seconds are clamped to
			   PULSE_MASK.  All other bits are unused.
			   This is a much simpler interface for user
			   programs, as well as eliminating "out of
			   phase" errors with space/pulse
			   autodetection. */

			/* calculate time since last interrupt in
			   microseconds */
			dcd=(status & hardware[type].signal_pin) ? 1:0;
			
			deltv=tv.tv_sec-lasttv.tv_sec;
			if(deltv>15) 
			{
#ifdef DEBUG
				printk(KERN_WARNING LIRC_DRIVER_NAME
				       ": AIEEEE: %d %d %lx %lx %lx %lx\n",
				       dcd,sense,
				       tv.tv_sec,lasttv.tv_sec,
				       tv.tv_usec,lasttv.tv_usec);
#endif
				data=PULSE_MASK; /* really long time */
				if(!(dcd^sense)) /* sanity check */
				{
				        /* detecting pulse while this
					   MUST be a space! */
				        sense=sense ? 0:1;
				}
			}
			else
			{
				data=(lirc_t) (deltv*1000000+
					       tv.tv_usec-
					       lasttv.tv_usec);
			};
			if(tv.tv_sec<lasttv.tv_sec ||
			   (tv.tv_sec==lasttv.tv_sec &&
			    tv.tv_usec<lasttv.tv_usec))
			{
				printk(KERN_WARNING LIRC_DRIVER_NAME
				       ": AIEEEE: your clock just jumped "
				       "backwards\n");
				printk(KERN_WARNING LIRC_DRIVER_NAME
				       ": %d %d %lx %lx %lx %lx\n",
				       dcd,sense,
				       tv.tv_sec,lasttv.tv_sec,
				       tv.tv_usec,lasttv.tv_usec);
				data=PULSE_MASK;
			}
			frbwrite(dcd^sense ? data : (data|PULSE_BIT));
			lasttv=tv;
			wake_up_interruptible(&lirc_wait_in);
		}
	} while(!(sinp(UART_IIR) & UART_IIR_NO_INT)); /* still pending ? */
}

#ifdef KERNEL_2_3
static DECLARE_WAIT_QUEUE_HEAD(power_supply_queue);
#else
static struct wait_queue *power_supply_queue = NULL;
#endif
#ifndef KERNEL_2_1
static struct timer_list power_supply_timer;

static void power_supply_up(unsigned long ignored)
{
        wake_up(&power_supply_queue);
}
#endif

static int init_port(void)
{
	unsigned long flags;

        /* Check io region*/
	
        if((check_region(io,8))==-EBUSY)
	{
#if 0
		/* this is the correct behaviour but many people have
                   the serial driver compiled into the kernel... */
		printk(KERN_ERR  LIRC_DRIVER_NAME  
		       ": port %04x already in use\n", io);
		return(-EBUSY);
#else
		printk(KERN_ERR LIRC_DRIVER_NAME  
		       ": port %04x already in use, proceeding anyway\n", io);
		printk(KERN_WARNING LIRC_DRIVER_NAME  
		       ": compile the serial port driver as module and\n");
		printk(KERN_WARNING LIRC_DRIVER_NAME  
		       ": make sure this module is loaded first\n");
		release_region(io,8);
#endif
	}
	
	/* Reserve io region. */
	request_region(io, 8, LIRC_DRIVER_NAME);
	
	save_flags(flags);cli();
	
	/* Set DLAB 0. */
	soutp(UART_LCR, sinp(UART_LCR) & (~UART_LCR_DLAB));
	
	/* First of all, disable all interrupts */
	soutp(UART_IER, sinp(UART_IER)&
	      (~(UART_IER_MSI|UART_IER_RLSI|UART_IER_THRI|UART_IER_RDI)));
	
	/* Clear registers. */
	sinp(UART_LSR);
	sinp(UART_RX);
	sinp(UART_IIR);
	sinp(UART_MSR);
	
	/* Set line for power source */
	soutp(UART_MCR, hardware[type].off);
	
	/* Clear registers again to be sure. */
	sinp(UART_LSR);
	sinp(UART_RX);
	sinp(UART_IIR);
	sinp(UART_MSR);

	switch(hardware[type].type)
	{
	case LIRC_IRDEO:
	case LIRC_IRDEO_REMOTE:
		/* setup port to 7N1 @ 115200 Baud */
		/* 7N1+start = 9 bits at 115200 ~ 3 bits at 38kHz */
		
		/* Set DLAB 1. */
		soutp(UART_LCR, sinp(UART_LCR) | UART_LCR_DLAB);
		/* Set divisor to 1 => 115200 Baud */
		soutp(UART_DLM,0);
		soutp(UART_DLL,1);
		/* Set DLAB 0 +  7N1 */
		soutp(UART_LCR,UART_LCR_WLEN7);
		/* THR interrupt already disabled at this point */
		break;
	default:
		break;
	}
	
	restore_flags(flags);
	
#       ifdef USE_RDTSC
	/* Initialize pulse/space widths */
	calc_pulse_lengths_in_clocks();
#       endif

	/* If pin is high, then this must be an active low receiver. */
	if(sense==-1)
	{
		/* wait 1 sec for the power supply */
		
#               ifdef KERNEL_2_1
		sleep_on_timeout(&power_supply_queue,HZ);
#               else
		init_timer(&power_supply_timer);
		power_supply_timer.expires=jiffies+HZ;
		power_supply_timer.data=(unsigned long) current;
		power_supply_timer.function=power_supply_up;
		add_timer(&power_supply_timer);
		sleep_on(&power_supply_queue);
		del_timer(&power_supply_timer);
#               endif
		
		sense=(sinp(UART_MSR) & hardware[type].signal_pin) ? 1:0;
		printk(KERN_INFO  LIRC_DRIVER_NAME  ": auto-detected active "
		       "%s receiver\n",sense ? "low":"high");
	}
	else
	{
		printk(KERN_INFO  LIRC_DRIVER_NAME  ": Manually using active "
		       "%s receiver\n",sense ? "low":"high");
	};
	
	return 0;
}

static int lirc_open(struct inode *ino, struct file *filep)
{
	int result;
	unsigned long flags;
	
#       ifdef KERNEL_2_1
	spin_lock(&lirc_lock);
#       endif
	if(MOD_IN_USE)
	{
#               ifdef KERNEL_2_1
		spin_unlock(&lirc_lock);
#               endif
		return -EBUSY;
	}
	
	/* initialize timestamp */
	do_gettimeofday(&lasttv);
	
	result=request_irq(irq,irq_handler,SA_INTERRUPT,LIRC_DRIVER_NAME,NULL);
	switch(result)
	{
	case -EBUSY:
		printk(KERN_ERR LIRC_DRIVER_NAME ": IRQ %d busy\n", irq);
#               ifdef KERNEL_2_1
		spin_unlock(&lirc_lock);
#               endif
		return -EBUSY;
	case -EINVAL:
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": Bad irq number or handler\n");
#               ifdef KERNEL_2_1
		spin_unlock(&lirc_lock);
#               endif
		return -EINVAL;
	default:
#               ifdef DEBUG
		printk(KERN_INFO LIRC_DRIVER_NAME
		       ": Interrupt %d, port %04x obtained\n", irq, io);
#               endif
		break;
	};

	/* finally enable interrupts. */
	save_flags(flags);cli();
	
	/* Set DLAB 0. */
	soutp(UART_LCR, sinp(UART_LCR) & (~UART_LCR_DLAB));
	
	soutp(UART_IER, sinp(UART_IER)|UART_IER_MSI);
	
	restore_flags(flags);
	
	/* Init read buffer pointers. */
	rbh = rbt = 0;
	
	MOD_INC_USE_COUNT;
#       ifdef KERNEL_2_1
	spin_unlock(&lirc_lock);
#       endif
	return 0;
}

#ifdef KERNEL_2_1
static int lirc_close(struct inode *node, struct file *file)
#else
static void lirc_close(struct inode *node, struct file *file)
#endif
{	unsigned long flags;
	
	save_flags(flags);cli();
	
	/* Set DLAB 0. */
	soutp(UART_LCR, sinp(UART_LCR) & (~UART_LCR_DLAB));
	
	/* First of all, disable all interrupts */
	soutp(UART_IER, sinp(UART_IER)&
	      (~(UART_IER_MSI|UART_IER_RLSI|UART_IER_THRI|UART_IER_RDI)));
	restore_flags(flags);
	
	free_irq(irq, NULL);
#       ifdef DEBUG
	printk(KERN_INFO  LIRC_DRIVER_NAME  ": freed IRQ %d\n", irq);
#       endif
	
	MOD_DEC_USE_COUNT;
	
#       ifdef KERNEL_2_1
	return 0;
#       endif
}

#ifdef KERNEL_2_1
static unsigned int lirc_poll(struct file *file, poll_table * wait)
{
	poll_wait(file, &lirc_wait_in, wait);
	if (rbh != rbt)
		return POLLIN | POLLRDNORM;
	return 0;
}
#else
static int lirc_select(struct inode *node, struct file *file,
		       int sel_type, select_table * wait)
{
	if (sel_type != SEL_IN)
		return 0;
	if (rbh != rbt)
		return 1;
	select_wait(&lirc_wait_in, wait);
	return 0;
}
#endif

#ifdef KERNEL_2_1
static ssize_t lirc_read(struct file *file, char *buf,
			 size_t count, loff_t * ppos)
#else
static int lirc_read(struct inode *node, struct file *file, char *buf,
		     int count)
#endif
{
	int n=0,retval=0;
#ifdef KERNEL_2_3
	DECLARE_WAITQUEUE(wait,current);
#else
	struct wait_queue wait={current,NULL};
#endif
	
	if(n%sizeof(lirc_t)) return(-EINVAL);
	
	add_wait_queue(&lirc_wait_in,&wait);
	current->state=TASK_INTERRUPTIBLE;
	while (n < count)
	{
		if (rbt != rbh) {
#                       ifdef KERNEL_2_1
			copy_to_user((void *) buf+n,
				     (void *) &rbuf[rbh],sizeof(lirc_t));
#                       else
			memcpy_tofs((void *) buf+n,
				    (void *) &rbuf[rbh],sizeof(lirc_t));
#                       endif
			rbh = (rbh + 1) & (RBUF_LEN - 1);
			n+=sizeof(lirc_t);
		} else {
			if (file->f_flags & O_NONBLOCK) {
				retval = -EAGAIN;
				break;
			}
#                       ifdef KERNEL_2_1
			if (signal_pending(current)) {
				retval = -ERESTARTSYS;
				break;
			}
#                       else
			if (current->signal & ~current->blocked) {
				retval = -EINTR;
				break;
			}
#                       endif
			schedule();
			current->state=TASK_INTERRUPTIBLE;
		}
	}
	remove_wait_queue(&lirc_wait_in,&wait);
	current->state=TASK_RUNNING;
	return (n ? n : retval);
}

#ifdef KERNEL_2_1
static ssize_t lirc_write(struct file *file, const char *buf,
			 size_t n, loff_t * ppos)
#else
static int lirc_write(struct inode *node, struct file *file, const char *buf,
                     int n)
#endif
{
	int retval,i,count;
	unsigned long flags;
	long delta=0;
	
	if(!(hardware[type].features&LIRC_CAN_SEND_PULSE))
	{
		return(-EBADF);
	}
	
	if(n%sizeof(lirc_t)) return(-EINVAL);
	retval=verify_area(VERIFY_READ,buf,n);
	if(retval) return(retval);
	count=n/sizeof(lirc_t);
	if(count>WBUF_LEN || count%2==0) return(-EINVAL);
#       ifdef KERNEL_2_1
	copy_from_user(wbuf,buf,n);
#       else
	memcpy_fromfs(wbuf,buf,n);
#       endif
	save_flags(flags);cli();
	if(hardware[type].type==LIRC_IRDEO)
	{
		/* DTR, RTS down */
		on();
	}
	for(i=0;i<count;i++)
	{
		if(i%2) hardware[type].send_space(wbuf[i]-delta);
		else delta=hardware[type].send_pulse(wbuf[i]);
	}
	off();
	restore_flags(flags);
	return(n);
}

static int lirc_ioctl(struct inode *node,struct file *filep,unsigned int cmd,
		      unsigned long arg)
{
        int result;
	unsigned long value;
	unsigned int ivalue;
	
	switch(cmd)
	{
	case LIRC_GET_FEATURES:
#               ifdef KERNEL_2_1
		result=put_user(hardware[type].features,
				(unsigned long *) arg);
		if(result) return(result); 
#               else
		result=verify_area(VERIFY_WRITE,(unsigned long*) arg,
				   sizeof(unsigned long));
		if(result) return(result);
		put_user(hardware[type].features,(unsigned long *) arg);
#               endif
		break;
		
	case LIRC_GET_SEND_MODE:
		if(!(hardware[type].features&LIRC_CAN_SEND_MASK))
		{
			return(-ENOIOCTLCMD);
		}
		
#               ifdef KERNEL_2_1
		result=put_user(LIRC_SEND2MODE
				(hardware[type].features&LIRC_CAN_SEND_MASK),
				(unsigned long *) arg);
		if(result) return(result); 
#               else
		result=verify_area(VERIFY_WRITE,(unsigned long *) arg,
				   sizeof(unsigned long));
		if(result) return(result);
		put_user(LIRC_SEND2MODE
			 (hardware[type].features&LIRC_CAN_SEND_MASK),
			 (unsigned long *) arg);
#               endif
		break;
		
	case LIRC_GET_REC_MODE:
		if(!(hardware[type].features&LIRC_CAN_REC_MASK))
		{
			return(-ENOIOCTLCMD);
		}
		
#               ifdef KERNEL_2_1
		result=put_user(LIRC_REC2MODE
				(hardware[type].features&LIRC_CAN_REC_MASK),
				(unsigned long *) arg);
		if(result) return(result); 
#               else
		result=verify_area(VERIFY_WRITE,(unsigned long *) arg,
				   sizeof(unsigned long));
		if(result) return(result);
		put_user(LIRC_REC2MODE
			 (hardware[type].features&LIRC_CAN_REC_MASK),
			 (unsigned long *) arg);
#               endif
		break;
		
	case LIRC_SET_SEND_MODE:
		if(!(hardware[type].features&LIRC_CAN_SEND_MASK))
		{
			return(-ENOIOCTLCMD);
		}
		
#               ifdef KERNEL_2_1
		result=get_user(value,(unsigned long *) arg);
		if(result) return(result);
#               else
		result=verify_area(VERIFY_READ,(unsigned long *) arg,
				   sizeof(unsigned long));
		if(result) return(result);
		value=get_user((unsigned long *) arg);
#               endif
		/* only LIRC_MODE_PULSE supported */
		if(value!=LIRC_MODE_PULSE) return(-ENOSYS);
		break;
		
	case LIRC_SET_REC_MODE:
		if(!(hardware[type].features&LIRC_CAN_REC_MASK))
		{
			return(-ENOIOCTLCMD);
		}
		
#               ifdef KERNEL_2_1
		result=get_user(value,(unsigned long *) arg);
		if(result) return(result);
#               else
		result=verify_area(VERIFY_READ,(unsigned long *) arg,
				   sizeof(unsigned long));
		if(result) return(result);
		value=get_user((unsigned long *) arg);
#               endif
		/* only LIRC_MODE_MODE2 supported */
		if(value!=LIRC_MODE_MODE2) return(-ENOSYS);
		break;
		
	case LIRC_SET_SEND_DUTY_CYCLE:
		if(!(hardware[type].features&LIRC_CAN_SET_SEND_DUTY_CYCLE))
		{
			return(-ENOIOCTLCMD);
		}
		
#               ifdef KERNEL_2_1
		result=get_user(ivalue,(unsigned int *) arg);
		if(result) return(result);
#               else
		result=verify_area(VERIFY_READ,(unsigned int *) arg,
				   sizeof(unsigned int));
		if(result) return(result);
		ivalue=get_user((unsigned int *) arg);
#               endif
		if(ivalue<=0 || ivalue>100) return(-EINVAL);
#               ifdef USE_RDTSC
		duty_cycle=ivalue;
		calc_pulse_lengths_in_clocks();
#               else /* ! USE_RDTSC */
		if(256*1000000L/freq*ivalue/100<=
		   LIRC_SERIAL_TRANSMITTER_LATENCY) return(-EINVAL);
		if(256*1000000L/freq*(100-ivalue)/100<=
		   LIRC_SERIAL_TRANSMITTER_LATENCY) return(-EINVAL);
		duty_cycle=ivalue;
		period=256*1000000L/freq;
		pulse_width=period*duty_cycle/100;
		space_width=period-pulse_width;
#               endif /* USE_RDTSC */
#               ifdef DEBUG
		printk(KERN_WARNING LIRC_DRIVER_NAME
		       ": after SET_SEND_DUTY_CYCLE, freq=%d pulse=%ld, "
		       "space=%ld, conv_us_to_clocks=%ld\n",
		       freq, pulse_width, space_width, conv_us_to_clocks);
#               endif
		break;
		
	case LIRC_SET_SEND_CARRIER:
		if(!(hardware[type].features&LIRC_CAN_SET_SEND_CARRIER))
		{
			return(-ENOIOCTLCMD);
		}
		
#               ifdef KERNEL_2_1
		result=get_user(ivalue,(unsigned int *) arg);
		if(result) return(result);
#               else
		result=verify_area(VERIFY_READ,(unsigned int *) arg,
				   sizeof(unsigned int));
		if(result) return(result);
		ivalue=get_user((unsigned int *) arg);
#               endif
		if(ivalue>500000 || ivalue<20000) return(-EINVAL);
#               ifdef USE_RDTSC
		freq=ivalue;
		calc_pulse_lengths_in_clocks();
#               else /* !USE_RDTSC */
		if(256*1000000L/freq*ivalue/100<=
		   LIRC_SERIAL_TRANSMITTER_LATENCY) return(-EINVAL);
		if(256*1000000L/freq*(100-ivalue)/100<=
		   LIRC_SERIAL_TRANSMITTER_LATENCY) return(-EINVAL);
		freq=ivalue;
		period=256*1000000L/freq;
		pulse_width=period*duty_cycle/100;
		space_width=period-pulse_width;
#               endif /* USE_RDTSC */
#               ifdef DEBUG
		printk(KERN_WARNING LIRC_DRIVER_NAME
		       ": after SET_SEND_CARRIER, freq=%d pulse=%ld, "
		       "space=%ld, conv_us_to_clocks=%ld\n",
		       freq, pulse_width, space_width, conv_us_to_clocks);
#               endif
		break;
		
	default:
		return(-ENOIOCTLCMD);
	}
	return(0);
}

static struct file_operations lirc_fops =
{
	read:    lirc_read,
	write:   lirc_write,
#       ifdef KERNEL_2_1
	poll:    lirc_poll,
#       else
	select:  lirc_select,
#       endif
	ioctl:   lirc_ioctl,
	open:    lirc_open,
	release: lirc_close
};

#ifdef MODULE

#if LINUX_VERSION_CODE >= 0x020100
MODULE_AUTHOR("Ralph Metzler, Trent Piepho, Ben Pfaff, Christoph Bartelmus");
MODULE_DESCRIPTION("Infra-red receiver driver for serial ports.");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

MODULE_PARM(type, "i");
MODULE_PARM_DESC(type, "Hardware type (0 = home-brew, 1 = IRdeo,"
		 " 2 = IRdeo Remote, 3 = AnimaX");

MODULE_PARM(io, "i");
MODULE_PARM_DESC(io, "I/O address base (0x3f8 or 0x2f8)");

MODULE_PARM(irq, "i");
MODULE_PARM_DESC(irq, "Interrupt (4 or 3)");

MODULE_PARM(sense, "i");
MODULE_PARM_DESC(sense, "Override autodetection of IR receiver circuit"
		 " (0 = active high, 1 = active low )");

MODULE_PARM(softcarrier, "i");
MODULE_PARM_DESC(softcarrier, "Software carrier (0 = off, 1 = on)");


EXPORT_NO_SYMBOLS;
#endif

int init_module(void)
{
	int result;
	
	switch(type)
	{
	case LIRC_HOMEBREW:
	case LIRC_IRDEO:
	case LIRC_IRDEO_REMOTE:
	case LIRC_ANIMAX:
		break;
	default:
		return(-EINVAL);
	}
	if(!softcarrier && hardware[type].type==LIRC_HOMEBREW)
	{
		hardware[type].features&=~(LIRC_CAN_SET_SEND_DUTY_CYCLE|
					   LIRC_CAN_SET_SEND_CARRIER);
	}
	if ((result = init_port()) < 0)
		return result;
	if (register_chrdev(major, LIRC_DRIVER_NAME, &lirc_fops) < 0) {
		printk(KERN_ERR  LIRC_DRIVER_NAME  
		       ": register_chrdev failed!\n");
		release_region(io, 8);
		return -EIO;
	}
	return 0;
}

void cleanup_module(void)
{
	release_region(io, 8);
	unregister_chrdev(major, LIRC_DRIVER_NAME);
#       ifdef DEBUG
	printk(KERN_INFO  LIRC_DRIVER_NAME  ": cleaned up module\n");
#       endif
}

#endif

/*      $Id: lirc_serial.c,v 5.1 1999/05/05 14:57:55 columbus Exp $      */

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
 * Copyright (C) 1999 Christoph Bartelmus <columbus@hit.handshake.de>
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
 
#include <linux/version.h>
#if LINUX_VERSION_CODE >= 0x020100
#define KERNEL_2_1
#endif
#include <linux/module.h>

#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/config.h>
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

#define LIRC_DRIVER_NAME "lirc_serial"

#define RS_ISR_PASS_LIMIT 256

/* A long pulse code from a remote might take upto 300 bytes.  The
   daemon should read the bytes as soon as they are generated, so take
   the number of keys you think you can push before the daemon runs
   and multiply by 300.  The driver will warn you if you overrun this
   buffer.  If you have a slow computer or non-busmastering IDE disks,
   maybe you will need to increase this.  */

/* This MUST be a power of two!  It has to be larger than 4 as well. */

#define RBUF_LEN 1024
#define WBUF_LEN 1024

static int major = LIRC_MAJOR;
static int sense = -1;   /* -1 = auto, 0 = active high, 1 = active low */

static struct wait_queue *lirc_wait_in = NULL;

static int port = LIRC_PORT;
static int irq = LIRC_IRQ;

static unsigned char rbuf[RBUF_LEN];
static int rbh, rbt;
#ifndef LIRC_SERIAL_ANIMAX
static unsigned char wbuf[WBUF_LEN];
unsigned long pulse_width = 13; /* pulse/space ratio of 50/50 */
unsigned space_width = 13;      /* 1000000/freq-pulse_width */
unsigned long freq = 38000;     /* modulation frequency */
#endif

#ifdef LIRC_SERIAL_ANIMAX
#define LIRC_OFF (UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2)
#else
#define LIRC_OFF (UART_MCR_RTS|UART_MCR_OUT2)
#define LIRC_ON  (LIRC_OFF|UART_MCR_DTR)
#endif

static inline unsigned int sinp(int offset)
{
	return inb(port + offset);
}

static inline void soutp(int offset, int value)
{
	outb(value, port + offset);
}

#ifndef LIRC_SERIAL_ANIMAX
void on(void)
{
	soutp(UART_MCR,LIRC_ON);
}
  
void off(void)
{
	soutp(UART_MCR,LIRC_OFF);
}

void send_pulse(unsigned long length)
{
#ifdef LIRC_SERIAL_SOFTCARRIER
	unsigned long k,delay;
	int flag;
#endif

	if(length==0) return;
#ifdef LIRC_SERIAL_SOFTCARRIER
	/* this won't give us the carrier frequency we really want
	   due to integer arithmetic, but we can accept this inaccuracy */

	for(k=flag=0;k<length;k+=delay,flag=!flag)
	{
		if(flag)
		{
			off();
			delay=space_width;
		}
		else
		{
			on();
			delay=pulse_width;
		}
		udelay(delay);
	}
#else
	on();
	udelay(length);
#endif
}

void send_space(unsigned long length)
{
	if(length==0) return;
	off();
	udelay(length);
}
#endif

static inline void rbwrite(unsigned long l)
{
	unsigned int nrbt;

	nrbt=(rbt+4) & (RBUF_LEN-1);
	if(nrbt==rbh)      /* no new signals will be accepted;
			      do NOT overwrite old signals because
			      part of it may already has been read
			      by the client */
	{
		printk(KERN_WARNING  LIRC_DRIVER_NAME  ": Buffer overrun\n");
		return;
	}
	*((unsigned long *) (rbuf+rbt)) = l;
	rbt=nrbt;
}

void irq_handler(int i, void *blah, struct pt_regs *regs)
{
	struct timeval tv;
	int status,counter,dcd;
	unsigned long deltv;
	static struct timeval lasttv = {0, 0};

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
		if(MOD_IN_USE && (status&UART_MSR_DDCD) && sense!=-1)
		{
			/* get current time */
			do_gettimeofday(&tv);
			
			/* New mode, written by Trent Piepho 
			   <xyzzy@u.washington.edu>. */
			
			/* This has a 4 byte packet format.
			   The MSB is a one byte value that is either 0,
			   meaning space, or 1, meaning pulse.
			   The driver needs to know if your receiver
			   is active high or active low, or the
			   space/pulse sense could be inverted. The
			   lower three bytes are the length in
			   microseconds. Lengths greater than or equal
			   to 16 seconds are clamped to 0xffffff.
			   This is a much simpler interface for user
			   programs, as well as eliminating "out of
			   phase" errors with space/pulse
			   autodetection. */

			/* calculate time since last interrupt in
			   microseconds */
			dcd=(status & UART_MSR_DCD) ? 1:0;
 
			deltv=tv.tv_sec-lasttv.tv_sec;
			if(deltv>15) 
			{
				deltv=0xFFFFFF;	/* really long time */
				if(!(dcd^sense)) /* sanity check */
				{
				        /* detecting pulse while this
					   MUST be a space! */
				        sense=sense ? 0:1;
					printk(KERN_WARNING LIRC_DRIVER_NAME 
					       ": AIEEEE: Damn autodetection "
					       "still is not working!\n");
				}
			}
			else
			{
				deltv=deltv*1000000+tv.tv_usec-lasttv.tv_usec;
			};
			rbwrite(dcd^sense ? deltv : (deltv | 0x1000000));
			lasttv=tv;
			wake_up_interruptible(&lirc_wait_in);
		}
	} while(!(sinp(UART_IIR) & UART_IIR_NO_INT)); /* still pending ? */
}

static struct wait_queue *power_supply_queue = NULL;
static struct timer_list power_supply_timer;

static void power_supply_up(unsigned long ignored)
{
        wake_up(&power_supply_queue);
}

static int init_port(void)
{
	int result;
	unsigned long flags;

        /* Check io region*/

        /* if((check_region(port,8))==-EBUSY)
	{
                printk(KERN_ERR  LIRC_DRIVER_NAME  ": port %04x already in use\n", port);
		return(-EBUSY);
	}*/

	/* Reserve io region. */
	request_region(port, 8, LIRC_DRIVER_NAME);

	printk(KERN_INFO  LIRC_DRIVER_NAME  ": Interrupt %d, port %04x "
	       "obtained\n", irq, port);

	save_flags(flags);cli();

	/* Init read buffer pointers. */
	rbh = rbt = 0;

	/* Set DLAB 0. */
	soutp(UART_LCR, sinp(UART_LCR) & (~UART_LCR_DLAB));

	/* Clear registers. */
	sinp(UART_LSR);
	sinp(UART_RX);
	sinp(UART_IIR);
	sinp(UART_MSR);

	/* Set line for power source */
	soutp(UART_MCR, LIRC_OFF);

	/* Clear registers again to be sure. */
	sinp(UART_LSR);
	sinp(UART_RX);
	sinp(UART_IIR);
	sinp(UART_MSR);
	restore_flags(flags);

	/* If DCD is high, then this must be an active low receiver. */
	if(sense==-1)
	{
		/* wait 1 sec for the power supply */

		init_timer(&power_supply_timer);
		power_supply_timer.expires=jiffies+HZ;
		power_supply_timer.data=(unsigned long) current;
		power_supply_timer.function=power_supply_up;
		add_timer(&power_supply_timer);
		sleep_on(&power_supply_queue);
		del_timer(&power_supply_timer);

		sense=(sinp(UART_MSR) & UART_MSR_DCD) ? 1:0;
		printk(KERN_INFO  LIRC_DRIVER_NAME  ": auto-detected active "
		       "%s receiver\n",sense ? "low":"high");
	}
	else
	{
		printk(KERN_INFO  LIRC_DRIVER_NAME  ": Manually using active "
		       "%s recevier\n",sense ? "low":"high");
	};

	result=request_irq(irq,irq_handler,SA_INTERRUPT,LIRC_DRIVER_NAME,NULL);
	switch(result)
	{
	case -EBUSY:
		printk(KERN_ERR  LIRC_DRIVER_NAME  ": IRQ %d busy\n", irq);
		return -EBUSY;
	case -EINVAL:
		printk(KERN_ERR  LIRC_DRIVER_NAME  ": Bad irq number or handler\n");
		return -EINVAL;
	default:
		break;
	};

	/* finally enable interrupts. */

	save_flags(flags);cli();

	/* Set DLAB 0. */
	soutp(UART_LCR, sinp(UART_LCR) & (~UART_LCR_DLAB));

	soutp(UART_IER, sinp(UART_IER)|UART_IER_MSI);
	restore_flags(flags);

	return 0;
}

static int lirc_open(struct inode *ino, struct file *filep)
{
	if (MOD_IN_USE)
		return -EBUSY;
	MOD_INC_USE_COUNT;
	return 0;
}

#ifdef KERNEL_2_1
static int lirc_close(struct inode *node, struct file *file)
{
	MOD_DEC_USE_COUNT;
	return 0;
}
#else
static void lirc_close(struct inode *node, struct file *file)
{
	MOD_DEC_USE_COUNT;
}

#endif

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
	int n = 0, retval = 0;
	unsigned long flags;

	while (n < count)
	{
		save_flags(flags);cli();
		if (rbt != rbh) {
#ifdef KERNEL_2_1
			copy_to_user((void *) buf + n, (void *) (rbuf + rbh), 1);
#else
			memcpy_tofs((void *) buf + n, (void *) (rbuf + rbh), 1);
#endif
			rbh = (rbh + 1) & (RBUF_LEN - 1);
			n++;
			restore_flags(flags);
		} else {
			restore_flags(flags);
			if (file->f_flags & O_NONBLOCK) {
				retval = -EAGAIN;
				break;
			}
#ifdef KERNEL_2_1
			if (signal_pending(current)) {
				retval = -ERESTARTSYS;
				break;
			}
#else
			if (current->signal & ~current->blocked) {
				retval = -EINTR;
				break;
			}
#endif
			interruptible_sleep_on(&lirc_wait_in);
			current->state = TASK_RUNNING;
		}
	}
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
#ifndef LIRC_SERIAL_ANIMAX
	int retval,i,count;
	
	if(n%sizeof(unsigned long) || n>WBUF_LEN) return(-EINVAL);
	retval=verify_area(VERIFY_READ,buf,n);
	if(retval) return(retval);
	count=n/sizeof(unsigned long);
	if(count%2==0) return(-EINVAL);
#ifdef KERNEL_2_1
	copy_from_user(wbuf,buf,n);
#else
	memcpy_fromfs(wbuf,buf,n);
#endif
	for(i=0;i<count;i++)
	{
		if(i%2) send_space(*((unsigned long *) (wbuf+i*4)));
		else send_pulse(*((unsigned long *) (wbuf+i*4)));
	}
	off();
	return(n);
#else
	return(-EBADF);
#endif
}

static int lirc_ioctl(struct inode *node,struct file *filep,unsigned int cmd,
		      unsigned long arg)
{
        int result;
	unsigned long value;
	unsigned long features=
#       ifdef LIRC_SERIAL_SOFTCARRIER
	LIRC_CAN_SET_SEND_PULSE_WIDTH|
	LIRC_CAN_SET_SEND_CARRIER|
#       endif
#       ifndef LIRC_SERIAL_ANIMAX
	LIRC_CAN_SEND_PULSE|
#       endif
	LIRC_CAN_REC_MODE2;
	
	switch(cmd)
	{
	case LIRC_GET_FEATURES:
		result=put_user(features,(unsigned long *) arg);
		if(result) return(result); 
		break;
#       ifndef LIRC_SERIAL_ANIMAX
	case LIRC_GET_SEND_MODE:
		result=put_user(LIRC_MODE_PULSE,(unsigned long *) arg);
		if(result) return(result); 
		break;
#       endif
	case LIRC_GET_REC_MODE:
		result=put_user(LIRC_MODE_MODE2,(unsigned long *) arg);
		if(result) return(result); 
		break;
#       ifndef LIRC_SERIAL_ANIMAX
	case LIRC_SET_SEND_MODE:
		result=get_user(value,(unsigned long *) arg);
		if(result) return(result);
		if(value!=LIRC_MODE_PULSE) return(-EINVAL);
		break;
#       endif
	case LIRC_SET_REC_MODE:
		result=get_user(value,(unsigned long *) arg);
		if(result) return(result);
		if(value!=LIRC_MODE_MODE2) return(-EINVAL);
		break;
#       ifndef LIRC_SERIAL_ANIMAX
#       ifdef LIRC_SERIAL_SOFTCARRIER
	case LIRC_SET_SEND_PULSE_WIDTH:
		result=get_user(value,(unsigned long *) arg);
		if(result) return(result);
		if(value>=1000000/freq) return(-EINVAL);
		pulse_width=value;
		space_width=1000000/freq-pulse_width;
		break;
	case LIRC_SET_SEND_CARRIER:
		result=get_user(value,(unsigned long *) arg);
		if(result) return(result);
		if(value>500000 || value <30000) return(-EINVAL);
		freq=value;
		pulse_width=1000000/2/value;
		space_width=1000000/freq-pulse_width;
		break;
#       endif
#       endif
	default:
		return(-ENOIOCTLCMD);
	}
	return(0);
}

static struct file_operations lirc_fops =
{
	NULL,			/* lseek */
	lirc_read,		/* read */
	lirc_write,		/* write */
	NULL,			/* readdir */
#ifdef KERNEL_2_1
	lirc_poll,		/* poll */
#else
	lirc_select,		/* select */
#endif
	lirc_ioctl,		/* ioctl */
	NULL,			/* mmap  */
	lirc_open,		/* open */
#ifdef KERNEL_2_1
	NULL,
#endif
	lirc_close,		/* release */
	NULL,			/* fsync */
	NULL,			/* fasync */
	NULL,			/* check_media_change */
	NULL			/* revalidate */
};

#ifdef MODULE

#if LINUX_VERSION_CODE >= 0x020100
MODULE_AUTHOR("Ralph Metzler, Trent Piepho, Ben Pfaff, Christoph Bartelmus");
MODULE_DESCRIPTION("Infrared receiver driver for serial ports.");

MODULE_PARM(port, "i");
MODULE_PARM_DESC(port, "I/O address (0x3f8 or 0x2f8)");

MODULE_PARM(irq, "i");
MODULE_PARM_DESC(irq, "Interrupt (4 or 3)");

MODULE_PARM(sense, "i");
MODULE_PARM_DESC(sense, "Override autodetection of IR receiver circuit"
		 " (0 = active high, 1 = active low )");

EXPORT_NO_SYMBOLS;
#endif

int init_module(void)
{
	int result;

	if ((result = init_port()) < 0)
		return result;
	if (register_chrdev(major, LIRC_DRIVER_NAME, &lirc_fops) < 0) {
		printk(KERN_ERR  LIRC_DRIVER_NAME  ": register_chrdev failed!\n");
		free_irq(irq, NULL);
		printk(KERN_INFO  LIRC_DRIVER_NAME  ": freed IRQ %d\n", irq);
		release_region(port, 8);
		return -EIO;
	}
	return 0;
}

void cleanup_module(void)
{
	if (MOD_IN_USE)
		return;
	free_irq(irq, NULL);
	printk(KERN_INFO  LIRC_DRIVER_NAME  ": freed IRQ %d\n", irq);
	release_region(port, 8);
	unregister_chrdev(major, LIRC_DRIVER_NAME);
	printk(KERN_INFO  LIRC_DRIVER_NAME  ": cleaned up module\n");
}

#endif

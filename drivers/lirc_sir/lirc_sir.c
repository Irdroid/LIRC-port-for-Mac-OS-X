/*
 * LIRC SIR driver, (C) 2000 Milan Pikula <www@fornax.sk>
 *
 * lirc_sir - Device driver for use with SIR (serial infra red)
 * mode of IrDA on many notebooks.
 *
 */


#include <linux/version.h>
#if LINUX_VERSION_CODE >= 0x020100
#define KERNEL_2_1
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,3,0)
#define KERNEL_2_3
#endif
#else
#define KERNEL_2_0
#endif

#include <linux/module.h>

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
 
#include <linux/config.h>

#ifndef CONFIG_SERIAL_MODULE
#error "--- Please compile your Linux kernel serial port    ---"
#error "--- driver as a module. Read the LIRC documentation ---"
#error "--- for further details.                            ---"
#endif

#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/signal.h>
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

/* SECTION: Definitions */

#define RBUF_LEN 1024
#define WBUF_LEN 1024

#define LIRC_DRIVER_NAME "lirc_sir"
#define PULSE '['
#define TIME_CONST (9000000ul/115200ul)

static int major = LIRC_MAJOR;
static int iobase = LIRC_PORT;
static int irq = LIRC_IRQ;

#ifdef KERNEL_2_3
static DECLARE_WAIT_QUEUE_HEAD(lirc_read_queue);
#else
static struct wait_queue * lirc_read_queue = NULL;
#endif

static spinlock_t hardware_lock = SPIN_LOCK_UNLOCKED;
static spinlock_t dev_lock = SPIN_LOCK_UNLOCKED;

static lirc_t rx_buf[RBUF_LEN]; unsigned int rx_tail = 0, rx_head = 0;
static lirc_t tx_buf[WBUF_LEN];

/* SECTION: Prototypes */

	/* Communication with user-space */
static int lirc_open(struct inode * inode, struct file * file);
#ifdef KERNEL_2_1
static int lirc_close(struct inode * inode, struct file *file);
static unsigned int lirc_poll(struct file * file, poll_table * wait);
#else
static void lirc_close(struct inode * inode, struct file *file);
static int lirc_select(struct inode * inode, struct file * file,
		int type, select_table * wait);
#endif
static ssize_t lirc_read(struct file * file, char * buf, size_t count,
		loff_t * ppos);
static ssize_t lirc_write(struct file * file, const char * buf, size_t n, loff_t * pos);
static int lirc_ioctl(struct inode *node,struct file *filep,unsigned int cmd,
		unsigned long arg);
static void add_read_queue(int flag, unsigned long val);
#ifdef MODULE
static int init_chrdev(void);
static void drop_chrdev(void);
#endif
	/* Hardware */
static void sir_interrupt(int irq, void * dev_id, struct pt_regs * regs);
static void send_space(unsigned long len);
static void send_pulse(unsigned long len);
static int init_hardware(void);
static void drop_hardware(void);
	/* Initialisation */
static int init_port(void);
static void drop_port(void);
int init_module(void);
void cleanup_module(void);


/* SECTION: Communication with user-space */

static int lirc_open(struct inode * inode, struct file * file)
{
	spin_lock(&dev_lock);
	if (MOD_IN_USE) {
		spin_unlock(&dev_lock);
		return -EBUSY;
	}
	MOD_INC_USE_COUNT;
	spin_unlock(&dev_lock);
	return 0;
}

#ifdef KERNEL_2_1
static int lirc_close(struct inode * inode, struct file *file)
#else
static void lirc_close(struct inode * inode, struct file *file)
#endif
{
	MOD_DEC_USE_COUNT;
#ifdef KERNEL_2_1
	return 0;
#endif
}

#ifdef KERNEL_2_1
static unsigned int lirc_poll(struct file * file, poll_table * wait)
{
	poll_wait(file, &lirc_read_queue, wait);
	if (rx_head != rx_tail)
		return POLLIN | POLLRDNORM;
	return 0;
}
#else
static int lirc_select(struct inode * inode, struct file * file,
		int type, select_table * wait)
{
	if (type != SEL_IN)
		return 0;
	if (rx_head != rx_tail)
		return 1;
	select_wait(&lirc_read_queue, wait);
	return 0;
}
#endif

static ssize_t lirc_read(struct file * file, char * buf, size_t count,
		loff_t * ppos)
{
	int i = 0;
	int retval = 0;
	unsigned long flags;
	
	while (i < count) {
		save_flags(flags); cli();
		if (rx_head != rx_tail) {
			retval = verify_area(VERIFY_WRITE,
					(void *)buf + i,
					sizeof(lirc_t));
			if (retval) {
				restore_flags(flags);
				return retval;
			}
#ifdef KERNEL_2_1
			copy_to_user((void *)buf + i,
					(void *)(rx_buf + rx_head),
					sizeof(lirc_t));
#else
			memcpy_tofs((void *)buf + i,
					(void *)(rx_buf + rx_head),
					sizeof(lirc_t));
#endif
			rx_head = (rx_head + 1) & (RBUF_LEN - 1);
			i+=sizeof(lirc_t);
			restore_flags(flags);
		} else {
			restore_flags(flags);
			if (file->f_flags & O_NONBLOCK) {
				retval = -EAGAIN;
				break;
			}
			interruptible_sleep_on(&lirc_read_queue);
			if (signal_pending(current)) {
				retval = -ERESTARTSYS;
				break;
			}
		}
	}
	if (i)
		return i;
	return retval;
}
static ssize_t lirc_write(struct file * file, const char * buf, size_t n, loff_t * pos)
{
	int i;
	int retval;

        if(n%sizeof(lirc_t) || (n/sizeof(lirc_t)) > WBUF_LEN)
		return(-EINVAL);
	retval = verify_area(VERIFY_READ, buf, n);
	if (retval)
		return retval;
	copy_from_user(tx_buf, buf, n);
	i = 0;
	n/=sizeof(lirc_t);
	while (1) {
		if (i >= n)
			break;
		if (tx_buf[i])
			send_pulse(tx_buf[i]);
		i++;
		if (i >= n)
			break;
		if (tx_buf[i])
			send_space(tx_buf[i]);
		i++;
	}
	return n;
}

static int lirc_ioctl(struct inode *node,struct file *filep,unsigned int cmd,
		unsigned long arg)
{
	int retval = 0;
	unsigned long value = 0;

	if (cmd == LIRC_GET_FEATURES)
		value = LIRC_CAN_SEND_PULSE | LIRC_CAN_REC_MODE2;
	else if (cmd == LIRC_GET_SEND_MODE)
		value = LIRC_MODE_PULSE;
	else if (cmd == LIRC_GET_REC_MODE)
		value = LIRC_MODE_MODE2;

	switch (cmd) {
	case LIRC_GET_FEATURES:
	case LIRC_GET_SEND_MODE:
	case LIRC_GET_REC_MODE:
#ifdef KERNEL_2_0
		retval = verify_area(VERIFY_WRITE, (unsigned long *) arg,
			sizeof(unsigned long));
		if (retval)
			break;
#else
		retval =
#endif
		put_user(value, (unsigned long *) arg);
		break;

	case LIRC_SET_SEND_MODE:
	case LIRC_SET_REC_MODE:
#ifdef KERNEL_2_0
		retval = verify_area(VERIFY_READ, (unsigned long *) arg,
			sizeof(unsigned long));
		if (retval)
			break;
		value = get_user((unsigned long *) arg);
#else
		retval = get_user(value, (unsigned long *) arg);
#endif
		break;
	default:
		retval = -ENOIOCTLCMD;

	}
	
	if (retval)
		return retval;
	
	if (cmd == LIRC_SET_REC_MODE) {
		if (value != LIRC_MODE_MODE2)
			retval = -ENOSYS;
	} else if (cmd == LIRC_SET_SEND_MODE) {
		if (value != LIRC_MODE_PULSE)
			retval = -ENOSYS;
	}
	return retval;
}

static void add_read_queue(int flag, unsigned long val)
{
	unsigned int new_rx_tail;
	lirc_t newval;

	newval = val & PULSE_MASK;
	if (flag)
		newval |= PULSE_BIT;
	new_rx_tail = (rx_tail + 1) & (RBUF_LEN - 1);
	if (new_rx_tail == rx_head) {
		printk(KERN_WARNING LIRC_DRIVER_NAME ": Buffer overrun.\n");
		return;
	}
	rx_buf[rx_tail] = newval;
	rx_tail = new_rx_tail;
	wake_up_interruptible(&lirc_read_queue);
}

static struct file_operations lirc_fops =
{
	read:    lirc_read,
	write:   lirc_write,
#ifdef KERNEL_2_1
	poll:    lirc_poll,
#else
	select:  lirc_select,
#endif
	ioctl:   lirc_ioctl,
	open:    lirc_open,
	release: lirc_close,
};

#ifdef MODULE
static int init_chrdev(void)
{
	int retval;

	retval = register_chrdev(major, LIRC_DRIVER_NAME, &lirc_fops);
	if (retval < 0) {
		printk(KERN_ERR LIRC_DRIVER_NAME ": init_chrdev() failed.\n");
		return retval;
	}
	return 0;
}

static void drop_chrdev(void)
{
	unregister_chrdev(major, LIRC_DRIVER_NAME);
}
#endif

/* SECTION: Hardware */
static long delta(struct timeval * tv1, struct timeval * tv2)
{
	unsigned long deltv;
	
	deltv = tv2->tv_sec - tv1->tv_sec;
	if (deltv > 15)
		deltv = 0xFFFFFF;
	else
		deltv = deltv*1000000 +
			tv2->tv_usec -
			tv1->tv_usec;
	return deltv;
}

static void sir_interrupt(int irq, void * dev_id, struct pt_regs * regs)
{
	int iir, lsr;
	unsigned char data;
	struct timeval curr_tv;
	static struct timeval last_tv = {0, 0};
	unsigned long deltv;
	static struct timeval last_intr_tv = {0, 0};
	unsigned long deltintrtv;
	static int last_value = 0;

	while ((iir = inb(iobase + UART_IIR) & UART_IIR_ID)) {
		switch (iir) { /* FIXME toto treba preriedit */
		case UART_IIR_RLSI:
			break;
		case UART_IIR_RDI:
			while ((lsr = inb(iobase + UART_LSR))
				& UART_LSR_DR) { /* data ready */
				data = inb(iobase + UART_RX);
				do_gettimeofday(&curr_tv);
				deltv = delta(&last_tv, &curr_tv);
				deltintrtv = delta(&last_intr_tv, &curr_tv);
				/* if nothing came in last 2 cycles,
				   it was gap */
				if (deltintrtv > TIME_CONST * 2) {
					if (last_value) {
						add_read_queue(last_value,
							TIME_CONST);
						last_value = 0;
						deltv -= TIME_CONST;
						last_tv.tv_usec += TIME_CONST;
					}
				}
				data = (data == PULSE);
				if (data ^ last_value) {
					add_read_queue(last_value, deltv);
					last_value = data;
					last_tv = curr_tv;
				}
				last_intr_tv = curr_tv;
			}
			break;
		case UART_IIR_THRI:
			// if (lsr & UART_LSR_THRE) /* FIFO is empty */
			//	outb(data, iobase + UART_TX)
			break;
		default:
		}
	}
}

static void send_space(unsigned long len)
{
	udelay(len);
}

static void send_pulse(unsigned long len)
{
	long bytes_out = len / TIME_CONST;
	long time_left;

	if (!bytes_out)
		bytes_out++;
	time_left = (long)len - (long)bytes_out * (long)TIME_CONST;
	while (--bytes_out) {
		outb(PULSE, iobase + UART_TX);
		/* FIXME treba seriozne cakanie z drivers/char/serial.c */
		while (!(inb(iobase + UART_LSR) & UART_LSR_TEMT));
	}
#if 0
	if (time_left > 0)
		udelay(time_left);
#endif
}

static int init_hardware(void)
{
	int flags;

	spin_lock_irqsave(&hardware_lock, flags);
	/* reset UART */
	outb(0, iobase + UART_MCR);
	outb(0, iobase + UART_IER);
	/* init UART */
		/* set DLAB, speed = 115200 */
	outb(UART_LCR_DLAB | UART_LCR_WLEN7, iobase + UART_LCR);
	outb(1, iobase + UART_DLL); outb(0, iobase + UART_DLM);
		/* 7N1+start = 9 bits at 115200 ~ 3 bits at 44000 */
	outb(UART_LCR_WLEN7, iobase + UART_LCR);
		/* FIFO operation */
	outb(UART_FCR_ENABLE_FIFO, iobase + UART_FCR);
		/* interrupts */
	// outb(UART_IER_RLSI|UART_IER_RDI|UART_IER_THRI, iobase + UART_IER);	
	outb(UART_IER_RDI, iobase + UART_IER);	
	/* turn on UART */
	outb(UART_MCR_DTR|UART_MCR_RTS|UART_MCR_OUT2, iobase + UART_MCR);
	spin_unlock_irqrestore(&hardware_lock, flags);
	return 0;
}

static void drop_hardware(void)
{
	int flags;

	spin_lock_irqsave(&hardware_lock, flags);
	/* turn off interrupts */
	outb(0, iobase + UART_IER);	
	spin_unlock_irqrestore(&hardware_lock, flags);
}

/* SECTION: Initialisation */

static int init_port(void)
{
	int retval;

	/* get I/O port access and IRQ line */
	retval = check_region(iobase, 8);
	if (retval < 0) {
		printk(KERN_ERR LIRC_DRIVER_NAME
			": i/o port 0x%.4x already in use.\n",
			iobase);
		return retval;
	}
	retval = request_irq(irq, sir_interrupt, SA_INTERRUPT,
			LIRC_DRIVER_NAME, NULL);
	if (retval < 0) {
		printk(KERN_ERR LIRC_DRIVER_NAME
			": IRQ %d already in use.\n",
			irq);
		return retval;
	}
	request_region(iobase, 8, LIRC_DRIVER_NAME);
	printk(KERN_INFO LIRC_DRIVER_NAME
		": I/O port 0x%.4x, IRQ %d.\n",
		iobase, irq);

	return 0;
}

static void drop_port(void)
{
	free_irq(irq, NULL);
	release_region(iobase, 8);
}

int init_lirc_sir(void)
{
	int retval;

#ifdef KERNEL_2_3
	init_waitqueue_head(&lirc_read_queue);
#endif
	retval = init_port();
	if (retval < 0)
		return retval;
	init_hardware();
	printk(KERN_INFO LIRC_DRIVER_NAME
		": Installed.\n");
	return 0;
}

#ifdef MODULE

#ifdef KERNEL_2_1
MODULE_AUTHOR("Milan Pikula");
MODULE_DESCRIPTION("Infrared receiver driver for SIR type serial ports");
MODULE_PARM(iobase, "i");
MODULE_PARM_DESC(iobase, "I/O address base (0x3f8 or 0x2f8)");
MODULE_PARM(irq, "i");
MODULE_PARM_DESC(irq, "Interrupt (4 or 3)");

EXPORT_NO_SYMBOLS;
#endif

int init_module(void)
{
	int retval;

	retval = init_chrdev();
	if (retval < 0)
		return retval;
	retval = init_lirc_sir();
	if (retval) {
		drop_chrdev();
		return retval;
	}
	return 0;
}

void cleanup_module(void)
{
	drop_hardware();
	drop_chrdev();
	drop_port();
	printk(KERN_INFO LIRC_DRIVER_NAME ": Uninstalled.\n");
}
#endif


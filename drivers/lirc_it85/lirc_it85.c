/*
 * LIRC driver for ITE IT85xx CIR port (PNP ID ITE8709)
 *
 * Copyright (C) 2008 Grégory Lardière <spmf2004-lirc@yahoo.fr>
 * Copyright (C) 2010 Yan-Min Lin <yanmin067@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/pnp.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
#include <asm/io.h>
#else
#include <linux/io.h>
#endif

#include "drivers/kcompat.h"
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 35)
#include <media/lirc.h>
#include <media/lirc_dev.h>
#else
#include "drivers/lirc.h"
#include "drivers/lirc_dev/lirc_dev.h"
#endif

#define LIRC_DRIVER_NAME "lirc_it85"

#define BUF_CHUNK_SIZE	sizeof(lirc_t)
#define BUF_SIZE	(128*BUF_CHUNK_SIZE)

/* IT85 Register addresses and values (reverse-engineered) */
#define IT85_MODE		0x1a
#define IT85_REG_ADR		0x1b
#define IT85_REG_VAL		0x1c
#define IT85_IIR		0x1e  /* Interrupt identification register */
#define IT85_RFSR		0x1f  /* Receiver FIFO status register */
#define IT85_FIFO_START		0x20

#define IT85_MODE_READY		0x00
#define IT85_MODE_WRITE		0x01
#define IT85_MODE_READ		0x02

/*
 * IT8512 CIR-module registers addresses and values
 * (from IT8512 N specification v0.7.8.4)
 */
#define IT85_REG_DR		0x00  /* Data Read */
#define IT85_REG_MSTCR		0x01  /* Master control register */
#define IT85_REG_IER		0x02  /* Interrupt enable register */
#define IT85_REG_IIR		0x03  /* Interrupt identification register */
#define IT85_REG_RFSR		0x04  /* Receive FIFO status register */
#define IT85_REG_RCR		0x05  /* Receive control register */
#define IT85_REG_TCR		0x07
#define IT85_REG_BDLR		0x09  /* Baud rate divisor low byte register */
#define IT85_REG_BDHR		0x0a  /* Baud rate divisor high byte register */
#define IT85_REG_CFR		0x0c  /* Carrier frequency register */
#define IT85_REG_CSCRR		0x10

#define IT85_IIR_RDAI		0x02  /* Receiver data available interrupt */
#define IT85_IIR_RFOI		0x04  /* Receiver FIFO overrun interrupt */
#define IT85_RFSR_MASK		0x3f  /* FIFO byte count mask */
#define IT85_MSTCR_RESET	0x01  /* Reset registers to default value */
#define IT85_MSTCR_FIFOCLR	0x02  /* Clear FIFO */
#define IT85_MSTCR_FIFOTL_7	0x04  /* FIFO threshold level : 7 */
#define IT85_MSTCR_FIFOTL_25	0x0c  /* FIFO threshold level : 25 */
#define IT85_IER_RDAIE		0x02  /* Enable data interrupt request */
#define IT85_IER_RFOIE		0x04  /* Enable FIFO overrun interrupt req */
#define IT85_IER_IEC		0x80  /* Enable interrupt request */
#define IT85_CFR_CF_36KHZ	0x09  /* Carrier freq : low speed, 36kHz */
#define IT85_RCR_RXDCR_1	0x01  /* Demodulation carrier range : 1 */
#define IT85_RCR_RXACT		0x08  /* Receiver active */
#define IT85_RCR_RXEN		0x80  /* Receiver enable */
#define IT85_BDR_1		0x01  /* Baud rate divisor : 1 */
#define IT85_BDR_6		0x06  /* Baud rate divisor : 6 */

/* Actual values used by this driver */
#define CFG_FIFOTL	IT85_MSTCR_FIFOTL_25
#define CFG_CR_FREQ	IT85_CFR_CF_36KHZ
#define CFG_DCR		IT85_RCR_RXDCR_1
#define CFG_BDR		IT85_BDR_6
#define CFG_TIMEOUT	100000 /* Rearm interrupt when a space is > 100 ms */

static int debug;

struct it85_device {
	int use_count;
	int io;
	int irq;
	spinlock_t hardware_lock;
	__u64 acc_pulse;
	__u64 acc_space;
	char lastbit;
	struct timeval last_tv;
	struct lirc_driver driver;
	struct lirc_buffer buffer;
	struct tasklet_struct tasklet;
	char force_rearm;
	char rearmed;
	char device_busy;
	char *pnp_id;
};

#define dprintk(fmt, args...)					\
	do {							\
		if (debug)					\
			printk(KERN_DEBUG LIRC_DRIVER_NAME ": "	\
				fmt, ## args);			\
	} while (0)


static unsigned char it85_read(struct it85_device *dev,
					unsigned char port)
{
	outb(port, dev->io);
	return inb(dev->io+1);
}

static void it85_write(struct it85_device *dev, unsigned char port,
				unsigned char data)
{
	outb(port, dev->io);
	outb(data, dev->io+1);
}

static void it85_wait_device(struct it85_device *dev)
{
	int i = 0;
	/*
	 * loop until device tells it's ready to continue
	 * iterations count is usually ~750 but can sometimes achieve 13000
	 */
	for (i = 0; i < 15000; i++) {
		udelay(2);
		if (it85_read(dev, IT85_MODE) == IT85_MODE_READY)
			break;
	}
}

static void it85_write_register(struct it85_device *dev,
				unsigned char reg_adr, unsigned char reg_value)
{
	it85_wait_device(dev);

	it85_write(dev, IT85_REG_VAL, reg_value);
	it85_write(dev, IT85_REG_ADR, reg_adr);
	it85_write(dev, IT85_MODE, IT85_MODE_WRITE);
}

static void it85_init_hardware(struct it85_device *dev)
{
	spin_lock_irq(&dev->hardware_lock);
	dev->device_busy = 1;
	spin_unlock_irq(&dev->hardware_lock);

	it85_write_register(dev, IT85_REG_BDHR, (CFG_BDR >> 8) & 0xff);
	it85_write_register(dev, IT85_REG_BDLR, CFG_BDR & 0xff);
	it85_write_register(dev, IT85_REG_CFR, CFG_CR_FREQ);
	it85_write_register(dev, IT85_REG_IER,
			IT85_IER_IEC | IT85_IER_RFOIE | IT85_IER_RDAIE);
	it85_write_register(dev, IT85_REG_RCR, CFG_DCR);
	it85_write_register(dev, IT85_REG_MSTCR,
					CFG_FIFOTL | IT85_MSTCR_FIFOCLR);
	it85_write_register(dev, IT85_REG_RCR,
				IT85_RCR_RXEN | IT85_RCR_RXACT | CFG_DCR);

	spin_lock_irq(&dev->hardware_lock);
	dev->device_busy = 0;
	spin_unlock_irq(&dev->hardware_lock);

	tasklet_enable(&dev->tasklet);
}

static void it85_drop_hardware(struct it85_device *dev)
{
	tasklet_disable(&dev->tasklet);

	spin_lock_irq(&dev->hardware_lock);
	dev->device_busy = 1;
	spin_unlock_irq(&dev->hardware_lock);

	it85_write_register(dev, IT85_REG_RCR, 0);
	it85_write_register(dev, IT85_REG_MSTCR,
				IT85_MSTCR_RESET | IT85_MSTCR_FIFOCLR);

	spin_lock_irq(&dev->hardware_lock);
	dev->device_busy = 0;
	spin_unlock_irq(&dev->hardware_lock);
}

static int it85_set_use_inc(void *data)
{
	struct it85_device *dev;
	MOD_INC_USE_COUNT;
	dev = data;
	if (dev->use_count == 0)
		it85_init_hardware(dev);
	dev->use_count++;
	return 0;
}

static void it85_set_use_dec(void *data)
{
	struct it85_device *dev;
	MOD_DEC_USE_COUNT;
	dev = data;
	dev->use_count--;
	if (dev->use_count == 0)
		it85_drop_hardware(dev);
}

static void it85_add_read_queue(struct it85_device *dev, int flag,
					__u64 val)
{
	lirc_t value;

	dprintk("add a %llu usec %s\n", val, flag ? "pulse" : "space");

	value = (val > PULSE_MASK) ? PULSE_MASK : val;
	if (flag)
		value |= PULSE_BIT;

	if (!lirc_buffer_full(&dev->buffer)) {
		lirc_buffer_write(&dev->buffer, (void *) &value);
		wake_up(&dev->buffer.wait_poll);
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static irqreturn_t it85_interrupt(int irq, void *dev_id,
					struct pt_regs *regs)
#else
static irqreturn_t it85_interrupt(int irq, void *dev_id)
#endif
{
	unsigned char data;
	int iir, rfsr, i;
	int fifo = 0;
	char bit;
	struct timeval curr_tv;

	/* Bit duration in microseconds */
	const unsigned long bit_duration = 1000000ul / (115200 / CFG_BDR);

	struct it85_device *dev;
	dev = dev_id;

	/*
	 * If device is busy, we simply discard data because we are in one of
	 * these two cases : shutting down or rearming the device, so this
	 * doesn't really matter and this avoids waiting too long in IRQ ctx
	 */
	spin_lock(&dev->hardware_lock);
	if (dev->device_busy) {
		spin_unlock(&dev->hardware_lock);
		return IRQ_RETVAL(IRQ_HANDLED);
	}

	iir = it85_read(dev, IT85_IIR);

	switch (iir) {
	case IT85_IIR_RFOI:
		dprintk("fifo overrun, scheduling forced rearm just in case\n");
		dev->force_rearm = 1;
		tasklet_schedule(&dev->tasklet);
		spin_unlock(&dev->hardware_lock);
		return IRQ_RETVAL(IRQ_HANDLED);

	case IT85_IIR_RDAI:
		rfsr = it85_read(dev, IT85_RFSR);
		fifo = rfsr & IT85_RFSR_MASK;
		if (fifo > 32)
			fifo = 32;
		dprintk("iir: 0x%x rfsr: 0x%x fifo: %d\n", iir, rfsr, fifo);

		if (dev->rearmed) {
			do_gettimeofday(&curr_tv);
			dev->acc_space += 1000000ull
				* (curr_tv.tv_sec - dev->last_tv.tv_sec)
				+ (curr_tv.tv_usec - dev->last_tv.tv_usec);
			dev->rearmed = 0;
		}
		for (i = 0; i < fifo; i++) {
			data = it85_read(dev, i+IT85_FIFO_START);
			data = ~data;
			/* Loop through */
			for (bit = 0; bit < 8; ++bit) {
				if ((data >> bit) & 1) {
					dev->acc_pulse += bit_duration;
					if (dev->lastbit == 0) {
						it85_add_read_queue(dev, 0,
							dev->acc_space);
						dev->acc_space = 0;
					}
				} else {
					dev->acc_space += bit_duration;
					if (dev->lastbit == 1) {
						it85_add_read_queue(dev, 1,
							dev->acc_pulse);
						dev->acc_pulse = 0;
					}
				}
				dev->lastbit = (data >> bit) & 1;
			}
		}
		it85_write(dev, IT85_RFSR, 0);

		if (dev->acc_space > CFG_TIMEOUT) {
			dprintk("scheduling rearm IRQ\n");
			do_gettimeofday(&dev->last_tv);
			dev->force_rearm = 0;
			tasklet_schedule(&dev->tasklet);
		}

		spin_unlock(&dev->hardware_lock);
		return IRQ_RETVAL(IRQ_HANDLED);

	default:
		/* not our irq */
		dprintk("unknown IRQ (shouldn't happen) !!\n");
		spin_unlock(&dev->hardware_lock);
		return IRQ_RETVAL(IRQ_NONE);
	}
}

static void it85_rearm_irq(unsigned long data)
{
	struct it85_device *dev;
	unsigned long flags;
	dev = (struct it85_device *) data;

	spin_lock_irqsave(&dev->hardware_lock, flags);
	dev->device_busy = 1;
	spin_unlock_irqrestore(&dev->hardware_lock, flags);

	if (dev->force_rearm || dev->acc_space > CFG_TIMEOUT) {
		dprintk("rearming IRQ\n");
		it85_write_register(dev, IT85_REG_RCR,
						IT85_RCR_RXACT | CFG_DCR);
		it85_write_register(dev, IT85_REG_MSTCR,
					CFG_FIFOTL | IT85_MSTCR_FIFOCLR);
		it85_write_register(dev, IT85_REG_RCR,
				IT85_RCR_RXEN | IT85_RCR_RXACT | CFG_DCR);
		if (!dev->force_rearm)
			dev->rearmed = 1;
		dev->force_rearm = 0;
	}

	spin_lock_irqsave(&dev->hardware_lock, flags);
	dev->device_busy = 0;
	spin_unlock_irqrestore(&dev->hardware_lock, flags);
}

static int it85_cleanup(struct it85_device *dev, int stage, int errno,
				char *msg)
{
	if (msg != NULL)
		printk(KERN_ERR LIRC_DRIVER_NAME ": %s\n", msg);

	switch (stage) {
	case 6:
		if (dev->use_count > 0)
			it85_drop_hardware(dev);
	case 5:
		free_irq(dev->irq, dev);
	case 4:
		release_region(dev->io, 2);
	case 3:
		lirc_unregister_driver(dev->driver.minor);
	case 2:
		lirc_buffer_free(dev->driver.rbuf);
	case 1:
		kfree(dev);
	case 0:
		;
	}

	return errno;
}

static int __devinit it85_pnp_probe(struct pnp_dev *dev,
					const struct pnp_device_id *dev_id)
{
	struct lirc_driver *driver;
	struct it85_device *it85_dev;
	int ret;

	/* Check resources validity */
	if (!pnp_irq_valid(dev, 0))
		return it85_cleanup(NULL, 0, -ENODEV, "invalid IRQ");
	if (!pnp_port_valid(dev, 2))
		return it85_cleanup(NULL, 0, -ENODEV, "invalid IO port");

	/* Allocate memory for device struct */
	it85_dev = kzalloc(sizeof(struct it85_device), GFP_KERNEL);
	if (it85_dev == NULL)
		return it85_cleanup(NULL, 0, -ENOMEM, "kzalloc failed");
	pnp_set_drvdata(dev, it85_dev);

	/* Initialize device struct */
	it85_dev->use_count = 0;
	it85_dev->irq = pnp_irq(dev, 0);
	it85_dev->io = pnp_port_start(dev, 2);
	it85_dev->hardware_lock =
		__SPIN_LOCK_UNLOCKED(it85_dev->hardware_lock);
	it85_dev->acc_pulse = 0;
	it85_dev->acc_space = 0;
	it85_dev->lastbit = 0;
	do_gettimeofday(&it85_dev->last_tv);
	tasklet_init(&it85_dev->tasklet, it85_rearm_irq,
							(long) it85_dev);
	it85_dev->force_rearm = 0;
	it85_dev->rearmed = 0;
	it85_dev->device_busy = 0;

	it85_dev->pnp_id = &dev_id->id;

	/* Initialize driver struct */
	driver = &it85_dev->driver;
	strcpy(driver->name, LIRC_DRIVER_NAME);
	driver->minor = -1;
	driver->code_length = sizeof(lirc_t) * 8;
	driver->sample_rate = 0;
	driver->features = LIRC_CAN_REC_MODE2;
	driver->data = it85_dev;
	driver->add_to_buf = NULL;
#ifndef LIRC_REMOVE_DURING_EXPORT
	driver->get_queue = NULL;
#endif
	driver->rbuf = &it85_dev->buffer;
	driver->set_use_inc = it85_set_use_inc;
	driver->set_use_dec = it85_set_use_dec;
	driver->fops = NULL;
	driver->dev = &dev->dev;
	driver->owner = THIS_MODULE;

	/* Initialize LIRC buffer */
	if (lirc_buffer_init(driver->rbuf, BUF_CHUNK_SIZE, BUF_SIZE))
		return it85_cleanup(it85_dev, 1, -ENOMEM,
				       "lirc_buffer_init() failed");

	/* Register LIRC driver */
	ret = lirc_register_driver(driver);
	if (ret < 0)
		return it85_cleanup(it85_dev, 2, ret,
					"lirc_register_driver() failed");

	/* Reserve I/O port access */
	if (!request_region(it85_dev->io, 2, LIRC_DRIVER_NAME))
		return it85_cleanup(it85_dev, 3, -EBUSY,
						"i/o port already in use");

	/* Reserve IRQ line */
	ret = request_irq(it85_dev->irq, it85_interrupt, 0,
					LIRC_DRIVER_NAME, it85_dev);
	if (ret < 0)
		return it85_cleanup(it85_dev, 4, ret,
						"IRQ already in use");

	/* Initialize hardware */
	it85_drop_hardware(it85_dev); /* Shutdown hw until first use */

	printk(KERN_INFO LIRC_DRIVER_NAME ": device found : irq=%d io=0x%x\n",
					it85_dev->irq, it85_dev->io);

	return 0;
}

static void __devexit it85_pnp_remove(struct pnp_dev *dev)
{
	struct it85_device *it85_dev;
	it85_dev = pnp_get_drvdata(dev);

	it85_cleanup(it85_dev, 6, 0, NULL);

	printk(KERN_INFO LIRC_DRIVER_NAME ": device removed\n");
}

#ifdef CONFIG_PM
static int it85_pnp_suspend(struct pnp_dev *dev, pm_message_t state)
{
	struct it85_device *it85_dev;
	it85_dev = pnp_get_drvdata(dev);

	if (it85_dev->use_count > 0)
		it85_drop_hardware(it85_dev);

	return 0;
}

static int it85_pnp_resume(struct pnp_dev *dev)
{
	struct it85_device *it85_dev;
	it85_dev = pnp_get_drvdata(dev);

	if (it85_dev->use_count > 0)
		it85_init_hardware(it85_dev);

	return 0;
}
#else
#define it85_pnp_suspend NULL
#define it85_pnp_resume NULL
#endif

static const struct pnp_device_id pnp_dev_table[] = {
	{"ITE8709", 0},
	{}
};

MODULE_DEVICE_TABLE(pnp, pnp_dev_table);

static struct pnp_driver it85_pnp_driver = {
	.name           = LIRC_DRIVER_NAME,
	.probe          = it85_pnp_probe,
	.remove         = __devexit_p(it85_pnp_remove),
	.suspend        = it85_pnp_suspend,
	.resume         = it85_pnp_resume,
	.id_table       = pnp_dev_table,
};

int init_module(void)
{
	return pnp_register_driver(&it85_pnp_driver);
}

void cleanup_module(void)
{
	pnp_unregister_driver(&it85_pnp_driver);
}

MODULE_DESCRIPTION("LIRC driver for ITE IT85xx CIR port (PNP ID ITE8709)");
MODULE_AUTHOR("Grégory Lardière, Yan-Min Lin");
MODULE_LICENSE("GPL");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Enable debugging messages");
EXPORT_NO_SYMBOLS;

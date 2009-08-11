/*
 * driver for ENE KB3924 CIR (also known as ENE0100)
 *
 * Copyright (C) 2009 Maxim Levitsky <maximlevitsky@gmail.com>
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


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pnp.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include "lirc_ene0100.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 29)
#error "Sorry, this driver needs kernel version 2.6.29 or higher"
#else

static int sample_period = 50;
static int enable_idle = 1;

static void ene_set_idle(struct ene_device *dev, int idle);

/* read a hardware register */
static u8 ene_hw_read_reg(struct ene_device *dev, u16 reg)
{
	outb(reg >> 8   , dev->hw_io + ENE_ADDR_HI);
	outb(reg & 0xFF , dev->hw_io + ENE_ADDR_LO);
	return inb(dev->hw_io + ENE_IO);
}

/* write a hardware register */
static void ene_hw_write_reg(struct ene_device *dev, u16 reg, u8 value)
{
	outb(reg >> 8   , dev->hw_io + ENE_ADDR_HI);
	outb(reg & 0xFF , dev->hw_io + ENE_ADDR_LO);
	outb(value, dev->hw_io + ENE_IO);
}

/* change specific bits in hardware register */
static void ene_hw_write_reg_mask(struct ene_device *dev,
						u16 reg, u8 value, u8 mask)
{
	u8 regvalue;

	outb(reg >> 8   , dev->hw_io + ENE_ADDR_HI);
	outb(reg & 0xFF , dev->hw_io + ENE_ADDR_LO);

	regvalue = inb(dev->hw_io + ENE_IO) & ~mask;
	regvalue |= (value & mask);
	outb(regvalue, dev->hw_io + ENE_IO);
}


/* which half of hardware buffer we read now ?*/
static int hw_get_buf_pointer(struct ene_device *dev)
{
	return 4 * (ene_hw_read_reg(dev, ENE_FW_BUFFER_POINTER)
				& ENE_FW_BUFFER_POINTER_HIGH);
}


/* read irq status and ack it */
static int ene_hw_irq_status(struct ene_device *dev)
{
	u8 irq_status = ene_hw_read_reg(dev, ENE_IRQ_STATUS);

	if (!irq_status & ENE_IRQ_STATUS_IR)
		return 0;

	ene_hw_write_reg(dev, ENE_IRQ_STATUS, irq_status & ~ENE_IRQ_STATUS_IR);
	return 1;
}


/* hardware initialization */
static int ene_hw_init(void *data)
{
	struct ene_device *dev = (struct ene_device *)data;
	dev->in_use = 1;

	ene_hw_write_reg(dev, ENE_IRQ, dev->irq << 1);
	ene_hw_write_reg(dev, ENE_ADC_UNK2, 0x00);
	ene_hw_write_reg(dev, ENE_ADC_SAMPLE_PERIOD, sample_period);
	ene_hw_write_reg(dev, ENE_ADC_UNK1, 0x07);
	ene_hw_write_reg(dev, ENE_UNK1, 0x01);
	ene_hw_write_reg_mask(dev, ENE_FW_SETTINGS, ENE_FW_ENABLE | ENE_FW_IRQ,
		ENE_FW_ENABLE | ENE_FW_IRQ);

	/* ack any pending irqs - just in case */
	ene_hw_irq_status(dev);

	/* enter idle mode */
	ene_set_idle(dev, 1);

	/* clear stats */
	dev->sample = 0;
	return 0;
}

/* deinitialization */
static void ene_hw_deinit(void *data)
{
	struct ene_device *dev = (struct ene_device *)data;

	/* disable hardware IRQ and firmware flag */
	ene_hw_write_reg_mask(dev, ENE_FW_SETTINGS, 0,
		ENE_FW_ENABLE | ENE_FW_IRQ);

	ene_set_idle(dev, 1);
	dev->in_use = 0;
}

/*  sends current sample to userspace */
static void send_sample(struct ene_device *dev)
{
	int value = abs(dev->sample) & PULSE_MASK;

	if (dev->sample > 0)
		value |= PULSE_BIT;

	if (!lirc_buffer_full(dev->lirc_driver->rbuf)) {
		lirc_buffer_write(dev->lirc_driver->rbuf, (void *) &value);
		wake_up(&dev->lirc_driver->rbuf->wait_poll);
	}
	dev->sample = 0;
}

/*  this updates current sample */
static void update_sample(struct ene_device *dev, int sample)
{
	if (!dev->sample)
		dev->sample = sample;
	else if (same_sign(dev->sample, sample))
		dev->sample += sample;
	else {
		send_sample(dev);
		dev->sample = sample;
	}
}

/* enable or disable idle mode */
static void ene_set_idle(struct ene_device *dev, int idle)
{
	struct timeval now;

	ene_hw_write_reg_mask(dev, ENE_ADC_SAMPLE_PERIOD,
		idle & enable_idle ? 0 : ENE_ADC_SAMPLE_OVERFLOW,
		ENE_ADC_SAMPLE_OVERFLOW);

	dev->idle = idle;


	/* remember when we have entered the idle mode */
	if (idle) {
		do_gettimeofday(&dev->gap_start);
		return;
	}

	/* send the gap between keypresses now */
	do_gettimeofday(&now);

	if (now.tv_sec - dev->gap_start.tv_sec > 16)
		dev->sample = PULSE_MASK;
	else
		dev->sample -= 1000000ull * (now.tv_sec - dev->gap_start.tv_sec)
		+ (now.tv_usec - dev->gap_start.tv_usec);

	if (dev->sample > PULSE_MASK)
		dev->sample = PULSE_MASK;
	send_sample(dev);
}


/* interrupt handler */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 19)
static irqreturn_t ene_hw_irq(int irq, void *data)
#else
static irqreturn_t ene_hw_irq(int irq, void *data, struct pt_regs *regs)
#endif
{
	u16 hw_address;
	u8 hw_value;
	int i, hw_sample;
	int space;

	struct ene_device *dev = (struct ene_device *)data;

	if (!ene_hw_irq_status(dev))
		return IRQ_NONE;

	hw_address = ENE_SAMPLE_BUFFER + hw_get_buf_pointer(dev);

	for (i = 0 ; i < ENE_SAMPLES_SIZE ; i++) {

		hw_value = ene_hw_read_reg(dev, hw_address + i);
		space = hw_value & ENE_SAMPLE_LOW_MASK;
		hw_value &= ~ENE_SAMPLE_LOW_MASK;

		/* no more data */
		if (!(hw_value))
			break;

		/* calculate hw sample */
		hw_sample = hw_value * sample_period;

		if (space)
			hw_sample *= -1;

		/* overflow sample recieved, handle it */
		if (hw_value == ENE_SAMPLE_OVERFLOW) {

			if (dev->idle)
				continue;

			if (abs(dev->sample) <= ENE_MAXGAP)
				update_sample(dev, hw_sample);
			else
				ene_set_idle(dev, 1);

			continue;
		}

		/* normal first sample recieved*/
		if (dev->idle) {
			ene_set_idle(dev, 0);

			/* discard first recieved value, its random
			   since its the time signal was off before
			   first pulse if idle mode is enabled, HW
			   does that for us */

			if (!enable_idle)
				continue;
		}

		update_sample(dev, hw_sample);
		send_sample(dev);
	}
	return IRQ_HANDLED;
}

static int ene_probe(struct pnp_dev *pnp_dev,
					const struct pnp_device_id *dev_id)
{
	struct resource *res;
	struct ene_device *dev;
	struct lirc_driver *lirc_driver;
	int error = -ENOMEM;

	dev = kzalloc(sizeof(struct ene_device), GFP_KERNEL);

	if (!dev)
		goto err1;

	dev->pnp_dev = pnp_dev;
	pnp_set_drvdata(pnp_dev, dev);

	error = -EINVAL;
	if (sample_period < 5) {

		printk(KERN_ERR ENE_DRIVER_NAME ": sample period must be at "
		       "least 5 ms, (at least 30 recommended)\n");

		goto err1;
	}

	/* validate and read resources */
	error = -ENODEV;
	res = pnp_get_resource(pnp_dev, IORESOURCE_IO, 0);
	if (!pnp_resource_valid(res))
		goto err2;

	dev->hw_io = res->start;

	if (pnp_resource_len(res) < ENE_MAX_IO)
		goto err2;


	res = pnp_get_resource(pnp_dev, IORESOURCE_IRQ, 0);
	if (!pnp_resource_valid(res))
		goto err2;

	dev->irq = res->start;

	/* prepare lirc interface */
	error = -ENOMEM;
	lirc_driver = kzalloc(sizeof(struct lirc_driver), GFP_KERNEL);

	if (!lirc_driver)
		goto err2;

	dev->lirc_driver = lirc_driver;

	strcpy(lirc_driver->name, ENE_DRIVER_NAME);
	lirc_driver->minor = -1;
	lirc_driver->code_length = sizeof(int) * 8;
	lirc_driver->features = LIRC_CAN_REC_MODE2;
	lirc_driver->data = dev;
	lirc_driver->set_use_inc = ene_hw_init;
	lirc_driver->set_use_dec = ene_hw_deinit;
	lirc_driver->dev = &pnp_dev->dev;
	lirc_driver->owner = THIS_MODULE;

	lirc_driver->rbuf = kzalloc(sizeof(struct lirc_buffer), GFP_KERNEL);

	if (!lirc_driver->rbuf)
		goto err3;

	if (lirc_buffer_init(lirc_driver->rbuf,
					sizeof(int), sizeof(int) * 256))
		goto err4;

	error = -ENODEV;
	if (lirc_register_driver(lirc_driver))
		goto err5;

	/* claim the resources */
	error = -EBUSY;
	if (!request_region(dev->hw_io, ENE_MAX_IO, ENE_DRIVER_NAME))
		goto err6;

	if (request_irq(dev->irq, ene_hw_irq,
			IRQF_SHARED, ENE_DRIVER_NAME, (void *)dev))
		goto err7;


	/* check firmware version */
	error = -ENODEV;
	if (ene_hw_read_reg(dev, ENE_FW_VERSION) != ENE_FW_VER_SUPP) {
		printk(KERN_WARNING ENE_DRIVER_NAME ": "
		       "unsupported firmware found, aborting\n");
		goto err8;
	}

	printk(KERN_NOTICE ENE_DRIVER_NAME ": "
	       "driver has been succesfully loaded\n");
	return 0;

err8:
	free_irq(dev->irq, dev);
err7:
	release_region(dev->hw_io, ENE_MAX_IO);
err6:
	lirc_unregister_driver(lirc_driver->minor);
err5:
	lirc_buffer_free(lirc_driver->rbuf);
err4:
	kfree(lirc_driver->rbuf);
err3:
	kfree(lirc_driver);
err2:
	kfree(dev);
err1:
	return error;
}


static void ene_remove(struct pnp_dev *pnp_dev)
{
	struct ene_device *dev = pnp_get_drvdata(pnp_dev);
	ene_hw_deinit(dev);
	free_irq(dev->irq, dev);
	release_region(dev->hw_io, ENE_MAX_IO);
	lirc_unregister_driver(dev->lirc_driver->minor);
	lirc_buffer_free(dev->lirc_driver->rbuf);
	kfree(dev->lirc_driver);
	kfree(dev);
}


#ifdef CONFIG_PM
static int ene_suspend(struct pnp_dev *pnp_dev, pm_message_t state)
{
	struct ene_device *dev = pnp_get_drvdata(pnp_dev);
	ene_hw_write_reg_mask(dev, ENE_FW_SETTINGS, ENE_FW_WAKE, ENE_FW_WAKE);
	return 0;
}


static int ene_resume(struct pnp_dev *pnp_dev)
{
	struct ene_device *dev = pnp_get_drvdata(pnp_dev);
	if (dev->in_use)
		ene_hw_init(dev);

	ene_hw_write_reg_mask(dev, ENE_FW_SETTINGS, 0, ENE_FW_WAKE);
	return 0;
}

#endif


static const struct pnp_device_id ene_ids[] = {
	{ .id = "ENE0100", },
	{ },
};

static struct pnp_driver ene_driver = {
	.name = ENE_DRIVER_NAME,
	.id_table = ene_ids,
	.flags = PNP_DRIVER_RES_DO_NOT_CHANGE,

	.probe = ene_probe,
	.remove = __devexit_p(ene_remove),

#ifdef CONFIG_PM
	.suspend = ene_suspend,
	.resume = ene_resume,
#endif
};


static int __init ene_init(void)
{
	return pnp_register_driver(&ene_driver);
}

static void ene_exit(void)
{
	pnp_unregister_driver(&ene_driver);
}


module_param(sample_period, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(sample_period, "Hardware sample period (50 us default)");


module_param(enable_idle, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(enable_idle,
"Allow hardware to signal when IR pulse starts, disable if your remote"
"doesn't send a sync pulse");


MODULE_DEVICE_TABLE(pnp, ene_ids);
MODULE_DESCRIPTION("LIRC driver for KB3924/ENE0100 CIR port");
MODULE_AUTHOR("Maxim Levitsky");
MODULE_LICENSE("GPL");

module_init(ene_init);
module_exit(ene_exit);
#endif

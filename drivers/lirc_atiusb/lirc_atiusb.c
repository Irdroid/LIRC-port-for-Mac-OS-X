/* lirc_usb - USB remote support for LIRC
 * (currently only supports ATI Remote Wonder USB)
 * Version 0.2  [beta status]
 *
 * Copyright (C) 2003 Paul Miller <pmiller9@users.sourceforge.net>
 *
 * This driver was derived from:
 *   Vladimir Dergachev <volodya@minspring.com>'s 2002
 *      "USB ATI Remote support" (input device)
 *   Adrian Dewhurst <sailor-lk@sailorfrag.net>'s 2002
 *      "USB StreamZap remote driver" (LIRC)
 *   Artur Lipowski <alipowski@kki.net.pl>'s 2002
 *      "lirc_dev" and "lirc_gpio" LIRC modules
 *
 * $Id: lirc_atiusb.c,v 1.15 2004/01/18 04:38:26 pmiller9 Exp $
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 2, 4)
#error "*******************************************************"
#error "Sorry, this driver needs kernel version 2.2.4 or higher"
#error "*******************************************************"
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
#warn "**********************************************"
#warn "Beware, kernel 2.6 is not currently supported!"
#warn "**********************************************"
#define KERNEL26		1
#else
#define KERNEL26		0
#endif

#include <linux/config.h>

#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/sched.h>
#include <linux/wrapper.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <asm/semaphore.h>
#include <asm/uaccess.h>

#include "drivers/lirc.h"
#include "drivers/lirc_dev/lirc_dev.h"

#define DRIVER_VERSION		"0.1"
#define DRIVER_AUTHOR		"Paul Miller <pmiller9@users.sourceforge.net>"
#define DRIVER_DESC			"USB remote driver for LIRC"
#define DRIVER_NAME			"lirc_atiusb"

#define CODE_LENGTH			5
#define CODE_MIN_LENGTH		4
#define USB_BUFLEN			(CODE_LENGTH*4)

#ifdef CONFIG_USB_DEBUG
	static int debug = 1;
#else
	static int debug = 0;
#endif
#define dprintk				if (debug) printk


/* get hi and low bytes of a 16-bits int */
#define HI(a)				((unsigned char)((a) >> 8))
#define LO(a)				((unsigned char)((a) & 0xff))

/* lock irctl structure */
#define IRLOCK				down_interruptible(&ir->lock)
#define IRUNLOCK			up(&ir->lock)

/* general constants */
#define SUCCESS					0
#define SEND_FLAG_IN_PROGRESS	1
#define SEND_FLAG_COMPLETE		2

#if KERNEL26
#define FILL_INT_URB(URB,DEV,PIPE,TRANSFER_BUFFER,BUFFER_LENGTH,COMPLETE,CONTEXT,INTERVAL) \
		usb_fill_int_urb(URB,DEV,PIPE,TRANSFER_BUFFER,BUFFER_LENGTH,COMPLETE,CONTEXT,INTERVAL)
#endif

struct irctl {
	/* usb */
	struct usb_device *usbdev;
	struct urb irq, out;

	struct lirc_plugin *p;

	wait_queue_head_t wait_out;
#if KERNEL26
	unsigned char *buf_out;
	dma_addr_t buf_out_dma;
	unsigned char *buf;
	dma_addr_t buf_dma;
#else
	unsigned char buf_out[USB_BUFLEN];
	unsigned char buf[USB_BUFLEN];
#endif

	int devnum;
	int send_flags;

	int connected;

	struct semaphore lock;
};


static char init1[] = {0x01, 0x00, 0x20, 0x14};
static char init2[] = {0x01, 0x00, 0x20, 0x14, 0x20, 0x20, 0x20};


static void send_packet(struct irctl *ir, u16 cmd, unsigned char* data)
{
	DECLARE_WAITQUEUE(wait, current);
	int timeout = HZ; /* 1 second */

	dprintk(DRIVER_NAME "[%d]: send called (%#x)\n", ir->devnum, cmd);

	if (!ir->usbdev) {
		dprintk(DRIVER_NAME "[%d]: no usbdev, abort send_packet\n",
			ir->devnum);
		return;
	}

	IRLOCK;
	memcpy(ir->out.transfer_buffer + 1, data, LO(cmd));
	((unsigned char*)ir->out.transfer_buffer)[0] = HI(cmd);
	ir->out.transfer_buffer_length = LO(cmd) + 1;
	ir->out.dev = ir->usbdev;
	ir->send_flags = SEND_FLAG_IN_PROGRESS;

	set_current_state(TASK_INTERRUPTIBLE);
	add_wait_queue(&ir->wait_out, &wait);

#if KERNEL26
	if (usb_submit_urb(ir->out, SLAB_ATOMIC)) {
#else
	if (usb_submit_urb(&ir->out)) {
#endif
		set_current_state(TASK_RUNNING);
		remove_wait_queue(&ir->wait_out, &wait);
		IRUNLOCK;
		return;
	}
	IRUNLOCK;

	while (timeout && (ir->out.status == -EINPROGRESS)
	       && !(ir->send_flags & SEND_FLAG_COMPLETE)) {
		timeout = schedule_timeout(timeout);
		rmb();
	}

	set_current_state(TASK_RUNNING);
	remove_wait_queue(&ir->wait_out, &wait);

#if KERNEL26
	usb_unlink_urb(ir->out);
#else
	usb_unlink_urb(&ir->out);
#endif
}
static int unregister_from_lirc(struct irctl *ir)
{
	struct lirc_plugin *p = ir->p;
	int devnum = ir->devnum;
	int retval;

	dprintk(DRIVER_NAME "[%d]: unregister from lirc called\n", ir->devnum);

	if ((retval = lirc_unregister_plugin(p->minor)) > 0) {
		printk(DRIVER_NAME "[%d]: error in lirc_unregister_minor: %d\n"
		       "Trying again...\n", devnum, p->minor);
		if(retval==-EBUSY){
			printk(DRIVER_NAME
			       "[%d]: device is opened, will unregister"
			       " on close\n", devnum);
			return -EAGAIN;
		}
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ);

		if ((retval = lirc_unregister_plugin(p->minor)) > 0) {
			printk(DRIVER_NAME "[%d]: lirc_unregister failed\n",
			       devnum);
		}
	}
	if(retval != SUCCESS){
		printk(DRIVER_NAME "[%d]: didn't free resources\n",
		       devnum);
		return -EAGAIN;
	}
	lirc_buffer_free(p->rbuf);
	kfree(p->rbuf);
	kfree(p);
	kfree(ir);
	return 0;

}

static int set_use_inc(void *data)
{
	struct irctl *ir = data;

	if (!ir) {
		printk(DRIVER_NAME "[?]: set_use_inc called with no context\n");
		return -EIO;
	}
	dprintk(DRIVER_NAME "[%d]: set use inc\n", ir->devnum);

	if (!ir->connected) {
		if(!ir->usbdev)
			return -ENOENT;
		ir->irq.dev = ir->usbdev;
#if KERNEL26
		if (usb_submit_urb(ir->irq, SLAB_ATOMIC)) {
#else
		if (usb_submit_urb(&ir->irq)) {
#endif
			printk(DRIVER_NAME
				   "[%d]: open result = -E10 error submitting urb\n",
				   ir->devnum);
			return -EIO;
		}
		ir->connected = 1;
	}

	MOD_INC_USE_COUNT;
	return 0;
}

static void set_use_dec(void *data)
{
	struct irctl *ir = data;

	if (!ir) {
		printk(DRIVER_NAME "[?]: set_use_dec called with no context\n");
		return;
	}
	dprintk(DRIVER_NAME "[%d]: set use dec\n", ir->devnum);

	/* the device was unplugged while we where open */
	if(!ir->usbdev)
		unregister_from_lirc(ir);

	MOD_DEC_USE_COUNT;
}

#if KERNEL26
static void usb_remote_irq(struct urb *urb, struct pt_regs *regs)
#else
static void usb_remote_irq(struct urb *urb)
#endif
{
	struct irctl *ir = urb->context;
	char buf[CODE_LENGTH];
	int i, len;

	if (!ir) {
		printk(DRIVER_NAME "[?]: usb irq called with no context\n");
#if KERNEL26
		usb_unlink_urb(urb, SLAB_ATOMIC);
#else
		usb_unlink_urb(urb);
#endif
		return;
	}
	if (urb->status) return;

	dprintk(DRIVER_NAME "[%d]: data received (length %d)\n",
			ir->devnum, urb->actual_length);

	/* some remotes emit both 4 and 5 byte length codes. */
	len = urb->actual_length;
	if (len < CODE_MIN_LENGTH || len > CODE_LENGTH) return;

	memcpy(buf,urb->transfer_buffer,len);
	for (i = len; i < CODE_LENGTH; i++) buf[i] = 0;

	lirc_buffer_write_1(ir->p->rbuf, buf);
	wake_up(&ir->p->rbuf->wait_poll);
}

#if KERNEL26
static void usb_remote_out(struct urb *urb, struct pt_regs *regs)
#else
static void usb_remote_out(struct urb *urb)
#endif
{
	struct irctl *ir = urb->context;

	if (!ir) {
		printk(DRIVER_NAME "[?]: usb out called with no context\n");
		usb_unlink_urb(urb);
		return;
	}

	dprintk(DRIVER_NAME "[%d]: usb out called\n", ir->devnum);

	if (urb->status) return;

	ir->send_flags |= SEND_FLAG_COMPLETE;
	wmb();
	if (waitqueue_active(&ir->wait_out)) wake_up(&ir->wait_out);
}

#if KERNEL26
static int usb_remote_probe(struct usb_interface *iface,
			      const struct usb_device_id *id)
#else
static void *usb_remote_probe(struct usb_device *dev, unsigned int ifnum,
			      const struct usb_device_id *id)
#endif
{

#if KERNEL26
	struct usb_device *dev = interface_to_usbdev(iface);
#else
	struct usb_interface *iface;
#endif
	struct usb_interface_descriptor *interface;
	struct usb_endpoint_descriptor *endpoint, *epout;
	struct irctl *ir = NULL;
	struct lirc_plugin *plugin = NULL;
	struct lirc_buffer *rbuf = NULL;
	int pipe, devnum, maxp, len, buf_len, bytes_in_key;
	int minor = 0;
	unsigned long features;
	char buf[63], name[128]="";
	int mem_failure = 0;
#if KERNEL26
	int rtn = 0;
#endif

	dprintk(DRIVER_NAME ": usb probe called\n");

#if !KERNEL26
	iface = &dev->actconfig->interface[ifnum];
#endif
	interface = &iface->altsetting[iface->act_altsetting];

#if KERNEL26
	if (interface->desc.bNumEndpoints != 2) return -ENODEV;
	endpoint = &(interface->endpoint[0].desc);
	if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
	    != USB_DIR_IN)
		return -ENODEV;
	if ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
	    != USB_ENDPOINT_XFER_INT)
		return -ENODEV;
	epout = &(interface->endpoint[1].desc);
#else
	if (interface->bNumEndpoints != 2) return NULL;
	endpoint = interface->endpoint + 0;
	if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
	    != USB_DIR_IN)
		return NULL;
	if ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
	    != USB_ENDPOINT_XFER_INT)
		return NULL;
	epout = interface->endpoint + 1;
#endif

	pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);
	maxp = usb_maxpacket(dev, pipe, usb_pipeout(pipe));

	usb_set_idle(dev, interface->bInterfaceNumber, 0, 0);

	devnum = dev->devnum;

	features = LIRC_CAN_REC_LIRCCODE;
	bytes_in_key = CODE_LENGTH;

	len = (maxp > USB_BUFLEN) ? USB_BUFLEN : maxp;
	buf_len = len - (len % bytes_in_key);

	/* allocate memory */
	mem_failure = 0;
#if KERNEL26
	rtn = -ENOMEM;
#endif
	if (!(ir = kmalloc(sizeof(struct irctl), GFP_KERNEL))) {
		mem_failure = 1;
	} else if (!(plugin = kmalloc(sizeof(struct lirc_plugin), GFP_KERNEL))) {
		mem_failure = 2;
	} else if (!(rbuf = kmalloc(sizeof(struct lirc_buffer), GFP_KERNEL))) {
		mem_failure = 3;
	} else if (lirc_buffer_init(rbuf, bytes_in_key, buf_len/bytes_in_key)) {
		mem_failure = 4;
#if KERNEL26
	} else if (!(ir->buf = usb_buffer_alloc(dev, USB_BUFLEN, SLAB_ATOMIC, &ir->buf_dma))) {
		mem_failure = 5;
	} else if (!(ir->buf_out = usb_buffer_alloc(dev, USB_BUFLEN, SLAB_ATOMIC, &ir->buf_out_dma))) {
		mem_failure = 6;
	} else if (!(ir->irq = usb_alloc_urb(0, GFP_KERNEL))) {
		mem_failure = 7;
		rtn = -ENODEV;
	} else if (!(ir->out = usb_alloc_urb(0, GFP_KERNEL))) {
		mem_failure = 8;
		rtn = -ENODEV;
#endif
	} else {

		memset(ir, 0, sizeof(struct irctl));
		memset(plugin, 0, sizeof(struct lirc_plugin));

		strcpy(plugin->name, DRIVER_NAME " ");
		plugin->minor = -1;
		plugin->code_length = bytes_in_key*8;
		plugin->features = features;
		plugin->data = ir;
		plugin->rbuf = rbuf;
		plugin->set_use_inc = &set_use_inc;
		plugin->set_use_dec = &set_use_dec;

		ir->connected = 0;
		init_MUTEX(&ir->lock);
		init_waitqueue_head(&ir->wait_out);

		if ((minor = lirc_register_plugin(plugin)) < 0) {
			mem_failure = 9;
#if KERNEL26
			rtn = -ENODEV;
#endif
		}
	}

	/* free allocated memory incase of failure */
	switch (mem_failure) {
	case 9:
		lirc_buffer_free(rbuf);
#if KERNEL26
	case 8:
		usb_free_urb(ir->out);
	case 7:
		usb_free_urb(ir->irq);
	case 6:
		usb_buffer_free(dev, USB_BUFLEN, ir->buf, ir->buf_dma);
	case 5:
		usb_buffer_free(dev, USB_BUFLEN, ir->buf_out, ir->buf_out_dma);
#endif
	case 4:
		kfree(rbuf);
	case 3:
		kfree(plugin);
	case 2:
		kfree(ir);
	case 1:
		printk(DRIVER_NAME "[%d]: out of memory\n", devnum);
#if KERNEL26
		return rtn;
#else
		return NULL;
#endif
	}

	plugin->minor = minor;
	ir->p = plugin;
	ir->devnum = devnum;
	ir->usbdev = dev;

	FILL_INT_URB(&ir->irq, dev, pipe, ir->buf, buf_len,
		     usb_remote_irq, ir, endpoint->bInterval);
	FILL_INT_URB(&ir->out, dev,
		     usb_sndintpipe(dev, epout->bEndpointAddress), ir->buf_out,
		     USB_BUFLEN, usb_remote_out, ir, epout->bInterval);

	if (dev->descriptor.iManufacturer
	    && usb_string(dev, dev->descriptor.iManufacturer, buf, 63) > 0)
		strncpy(name, buf, 128);
	if (dev->descriptor.iProduct
	    && usb_string(dev, dev->descriptor.iProduct, buf, 63) > 0)
		snprintf(name, 128, "%s %s", name, buf);
	printk(DRIVER_NAME "[%d]: %s on usb%d:%d.%d\n", devnum, name,
	       dev->bus->busnum, dev->devnum, ifnum);
	dprintk(DRIVER_NAME "[%d]: maxp = %d, buf_len = %d\n", devnum,
		buf_len, maxp);

	send_packet(ir, 0x8004, init1);
	send_packet(ir, 0x8007, init2);

#if KERNEL26
	usb_set_intfdata(iface, ir);
	return 0;
#else
	return ir;
#endif
}

#if KERNEL26
static void usb_remote_disconnect(struct usb_interface *iface)
#else
static void usb_remote_disconnect(struct usb_device *dev, void *ptr)
#endif
{
#if KERNEL26
	struct irctl *ir = usb_get_intfdata(iface);
	struct usb_device *dev = interface_to_usbdev(iface);
#else
	struct irctl *ir = ptr;
#endif
	int devnum;

#if KERNEL26
	usb_set_intfdata(iface, NULL);
#endif

	if (!ir || !ir->p) {
		printk(DRIVER_NAME
		       "[?]: usb_remote_disconnect called with no context\n");
		return;
	}
	devnum = ir->devnum;

	dprintk(DRIVER_NAME "[%d]: usb_remote_disconnect called\n", devnum);

	ir->usbdev = NULL;

#warning is there a way for the readers to know that the game is over?
	/*wake_up_all(&ir->rbuf->wait_poll);*/
	wake_up_all(&ir->wait_out);

	IRLOCK;
#if KERNEL26
	usb_unlink_urb(ir->irq);
	usb_unlink_urb(ir->out);
	usb_free_urb(ir->irq);
	usb_free_urb(ir->out);
	usb_buffer_free(dev, USB_BUFLEN, ir->buf, ir->buf_dma);
	usb_buffer_free(dev, USB_BUFLEN, ir->buf_out, ir->buf_out_dma);
#else
	usb_unlink_urb(&ir->irq);
	usb_unlink_urb(&ir->out);
#endif
	IRUNLOCK;

	unregister_from_lirc(ir);

	printk(DRIVER_NAME "[%d]: usb remote disconnected\n", devnum);
}

static struct usb_device_id usb_remote_id_table [] = {
	{ USB_DEVICE(0x0bc7, 0x0002) },		/* X10 USB Firecracker Interface */
	{ USB_DEVICE(0x0bc7, 0x0003) },		/* X10 VGA Video Sender */
	{ USB_DEVICE(0x0bc7, 0x0004) },		/* ATI Wireless Remote Receiver */
	{ USB_DEVICE(0x0bc7, 0x0005) },		/* NVIDIA Wireless Remote Receiver */
	{ USB_DEVICE(0x0bc7, 0x0006) },		/* ATI Wireless Remote Receiver */
	{ USB_DEVICE(0x0bc7, 0x0007) },		/* X10 USB Wireless Transceiver */
	{ USB_DEVICE(0x0bc7, 0x0008) },		/* X10 USB Wireless Transceiver */
	{ USB_DEVICE(0x0bc7, 0x0009) },		/* X10 USB Wireless Transceiver */
	{ USB_DEVICE(0x0bc7, 0x000A) },		/* X10 USB Wireless Transceiver */
	{ USB_DEVICE(0x0bc7, 0x000B) },		/* X10 USB Transceiver */
	{ USB_DEVICE(0x0bc7, 0x000C) },		/* X10 USB Transceiver */
	{ USB_DEVICE(0x0bc7, 0x000D) },		/* X10 USB Transceiver */
	{ USB_DEVICE(0x0bc7, 0x000E) },		/* X10 USB Transceiver */
	{ USB_DEVICE(0x0bc7, 0x000F) },		/* X10 USB Transceiver */

	{ }						/* Terminating entry */
};

static struct usb_driver usb_remote_driver = {
	owner:		THIS_MODULE,
	name:		DRIVER_NAME,
	probe:		usb_remote_probe,
	disconnect:	usb_remote_disconnect,
	fops:		NULL,
	id_table:	usb_remote_id_table
};

static int __init usb_remote_init(void)
{
	int i;
	debug = 1;
	printk("\n" DRIVER_NAME ": " DRIVER_DESC " v" DRIVER_VERSION "\n");
	printk(DRIVER_NAME ": " DRIVER_AUTHOR "\n");
	dprintk(DRIVER_NAME ": debug mode enabled\n");

	i = usb_register(&usb_remote_driver);
	if (i < 0) {
		printk(DRIVER_NAME ": usb register failed, result = %d\n", i);
		return -1;
	}

	return SUCCESS;
}

static void __exit usb_remote_exit(void)
{
	usb_deregister(&usb_remote_driver);
}

module_init(usb_remote_init);
module_exit(usb_remote_exit);

MODULE_AUTHOR (DRIVER_AUTHOR);
MODULE_DESCRIPTION (DRIVER_DESC);
MODULE_LICENSE ("GPL");
MODULE_DEVICE_TABLE (usb, usb_remote_id_table);

MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "enable driver debug mode");

EXPORT_NO_SYMBOLS;

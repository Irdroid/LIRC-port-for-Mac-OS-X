/* lirc_usb - USB remote support for LIRC
 * (currently only supports ATI Remote Wonder USB)
 * Version 0.1  [pre-alpha status]
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
#define DRIVER_NAME			"lirc_usb"
#define DRIVER_LOG			DRIVER_NAME "  "

#define MAX_DEVICES			16
#define DRIVER_MAJOR		USB_MAJOR

#define BUFLEN				16

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



struct irctl {
	/* usb */
	struct usb_device *usbdev;
	struct urb irq, out;

	struct lirc_plugin *p;
	unsigned char buf[BUFLEN];

	wait_queue_head_t wait_out;
	unsigned char buf_out[BUFLEN];

	int devnum;
	int send_flags;

	int count;

	struct semaphore lock;
};


static char init1[] = {0x01, 0x00, 0x20, 0x14};
static char init2[] = {0x01, 0x00, 0x20, 0x14, 0x20, 0x20, 0x20};

static void usb_remote_disconnect(struct usb_device *dev, void *ptr);



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

	if (usb_submit_urb(&ir->out)) {
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

	usb_unlink_urb(&ir->out);
}
static int unregister_from_lirc(struct irctl *ir)
{
	struct lirc_plugin *p = ir->p;
	int devnum = ir->devnum;
	int retval;
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

	if (!ir->count) {
		if(!ir->usbdev)
			return -ENOENT;
		ir->irq.dev = ir->usbdev;
		if (usb_submit_urb(&ir->irq)){
			printk(DRIVER_NAME
				   "[%d]: open result = -E10 error submitting urb\n",
				   ir->devnum);
			return -EIO;
		}
	}
	ir->count++;

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

	if (ir->count) {
		ir->count--;
	} else {
		usb_remote_disconnect(NULL,ir);

		/* the device was unplugged while we where open */
		if(!ir->usbdev)
			unregister_from_lirc(ir);
	}

	MOD_DEC_USE_COUNT;
}

static void usb_remote_irq(struct urb *urb)
{
	struct irctl *ir = urb->context;

	if (!ir) {
		printk(DRIVER_NAME "[?]: usb irq called with no context\n");
		usb_unlink_urb(urb);
		return;
	}

	dprintk(DRIVER_NAME "[%d]: usb irq called\n", ir->devnum);

	if (urb->status) return;
	if (urb->actual_length != ir->p->code_length/8) return;

	lirc_buffer_write_1(ir->p->rbuf, urb->transfer_buffer);
	wake_up(&ir->p->rbuf->wait_poll);
}

static void usb_remote_out(struct urb *urb)
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

static void *usb_remote_probe(struct usb_device *dev, unsigned int ifnum,
			      const struct usb_device_id *id)
{
	struct usb_interface *iface;
	struct usb_interface_descriptor *interface;
	struct usb_endpoint_descriptor *endpoint, *epout;
	struct irctl *ir;
	struct lirc_plugin *plugin;
	struct lirc_buffer *rbuf;
	int pipe, minor, devnum, maxp, len, buf_len, bytes_in_key;
	unsigned long features;
	char buf[63], name[128]="";

	dprintk(DRIVER_NAME ": usb probe called\n");

	iface = &dev->actconfig->interface[ifnum];
	interface = &iface->altsetting[iface->act_altsetting];

	if (interface->bNumEndpoints != 2) return NULL;
	endpoint = interface->endpoint + 0;
	if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
	    != USB_DIR_IN)
		return NULL;
	if ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
	    != USB_ENDPOINT_XFER_INT)
		return NULL;
	epout = interface->endpoint + 1;

	pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);
	maxp = usb_maxpacket(dev, pipe, usb_pipeout(pipe));

	usb_set_idle(dev, interface->bInterfaceNumber, 0, 0);

	devnum = dev->devnum;

	features = LIRC_CAN_REC_LIRCCODE;
	bytes_in_key = 4;


	len = (maxp > BUFLEN) ? BUFLEN : maxp;
	buf_len = len - (len % bytes_in_key);

	if (!(ir = kmalloc(sizeof(struct irctl), GFP_KERNEL))) {
		printk(DRIVER_NAME "[%d]: out of memory\n", devnum);
		return NULL;
	}
	memset(ir, 0, sizeof(struct irctl));

	if (!(plugin = kmalloc(sizeof(struct lirc_plugin), GFP_KERNEL))) {
		printk(DRIVER_NAME "[%d]: out of memory\n", devnum);
		kfree(ir);
		return NULL;
	}
	memset(plugin, 0, sizeof(struct lirc_plugin));

	if (!(rbuf = kmalloc(sizeof(struct lirc_buffer), GFP_KERNEL))) {
		printk(DRIVER_NAME "[%d]: out of memory\n", devnum);
		kfree(ir);
		kfree(plugin);
		return NULL;
	}

	if (lirc_buffer_init(rbuf, sizeof(lirc_t), buf_len)) {
		printk(DRIVER_NAME "[%d]: out of memory\n", devnum);
		kfree(ir);
		kfree(plugin);
		kfree(rbuf);
		return NULL;
	}

	strcpy(plugin->name, DRIVER_LOG);
	plugin->minor = -1;
	plugin->code_length = bytes_in_key*8;
	plugin->features = features;
	plugin->data = ir;
	plugin->rbuf = rbuf;
	plugin->set_use_inc = &set_use_inc;
	plugin->set_use_dec = &set_use_dec;

	ir->count = 0;
	init_MUTEX(&ir->lock);
	init_waitqueue_head(&ir->wait_out);

	if ((minor = lirc_register_plugin(plugin)) < 0) {
		kfree(ir);
		kfree(plugin);
		lirc_buffer_free(rbuf);
		kfree(rbuf);
		return NULL;
	}

	plugin->minor = minor;
	ir->p = plugin;
	ir->devnum = devnum;
	ir->usbdev = dev;

	FILL_INT_URB(&ir->irq, dev, pipe, ir->buf, buf_len,
		     usb_remote_irq, ir, endpoint->bInterval);
	FILL_INT_URB(&ir->out, dev,
		     usb_sndintpipe(dev, epout->bEndpointAddress), ir->buf_out,
		     BUFLEN, usb_remote_out, ir, epout->bInterval);

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

	return ir;
}

static void usb_remote_disconnect(struct usb_device *dev, void *ptr)
{
	struct irctl *ir = ptr;
	int devnum;
	struct lirc_plugin *p;

	if (!ir || !ir->p) {
		printk(DRIVER_NAME
		       "[?]: usb_remote_disconnect called with no context\n");
		return;
	}

	devnum = ir->devnum;
	p = ir->p;

	ir->usbdev = NULL;

#warning is there a way for the readers to know that the game is over?
	/*wake_up_all(&ir->rbuf->wait_poll);*/
	wake_up_all(&ir->wait_out);

	IRLOCK;
	usb_unlink_urb(&ir->irq);
	usb_unlink_urb(&ir->out);
	IRUNLOCK;

	unregister_from_lirc(ir);

	printk(DRIVER_NAME "[%d]: usb remote disconnected\n",
	       ((struct irctl *)ptr)->devnum);
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

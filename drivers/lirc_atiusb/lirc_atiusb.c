/* lirc_atiusb - USB remote support for LIRC
 * (currently only supports X10 USB remotes)
 * (supports ATI Remote Wonder and ATI Remote Wonder II, too)
 *
 * Copyright (C) 2003-2004 Paul Miller <pmiller9@users.sourceforge.net>
 *
 * This driver was derived from:
 *   Vladimir Dergachev <volodya@minspring.com>'s 2002
 *      "USB ATI Remote support" (input device)
 *   Adrian Dewhurst <sailor-lk@sailorfrag.net>'s 2002
 *      "USB StreamZap remote driver" (LIRC)
 *   Artur Lipowski <alipowski@kki.net.pl>'s 2002
 *      "lirc_dev" and "lirc_gpio" LIRC modules
 *
 * $Id: lirc_atiusb.c,v 1.41 2004/10/29 02:02:01 pmiller9 Exp $
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
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 0)
#error "*******************************************************"
#error "Sorry, this driver needs kernel version 2.4.0 or higher"
#error "*******************************************************"
#endif

#include <linux/config.h>

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/smp_lock.h>
#include <linux/completion.h>
#include <asm/uaccess.h>
#include <linux/usb.h>
#include <linux/poll.h>
#include <linux/wait.h>

#include "drivers/lirc.h"
#include "drivers/kcompat.h"
#include "drivers/lirc_dev/lirc_dev.h"

#define DRIVER_VERSION		"0.4"
#define DRIVER_AUTHOR		"Paul Miller <pmiller9@users.sourceforge.net>"
#define DRIVER_DESC		"USB remote driver for LIRC"
#define DRIVER_NAME		"lirc_atiusb"

#define CODE_LENGTH		5
#define CODE_MIN_LENGTH		3
#define USB_BUFLEN		(CODE_LENGTH*4)

/* module parameters */
#ifdef CONFIG_USB_DEBUG
	static int debug = 1;
#else
	static int debug = 0;
#endif
#define dprintk(fmt, args...)                                 \
	do{                                                   \
		if(debug) printk(KERN_DEBUG fmt, ## args);    \
	}while(0)

static int mask = 0xFFFF;	// channel acceptance bit mask
static int unique = 0;		// enable channel-specific codes
static int repeat = 10;		// repeat time in 1/100 sec
static unsigned long repeat_jiffies; // repeat timeout

/* get hi and low bytes of a 16-bits int */
#define HI(a)			((unsigned char)((a) >> 8))
#define LO(a)			((unsigned char)((a) & 0xff))

/* lock irctl structure */
#define IRLOCK			down_interruptible(&ir->lock)
#define IRUNLOCK		up(&ir->lock)

/* general constants */
#define SUCCESS			0
#define SEND_FLAG_IN_PROGRESS	1
#define SEND_FLAG_COMPLETE	2

/* endpoints */
#define EP_KEYS			0
#define EP_MOUSE		1

#define VENDOR_ATI1		0x0bc7
#define VENDOR_ATI2		0x0471

static struct usb_device_id usb_remote_table [] = {
	{ USB_DEVICE(VENDOR_ATI1, 0x0002) },	/* X10 USB Firecracker Interface */
	{ USB_DEVICE(VENDOR_ATI1, 0x0003) },	/* X10 VGA Video Sender */
	{ USB_DEVICE(VENDOR_ATI1, 0x0004) },	/* ATI Wireless Remote Receiver */
	{ USB_DEVICE(VENDOR_ATI1, 0x0005) },	/* NVIDIA Wireless Remote Receiver */
	{ USB_DEVICE(VENDOR_ATI1, 0x0006) },	/* ATI Wireless Remote Receiver */
	{ USB_DEVICE(VENDOR_ATI1, 0x0007) },	/* X10 USB Wireless Transceiver */
	{ USB_DEVICE(VENDOR_ATI1, 0x0008) },	/* X10 USB Wireless Transceiver */
	{ USB_DEVICE(VENDOR_ATI1, 0x0009) },	/* X10 USB Wireless Transceiver */
	{ USB_DEVICE(VENDOR_ATI1, 0x000A) },	/* X10 USB Wireless Transceiver */
	{ USB_DEVICE(VENDOR_ATI1, 0x000B) },	/* X10 USB Transceiver */
	{ USB_DEVICE(VENDOR_ATI1, 0x000C) },	/* X10 USB Transceiver */
	{ USB_DEVICE(VENDOR_ATI1, 0x000D) },	/* X10 USB Transceiver */
	{ USB_DEVICE(VENDOR_ATI1, 0x000E) },	/* X10 USB Transceiver */
	{ USB_DEVICE(VENDOR_ATI1, 0x000F) },	/* X10 USB Transceiver */

	{ USB_DEVICE(VENDOR_ATI2, 0x0602) },	/* ATI Remote Wonder 2: Input Device */
	{ USB_DEVICE(VENDOR_ATI2, 0x0603) },	/* ATI Remote Wonder 2: Controller (???) */

	{ }					/* Terminating entry */
};






struct in_endpt {
	struct irctl *ir;
	struct urb *urb;
	struct usb_endpoint_descriptor *ep;
	int type;

	/* buffers and dma */
	unsigned char *buf;
	unsigned int len;
#ifdef KERNEL_2_5
	dma_addr_t dma;
#endif

	/* handle repeats */
	unsigned char old[CODE_LENGTH];
	unsigned long old_jiffies;
};

struct out_endpt {
	struct irctl *ir;
	struct urb *urb;
	struct usb_endpoint_descriptor *ep;

	/* buffers and dma */
	unsigned char *buf;
#ifdef KERNEL_2_5
	dma_addr_t dma;
#endif

	/* handle sending (init strings) */
	int send_flags;
	wait_queue_head_t wait;
};


/* data structure for each usb remote */
struct irctl {

	/* usb */
	struct usb_device *usbdev;
	struct in_endpt *in_keys, *in_mouse;
	struct out_endpt *out_init;
	int devnum;

	/* remote type based on usb_device_id tables */
	enum {
		ATI1_COMPATIBLE,
		ATI2_COMPATIBLE
	} remote_type;

	/* lirc */
	struct lirc_plugin *p;
	int connected;

	/* locking */
	struct semaphore lock;
};






/* init strings */
static char init1[] = {0x01, 0x00, 0x20, 0x14};
static char init2[] = {0x01, 0x00, 0x20, 0x14, 0x20, 0x20, 0x20};

/* send packet - used to initialize remote */
static void send_packet(struct out_endpt *oep, u16 cmd, unsigned char *data)
{
	struct irctl *ir = oep->ir;
	DECLARE_WAITQUEUE(wait, current);
	int timeout = HZ; /* 1 second */
	unsigned char buf[USB_BUFLEN];

	dprintk(DRIVER_NAME "[%d]: send called (%#x)\n", ir->devnum, cmd);

	IRLOCK;
	oep->urb->transfer_buffer_length = LO(cmd) + 1;
	oep->urb->dev = oep->ir->usbdev;
	oep->send_flags = SEND_FLAG_IN_PROGRESS;

	memcpy(buf+1, data, LO(cmd));
	buf[0] = HI(cmd);
	memcpy(oep->buf, buf, LO(cmd)+1);

	set_current_state(TASK_INTERRUPTIBLE);
	add_wait_queue(&oep->wait, &wait);

#ifdef KERNEL_2_5
	if (usb_submit_urb(oep->urb, SLAB_ATOMIC)) {
#else
	if (usb_submit_urb(oep->urb)) {
#endif
		set_current_state(TASK_RUNNING);
		remove_wait_queue(&oep->wait, &wait);
		IRUNLOCK;
		return;
	}
	IRUNLOCK;

	while (timeout && (oep->urb->status == -EINPROGRESS)
		&& !(oep->send_flags & SEND_FLAG_COMPLETE)) {
		timeout = schedule_timeout(timeout);
		rmb();
	}

	dprintk(DRIVER_NAME "[%d]: send complete (%#x)\n", ir->devnum, cmd);

	set_current_state(TASK_RUNNING);
	remove_wait_queue(&oep->wait, &wait);
	usb_unlink_urb(oep->urb);
}

static int unregister_from_lirc(struct irctl *ir)
{
	struct lirc_plugin *p = ir->p;
	int devnum;
	int rtn;

	devnum = ir->devnum;
	dprintk(DRIVER_NAME "[%d]: unregister from lirc called\n", devnum);

	if ((rtn = lirc_unregister_plugin(p->minor)) > 0) {
		printk(DRIVER_NAME "[%d]: error in lirc_unregister minor: %d\n"
			"Trying again...\n", devnum, p->minor);
		if (rtn == -EBUSY) {
			printk(DRIVER_NAME
				"[%d]: device is opened, will unregister"
				" on close\n", devnum);
			return -EAGAIN;
		}
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ);

		if ((rtn = lirc_unregister_plugin(p->minor)) > 0) {
			printk(DRIVER_NAME "[%d]: lirc_unregister failed\n",
			devnum);
		}
	}

	if (rtn != SUCCESS) {
		printk(DRIVER_NAME "[%d]: didn't free resources\n", devnum);
		return -EAGAIN;
	}

	printk(DRIVER_NAME "[%d]: usb remote disconnected\n", devnum);
	return SUCCESS;
}

static int set_use_inc(void *data)
{
	struct irctl *ir = data;

	if (!ir) {
		printk(DRIVER_NAME "[?]: set_use_inc called with no context\n");
		return -EIO;
	}
	dprintk(DRIVER_NAME "[%d]: set use inc\n", ir->devnum);

	MOD_INC_USE_COUNT;

	if (!ir->connected) {
		if (!ir->usbdev)
			return -ENOENT;
		if (ir->in_keys) {
			ir->in_keys->urb->dev = ir->usbdev;
#ifdef KERNEL_2_5
			if (usb_submit_urb(ir->in_keys->urb, SLAB_ATOMIC)) {
#else
			if (usb_submit_urb(ir->in_keys->urb)) {
#endif
				printk(DRIVER_NAME "[%d]: open result = -EIO error "
					"submitting urb\n", ir->devnum);
				MOD_DEC_USE_COUNT;
				return -EIO;
			}
		}
		if (ir->in_mouse) {
			ir->in_mouse->urb->dev = ir->usbdev;
#ifdef KERNEL_2_5
			if (usb_submit_urb(ir->in_mouse->urb, SLAB_ATOMIC)) {
#else
			if (usb_submit_urb(ir->in_mouse->urb)) {
#endif
				printk(DRIVER_NAME "[%d]: open result = -EIO error "
					"submitting urb\n", ir->devnum);
				MOD_DEC_USE_COUNT;
				return -EIO;
			}
		}
		ir->connected = 1;
	}

	return SUCCESS;
}

static void set_use_dec(void *data)
{
	struct irctl *ir = data;

	if (!ir) {
		printk(DRIVER_NAME "[?]: set_use_dec called with no context\n");
		return;
	}
	dprintk(DRIVER_NAME "[%d]: set use dec\n", ir->devnum);

	if (ir->connected) {
		IRLOCK;
		if (ir->in_keys) usb_unlink_urb(ir->in_keys->urb);
		if (ir->in_mouse) usb_unlink_urb(ir->in_mouse->urb);
		ir->connected = 0;
		IRUNLOCK;
	}
	MOD_DEC_USE_COUNT;
}

static void usb_remote_printdata(struct irctl *ir, char *buf, int len)
{
	char codes[USB_BUFLEN*3 + 1];
	int i;

	if (len <= 0)
		return;

	for (i = 0; i < len && i < USB_BUFLEN; i++) {
		snprintf(codes+i*3, 4, "%02x ", buf[i] & 0xFF);
	}
	printk(DRIVER_NAME "[%d]: data received %s (length=%d)\n",
		ir->devnum, codes, len);
}

static int code_check(struct in_endpt *iep, int len)
{
	struct irctl *ir = iep->ir;
	int i, chan;

	/* ATI RW1: some remotes emit both 4 and 5 byte length codes. */
	/* ATI RW2: emit 3 byte codes */
	if (len < CODE_MIN_LENGTH || len > CODE_LENGTH)
		return -1;

	switch (ir->remote_type) {

	case ATI1_COMPATIBLE:

		// *** channel not tested with 4/5-byte Dutch remotes ***
		chan = ((iep->buf[len-1]>>4) & 0x0F);

		/* strip channel code */
		if (!unique) {
			iep->buf[len-1] &= 0x0F;
			iep->buf[len-3] -= (chan<<4);
		}
		break;

	case ATI2_COMPATIBLE:
		chan = iep->buf[0];
		if (!unique) iep->buf[0] = 0;
		// ignore mouse navigation key
//		if ((iep->type == EP_KEYS) && (iep->buf[1] == MOUSE_CODE)) {
//			return -1;
//		}
		break;

	default:
		chan = 0;
	}

	if ( !((1U<<chan) & mask) ) {
		dprintk(DRIVER_NAME "[%d]: ignore channel %d\n", ir->devnum, chan+1);
		return -1;
	}
	dprintk(DRIVER_NAME "[%d]: accept channel %d\n", ir->devnum, chan+1);

	/* check for repeats */
	if (memcmp(iep->old, iep->buf, len) == 0) {
		if (iep->old_jiffies + repeat_jiffies > jiffies) {
			return -1;
		}
	} else {
		memcpy(iep->old, iep->buf, len);
		for (i = len; i < CODE_LENGTH; i++) iep->old[i] = 0;
	}
	iep->old_jiffies = jiffies;

	return SUCCESS;
}



#ifdef KERNEL_2_5
static void usb_remote_recv(struct urb *urb, struct pt_regs *regs)
#else
static void usb_remote_recv(struct urb *urb)
#endif
{
	struct in_endpt *iep;
	int len;

	if (!urb)
		return;
	if (!(iep = urb->context)) {
#ifdef KERNEL_2_5
		urb->transfer_flags |= URB_ASYNC_UNLINK;
#endif
		usb_unlink_urb(urb);
		return;
	}
	if (!iep->ir->usbdev)
		return;

	len = urb->actual_length;
	if (debug)
		usb_remote_printdata(iep->ir,urb->transfer_buffer,len);

	switch (urb->status) {

	/* success */
	case SUCCESS:

		if (code_check(iep, len) < 0)
			break;

		lirc_buffer_write_1(iep->ir->p->rbuf, iep->old);
		wake_up(&iep->ir->p->rbuf->wait_poll);
		break;

	/* unlink */
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
#ifdef KERNEL_2_5
		urb->transfer_flags |= URB_ASYNC_UNLINK;
#endif
		usb_unlink_urb(urb);
		return;

	case -EPIPE:
	default:
		break;
	}

	/* resubmit urb */
#ifdef KERNEL_2_5
	usb_submit_urb(urb, SLAB_ATOMIC);
#endif
}

#ifdef KERNEL_2_5
static void usb_remote_send(struct urb *urb, struct pt_regs *regs)
#else
static void usb_remote_send(struct urb *urb)
#endif
{
	struct out_endpt *oep;

	if (!urb)
		return;
	if (!(oep = urb->context)) {
		usb_unlink_urb(urb);
		return;
	}
	if (!oep->ir->usbdev)
		return;

	dprintk(DRIVER_NAME "[%d]: usb out called\n", oep->ir->devnum);

	if (urb->status)
		return;

	oep->send_flags |= SEND_FLAG_COMPLETE;
	wmb();
	if (waitqueue_active(&oep->wait))
		wake_up(&oep->wait);
}













static void free_in_endpt(struct in_endpt *iep, int mem_failure)
{
	struct irctl *ir;
	if (!iep) return;

	ir = iep->ir;
	IRLOCK;
	switch (mem_failure) {
	case 4:
		usb_unlink_urb(iep->urb);
		usb_free_urb(iep->urb);
#ifdef KERNEL_2_5
	case 3:
		usb_buffer_free(iep->ir->usbdev, iep->len, iep->buf, iep->dma);
#else
	case 3:
		kfree(iep->buf);
#endif
	case 2:
		kfree(iep);
	}
	IRUNLOCK;
}

static struct in_endpt *new_in_endpt(struct irctl *ir, struct usb_endpoint_descriptor *ep)
{
	struct usb_device *dev = ir->usbdev;
	struct in_endpt *iep;
	int pipe, maxp, len, addr;
	int mem_failure;

	addr = ep->bEndpointAddress;
	pipe = usb_rcvintpipe(dev, addr);
	maxp = usb_maxpacket(dev, pipe, usb_pipeout(pipe));

	len = (maxp > USB_BUFLEN) ? USB_BUFLEN : maxp;
	len -= (len % CODE_LENGTH);

	dprintk(DRIVER_NAME ": acceptable inbound endpoint (0x%x) found\n", addr);
	dprintk(DRIVER_NAME ": ep=0x%x maxp=%d len=%d\n", addr, maxp, len);

	mem_failure = 0;
	if ( !(iep = kmalloc(sizeof(*iep), GFP_KERNEL)) ) {
		mem_failure = 1;
	} else {
		memset(iep, 0, sizeof(*iep));
		iep->ir = ir;
		iep->ep = ep;
		iep->len = len;

#ifdef KERNEL_2_5
		if ( !(iep->buf = usb_buffer_alloc(dev, len, SLAB_ATOMIC, &iep->dma)) ) {
			mem_failure = 2;
		} else if ( !(iep->urb = usb_alloc_urb(0, GFP_KERNEL)) ) {
			mem_failure = 3;
		}
#else
		if ( !(iep->buf = kmalloc(len, GFP_KERNEL)) ) {
			mem_failure = 2;
		} else if ( !(iep->urb = usb_alloc_urb(0)) ) {
			mem_failure = 3;
		}
#endif
	}
	if (mem_failure) {
		free_in_endpt(iep, mem_failure);
		printk(DRIVER_NAME ": ep=0x%x out of memory (code=%d)\n", addr, mem_failure);
		return NULL;
	}
	return iep;
}

static void free_out_endpt(struct out_endpt *oep, int mem_failure)
{
	struct irctl *ir;
	if (!oep) return;

	ir = oep->ir;
	wake_up_all(&oep->wait);

	IRLOCK;
	switch (mem_failure) {
	case 4:
		usb_unlink_urb(oep->urb);
		usb_free_urb(oep->urb);
#ifdef KERNEL_2_5
	case 3:
		usb_buffer_free(oep->ir->usbdev, USB_BUFLEN, oep->buf, oep->dma);
#else
	case 3:
		kfree(oep->buf);
#endif
	case 2:
		kfree(oep);
	}
	IRUNLOCK;
}

static struct out_endpt *new_out_endpt(struct irctl *ir, struct usb_endpoint_descriptor *ep)
{
	struct usb_device *dev = ir->usbdev;
	struct out_endpt *oep;
	int mem_failure;

	dprintk(DRIVER_NAME ": acceptable outbound endpoint (0x%x) found\n", ep->bEndpointAddress);

	mem_failure = 0;
	if ( !(oep = kmalloc(sizeof(*oep), GFP_KERNEL)) ) {
		mem_failure = 1;
	} else {
		memset(oep, 0, sizeof(*oep));
		oep->ir = ir;
		oep->ep = ep;
		init_waitqueue_head(&oep->wait);

#ifdef KERNEL_2_5
		if ( !(oep->buf = usb_buffer_alloc(dev, USB_BUFLEN, SLAB_ATOMIC, &oep->dma)) ) {
			mem_failure = 2;
		} else if ( !(oep->urb = usb_alloc_urb(0, GFP_KERNEL)) ) {
			mem_failure = 3;
		}
#else
		if ( !(oep->buf = kmalloc(USB_BUFLEN, GFP_KERNEL)) ) {
			mem_failure = 2;
		} else if ( !(oep->urb = usb_alloc_urb(0)) ) {
			mem_failure = 3;
		}
#endif
	}
	if (mem_failure) {
		free_out_endpt(oep, mem_failure);
		printk(DRIVER_NAME ": ep=0x%x out of memory (code=%d)\n", ep->bEndpointAddress, mem_failure);
		return NULL;
	}
	return oep;
}

static void free_irctl(struct irctl *ir, int mem_failure)
{
	if (!ir) return;

	free_in_endpt(ir->in_keys, 0xFF);
	free_in_endpt(ir->in_mouse, 0xFF);
	free_out_endpt(ir->out_init, 0xFF);

	switch (mem_failure) {
	case 5:
		lirc_buffer_free(ir->p->rbuf);
	case 4:
		kfree(ir->p->rbuf);
	case 3:
		kfree(ir->p);
	case 2:
		kfree(ir);
	}
}

static struct irctl *new_irctl(struct usb_device *dev)
{
	struct irctl *ir;
	struct lirc_plugin *plugin;
	struct lirc_buffer *rbuf;
	int type;
	int mem_failure;

	/* determine remote type */
	switch (dev->descriptor.idVendor) {
	case VENDOR_ATI1:
		type = ATI1_COMPATIBLE;
		break;
	case VENDOR_ATI2:
		type = ATI2_COMPATIBLE;
		break;
	default:
		dprintk(DRIVER_NAME ": unknown type\n");
		return NULL;
	}
	dprintk(DRIVER_NAME ": remote type = %d\n", type);

	/* allocate kernel memory */
	mem_failure = 0;
	if ( !(ir = kmalloc(sizeof(*ir), GFP_KERNEL)) ) {
		mem_failure = 1;
	} else {
		memset(ir, 0, sizeof(*ir));

		if (!(plugin = kmalloc(sizeof(*plugin), GFP_KERNEL))) {
			mem_failure = 2;
		} else if (!(rbuf = kmalloc(sizeof(*rbuf), GFP_KERNEL))) {
			mem_failure = 3;
		} else if (lirc_buffer_init(rbuf, CODE_LENGTH, USB_BUFLEN/CODE_LENGTH)) {
			mem_failure = 4;
		} else {
			memset(plugin, 0, sizeof(*plugin));
			strcpy(plugin->name, DRIVER_NAME " ");
			plugin->minor = -1;
			plugin->code_length = CODE_LENGTH*8;
			plugin->features = LIRC_CAN_REC_LIRCCODE;
			plugin->data = ir;
			plugin->rbuf = rbuf;
			plugin->set_use_inc = &set_use_inc;
			plugin->set_use_dec = &set_use_dec;
			ir->usbdev = dev;
			ir->p = plugin;
			ir->remote_type = type;
			ir->devnum = dev->devnum;

			init_MUTEX(&ir->lock);
		}
	}
	if (mem_failure) {
		free_irctl(ir, mem_failure);
		printk(DRIVER_NAME ": out of memory (code=%d)\n", mem_failure);
		return NULL;
	}
	return ir;
}



#ifdef KERNEL_2_5
static int usb_remote_probe(struct usb_interface *intf,
				const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_host_interface *idesc;
#else
static void *usb_remote_probe(struct usb_device *dev, unsigned int ifnum,
				const struct usb_device_id *id)
{
	struct usb_interface *intf = &dev->actconfig->interface[ifnum];
	struct usb_interface_descriptor *idesc;
#endif
	struct usb_endpoint_descriptor *ep;
	struct irctl *ir;
	int i, type, devnum;
	char buf[63], name[128]="";

	dprintk(DRIVER_NAME ": usb probe called\n");

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,4)
	idesc = intf->cur_altsetting;
#else
	idesc = &intf->altsetting[intf->act_altsetting];
#endif

	if ( !(ir = new_irctl(dev)) ) {
#ifdef KERNEL_2_5
		return -ENOMEM;
#else
		return NULL;
#endif
	}
	type = ir->remote_type;
	devnum = ir->devnum;

	// step through the endpoints to find first in and first out endpoint
	// of type interrupt transfer
#ifdef KERNEL_2_5
	for (i = 0; i < idesc->desc.bNumEndpoints; ++i) {
		ep = &idesc->endpoint[i].desc;
#else
	for (i = 0; i < idesc->bNumEndpoints; ++i) {
		ep = &idesc->endpoint[i];
#endif
		if ( ((ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN)
			&& ((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT)) {

			// ATI2's ep 1 only reports mouse emulation, ep 2 reports both
			if ((type == ATI2_COMPATIBLE) && (ir->in_mouse == NULL)) {
				ir->in_mouse = new_in_endpt(ir,ep);
				ir->in_mouse->type = EP_MOUSE;
			} else if (ir->in_keys == NULL) {
				ir->in_keys = new_in_endpt(ir,ep);
				ir->in_keys->type = EP_KEYS;
			}
		}

		if ( ((ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT)
			&& ((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT)
			&& (ir->out_init == NULL)) {

			ir->out_init = new_out_endpt(ir,ep);
		}
	}
	if ((ir->in_keys == NULL) && (ir->in_mouse == NULL)) {
		dprintk(DRIVER_NAME ": inbound endpoint not found\n");
		free_irctl(ir, 0xFF);
#ifdef KERNEL_2_5
		return -ENODEV;
#else
		return NULL;
#endif
	}
	if ((ir->p->minor = lirc_register_plugin(ir->p)) < 0) {
		free_irctl(ir, 0xFF);
#ifdef KERNEL_2_5
		return -ENODEV;
#else
		return NULL;
#endif
	}


	if (dev->descriptor.iManufacturer
		&& usb_string(dev, dev->descriptor.iManufacturer, buf, 63) > 0)
		strncpy(name, buf, 128);
	if (dev->descriptor.iProduct
		&& usb_string(dev, dev->descriptor.iProduct, buf, 63) > 0)
		snprintf(name, 128, "%s %s", name, buf);
	printk(DRIVER_NAME "[%d]: %s on usb%d:%d\n", devnum, name,
	       dev->bus->busnum, devnum);

	/* inbound data */
	if (ir->in_keys) {
		struct in_endpt *iep = ir->in_keys;
		usb_fill_int_urb(iep->urb, dev,
			usb_rcvintpipe(dev,iep->ep->bEndpointAddress), iep->buf,
			iep->len, usb_remote_recv, iep, iep->ep->bInterval);
	}
	if (ir->in_mouse) {
		struct in_endpt *iep = ir->in_mouse;
		usb_fill_int_urb(iep->urb, dev,
			usb_rcvintpipe(dev,iep->ep->bEndpointAddress), iep->buf,
			iep->len, usb_remote_recv, iep, iep->ep->bInterval);
	}

	/* outbound data (initialization) */
	if (ir->out_init) {
		struct out_endpt *oep = ir->out_init;
		usb_fill_int_urb(oep->urb, dev,
			usb_sndintpipe(dev, oep->ep->bEndpointAddress), oep->buf,
			USB_BUFLEN, usb_remote_send, oep, oep->ep->bInterval);

		send_packet(oep, 0x8004, init1);
		send_packet(oep, 0x8007, init2);
	}

#ifdef KERNEL_2_5
	usb_set_intfdata(intf, ir);
	return SUCCESS;
#else
	return ir;
#endif
}


#ifdef KERNEL_2_5
static void usb_remote_disconnect(struct usb_interface *intf)
{
//	struct usb_device *dev = interface_to_usbdev(intf);
	struct irctl *ir = usb_get_intfdata(intf);
	usb_set_intfdata(intf, NULL);
#else
static void usb_remote_disconnect(struct usb_device *dev, void *ptr)
{
	struct irctl *ir = ptr;
#endif

	if (!ir || !ir->p)
		return;

	ir->usbdev = NULL;

	free_in_endpt(ir->in_keys, 0xFF);
	free_in_endpt(ir->in_mouse, 0xFF);
	free_out_endpt(ir->out_init, 0xFF);
	unregister_from_lirc(ir);
	free_irctl(ir, 0xFF);
}

static struct usb_driver usb_remote_driver = {
	.owner =	THIS_MODULE,
	.name =		DRIVER_NAME,
	.probe =	usb_remote_probe,
	.disconnect =	usb_remote_disconnect,
	.id_table =	usb_remote_table
};

static int __init usb_remote_init(void)
{
	int i;

	printk("\n" DRIVER_NAME ": " DRIVER_DESC " v" DRIVER_VERSION "\n");
	printk(DRIVER_NAME ": " DRIVER_AUTHOR "\n");
	dprintk(DRIVER_NAME ": debug mode enabled: $Id: lirc_atiusb.c,v 1.41 2004/10/29 02:02:01 pmiller9 Exp $\n");

	request_module("lirc_dev");

	repeat_jiffies = repeat*HZ/100;

	if ((i = usb_register(&usb_remote_driver)) < 0) {
		printk(DRIVER_NAME ": usb register failed, result = %d\n", i);
		return -ENODEV;
	}

	return SUCCESS;
}

static void __exit usb_remote_exit(void)
{
	usb_deregister(&usb_remote_driver);
}

module_init(usb_remote_init);
module_exit(usb_remote_exit);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(usb, usb_remote_table);

module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Debug enabled or not");

module_param(mask, int, 0444);
MODULE_PARM_DESC(mask, "Set channel acceptance bit mask");

module_param(unique, int, 0444);
MODULE_PARM_DESC(unique, "Enable channel-specific codes");

module_param(repeat, int, 0444);
MODULE_PARM_DESC(repeat, "Repeat timeout (1/100 sec)");

EXPORT_NO_SYMBOLS;

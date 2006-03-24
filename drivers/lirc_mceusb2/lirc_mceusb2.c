/*
 * LIRC driver for Philips eHome USB Infrared Transciever
 * and the Microsoft MCE 2005 Remote Control
 * 
 * (C) by Martin A. Blatter <martin_a_blatter@yahoo.com>
 *
 * Derived from ATI USB driver by Paul Miller and the original
 * MCE USB driver by Dan Corti
 *
 * This driver will only work reliably with kernel version 2.6.10
 * or higher, probably because of differences in USB device enumeration
 * in the kernel code. Device initialization fails most of the time
 * with earlier kernel versions.
 *
 **********************************************************************
 *
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
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 5)
#error "*******************************************************"
#error "Sorry, this driver needs kernel version 2.6.5 or higher"
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

#define DRIVER_VERSION		"0.22"
#define DRIVER_AUTHOR		"Martin Blatter <martin_a_blatter@yahoo.com>"
#define DRIVER_DESC		"USB remote driver for LIRC"
#define DRIVER_NAME		"lirc_mceusb2"

#define USB_BUFLEN		16
#define LIRCBUF_SIZE            256

#define MCE_CODE_LENGTH		5
#define MCE_TIME_UNIT           50
#define MCE_PACKET_SIZE 	4
#define MCE_PACKET_HEADER 	0x84

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

/* lock irctl structure */
//#define IRLOCK			down_interruptible(&ir->lock)
#define IRLOCK			down(&ir->lock)
#define IRUNLOCK		up(&ir->lock)

/* general constants */
#define SUCCESS			0
#define SEND_FLAG_IN_PROGRESS	1
#define SEND_FLAG_COMPLETE	2
#define RECV_FLAG_IN_PROGRESS	3
#define RECV_FLAG_COMPLETE	4

#define PHILUSB_INBOUND		1
#define PHILUSB_OUTBOUND	2

#define VENDOR_PHILIPS		0x0471
#define VENDOR_SMK              0x0609
#define VENDOR_TATUNG		0x1460
#define VENDOR_GATEWAY		0x107b

static struct usb_device_id usb_remote_table [] = {
	{ USB_DEVICE(VENDOR_PHILIPS, 0x0815) },	/* Philips eHome Infrared Transciever */
	{ USB_DEVICE(VENDOR_SMK, 0x031d) },	/* SMK/Toshiba G83C0004D410 */
	{ USB_DEVICE(VENDOR_TATUNG, 0x9150) },  /* Tatung eHome Infrared Transceiver */
        { USB_DEVICE(VENDOR_GATEWAY, 0x3009) },  /* Gateway eHome Infrared Transceiver */
	{ }					/* Terminating entry */
};

/* data structure for each usb remote */
struct irctl {

	/* usb */
	struct usb_device *usbdev;
	struct urb *urb_in;
	int devnum;

	/* buffers and dma */
	unsigned char *buf_in;
	unsigned int len_in;
	dma_addr_t dma_in;
	dma_addr_t dma_out;

	/* lirc */
	struct lirc_plugin *p;
	lirc_t lircdata[LIRCBUF_SIZE];
	int lirccnt;
	int connected;
	int last_space;

	/* handle sending (init strings) */
	int send_flags;
	wait_queue_head_t wait_out;
   
	struct semaphore lock;
};

/* init strings */
static char init1[] = {0x00, 0xff, 0xaa, 0xff, 0x0b};
static char init2[] = {0xff, 0x18};


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

static void usb_async_callback(struct urb *urb, struct pt_regs *regs)
{
	struct irctl *ir;
	int len;

	if (!urb)
		return;
	
	if ((ir = urb->context)) {
		len = urb->actual_length;

		dprintk(DRIVER_NAME "[%d]: callback called (status=%d len=%d)\n",ir->devnum,urb->status,len);

		if (debug)
			usb_remote_printdata(ir,urb->transfer_buffer,len);
	}

//	usb_unlink_urb(urb);
//	usb_free_urb(urb);
//	kfree(urb->transfer_buffer);
}


/* request incoming or send outgoing usb packet - used to initialize remote */
static void request_packet_async(struct irctl *ir, struct usb_endpoint_descriptor *ep, unsigned char* data, int size, int urb_type)
{
	int res;
	struct urb *async_urb;
	unsigned char *async_buf;

	if (urb_type) {
	    	if ((async_urb = usb_alloc_urb(0, GFP_KERNEL))) {
			/* alloc buffer */
			if ((async_buf = kmalloc(size, GFP_KERNEL))) {
				if (urb_type==PHILUSB_OUTBOUND) {
					/* outbound data */
					usb_fill_int_urb(async_urb, ir->usbdev, usb_sndintpipe(ir->usbdev, ep->bEndpointAddress), async_buf,
					size, usb_async_callback, ir, ep->bInterval);

					memcpy(async_buf, data, size);
					async_urb->transfer_flags=URB_ASYNC_UNLINK;
				}
				else {
					/* inbound data */
					usb_fill_int_urb(async_urb, ir->usbdev, usb_rcvintpipe(ir->usbdev, ep->bEndpointAddress), async_buf,
					size, usb_async_callback, ir, ep->bInterval);

					async_urb->transfer_flags=URB_ASYNC_UNLINK;
//					async_urb->transfer_flags=URB_SHORT_NOT_OK;
				}
			}
			else {
				usb_free_urb(async_urb);
				return;
			}
		}
	}
	else {
		/* standard request */
		async_urb=ir->urb_in;
		ir->send_flags = RECV_FLAG_IN_PROGRESS;
	}
	dprintk(DRIVER_NAME "[%d]: receive request called (size=%#x)\n", ir->devnum, size);

	async_urb->transfer_buffer_length = size;
	async_urb->dev = ir->usbdev;

	if ((res=usb_submit_urb(async_urb, SLAB_ATOMIC))) {
	    dprintk(DRIVER_NAME "[%d]: receive request FAILED! (res=%d)\n", ir->devnum, res);
	    return;
	}
	dprintk(DRIVER_NAME "[%d]: receive request complete (res=%d)\n", ir->devnum, res);
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

	lirc_buffer_free(p->rbuf);
	kfree(p->rbuf);
	kfree(p);
	kfree(ir);
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
		ir->connected = 0;
		IRUNLOCK;
	}
	MOD_DEC_USE_COUNT;
}

static void send_packet_to_lirc(struct irctl *ir)
{
    if ( ir->lirccnt ) {
	lirc_buffer_write_n(ir->p->rbuf,(unsigned char *) ir->lircdata,ir->lirccnt);

	wake_up(&ir->p->rbuf->wait_poll);
	ir->lirccnt=0;
    }
}

static void usb_remote_recv(struct urb *urb, struct pt_regs *regs)
{
	struct irctl *ir;
	int len;

	if (!urb)
		return;

	if (!(ir = urb->context)) {
		urb->transfer_flags |= URB_ASYNC_UNLINK;
		usb_unlink_urb(urb);
		return;
	}

	len = urb->actual_length;
	if (debug)
		usb_remote_printdata(ir,urb->transfer_buffer,len);

	if (ir->send_flags==RECV_FLAG_IN_PROGRESS) {
	  	ir->send_flags = SEND_FLAG_COMPLETE;
		dprintk(DRIVER_NAME "[%d]: setup answer received %d bytes\n",ir->devnum,len);
	}

	switch (urb->status) {

	/* success */
	case SUCCESS:

	    if ((len==MCE_CODE_LENGTH)) {

		if ((ir->buf_in[0]==MCE_PACKET_HEADER)) {

		    int i,keycode,pulse;
			
			/* buffer exhausted? */
			if (ir->lirccnt>(LIRCBUF_SIZE-MCE_CODE_LENGTH))
				send_packet_to_lirc(ir);

		    for(i=0;i<MCE_PACKET_SIZE;i++) {
			pulse = 0;
			keycode=(signed char)ir->buf_in[i+1];
			if ( keycode < 0 ) {
			    pulse = 1;
			    keycode += 128;
			}
			keycode *= MCE_TIME_UNIT;

			if ( pulse ) {
			    if ( ir->last_space ) {
				ir->lircdata[ir->lirccnt++] = ir->last_space;
				ir->last_space = 0;
				ir->lircdata[ir->lirccnt] = 0;
			    }
			    ir->lircdata[ir->lirccnt] += keycode;
			    ir->lircdata[ir->lirccnt] |= PULSE_BIT;
			}
			else {
			    if ( ir->lircdata[ir->lirccnt] &&
				!ir->last_space ) {
				ir->lirccnt++;
			    }
			    ir->last_space += keycode;
			}
		    }
		}
	    }
	    else {
		/* transmission finished (long packet) */
		send_packet_to_lirc(ir);
	    }
	    
	    break;

	/* unlink */
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		urb->transfer_flags |= URB_ASYNC_UNLINK;
		usb_unlink_urb(urb);
		return;

	case -EPIPE:
	default:
		break;
	}

	/* resubmit urb */
	usb_submit_urb(urb, SLAB_ATOMIC);
}

static int usb_remote_probe(struct usb_interface *intf,
				const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_host_interface *idesc;
	struct usb_endpoint_descriptor *ep=NULL, *ep_in=NULL, *ep_out=NULL;
	struct usb_host_config *config;
	struct irctl *ir = NULL;
	struct lirc_plugin *plugin = NULL;
	struct lirc_buffer *rbuf = NULL;
	int devnum, pipe, maxp;
	int minor = 0;
	int i;
	char buf[63], name[128]="";
	int mem_failure = 0;

	dprintk(DRIVER_NAME ": usb probe called\n");

	usb_reset_device(dev);

	config=dev->actconfig;

	idesc = intf->cur_altsetting;

	/* step through the endpoints to find first bulk in and out endpoint */
	for (i = 0; i < idesc->desc.bNumEndpoints; ++i) {
		ep = &idesc->endpoint[i].desc;

		if ((ep_in == NULL)
			&& ((ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN)
			&& (((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK)
			||  ((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT))) {

			dprintk(DRIVER_NAME ": acceptable inbound endpoint found\n");
			ep_in = ep;
			ep_in->bmAttributes=USB_ENDPOINT_XFER_INT;
			ep_in->bInterval=1;
		}

		if ((ep_out == NULL)
			&& ((ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT)
			&& (((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK)
			||  ((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT))) {

			dprintk(DRIVER_NAME ": acceptable outbound endpoint found\n");
			ep_out = ep;
			ep_out->bmAttributes=USB_ENDPOINT_XFER_INT;
			ep_out->bInterval=1;
		}
	}
	if (ep_in == NULL) {
		dprintk(DRIVER_NAME ": inbound and/or endpoint not found\n");
		return -ENODEV;
	}

	devnum = dev->devnum;
	pipe = usb_rcvintpipe(dev, ep_in->bEndpointAddress);
	maxp = usb_maxpacket(dev, pipe, usb_pipeout(pipe));

	/* allocate kernel memory */
	mem_failure = 0;
	if (!(ir = kmalloc(sizeof(struct irctl), GFP_KERNEL))) {
		mem_failure = 1;
	} else {
		memset(ir, 0, sizeof(struct irctl));

		if (!(plugin = kmalloc(sizeof(struct lirc_plugin), GFP_KERNEL))) {
			mem_failure = 2;
		} else if (!(rbuf = kmalloc(sizeof(struct lirc_buffer), GFP_KERNEL))) {
			mem_failure = 3;
		} else if (lirc_buffer_init(rbuf, sizeof(lirc_t), LIRCBUF_SIZE)) {
			mem_failure = 4;
		} else if (!(ir->buf_in = usb_buffer_alloc(dev, maxp, SLAB_ATOMIC, &ir->dma_in))) {
			mem_failure = 5;
		} else if (!(ir->urb_in = usb_alloc_urb(0, GFP_KERNEL))) {
			mem_failure = 7;
		} else {

			memset(plugin, 0, sizeof(struct lirc_plugin));

			strcpy(plugin->name, DRIVER_NAME " ");
			plugin->minor = -1;
			plugin->features = LIRC_CAN_REC_MODE2;
			plugin->data = ir;
			plugin->rbuf = rbuf;
			plugin->set_use_inc = &set_use_inc;
			plugin->set_use_dec = &set_use_dec;
			plugin->code_length = sizeof(lirc_t) * 8;
			plugin->ioctl = NULL;
			plugin->owner = THIS_MODULE;

			init_MUTEX(&ir->lock);
			init_waitqueue_head(&ir->wait_out);

			if ((minor = lirc_register_plugin(plugin)) < 0) {
				mem_failure = 9;
			}
		}
	}

	/* free allocated memory incase of failure */
	switch (mem_failure) {
	case 9:
		lirc_buffer_free(rbuf);
	case 7:
		usb_free_urb(ir->urb_in);
	case 5:
		usb_buffer_free(dev, maxp, ir->buf_in, ir->dma_in);
	case 4:
		kfree(rbuf);
	case 3:
		kfree(plugin);
	case 2:
		kfree(ir);
	case 1:
		printk(DRIVER_NAME "[%d]: out of memory (code=%d)\n",
			devnum, mem_failure);
		return -ENOMEM;
	}

	plugin->minor = minor;
	ir->p = plugin;
	ir->devnum = devnum;
	ir->usbdev = dev;
	ir->len_in = maxp;
	ir->last_space = PULSE_MASK;
	ir->connected = 0;

	if (dev->descriptor.iManufacturer
		&& usb_string(dev, dev->descriptor.iManufacturer, buf, 63) > 0)
		strncpy(name, buf, 128);
	if (dev->descriptor.iProduct
		&& usb_string(dev, dev->descriptor.iProduct, buf, 63) > 0)
		snprintf(name, 128, "%s %s", name, buf);
	printk(DRIVER_NAME "[%d]: %s on usb%d:%d\n", devnum, name,
	       dev->bus->busnum, devnum);

	/* inbound data */
	usb_fill_int_urb(ir->urb_in, dev, pipe, ir->buf_in,
		maxp, usb_remote_recv, ir, ep_in->bInterval);

	/* initialize device */
	request_packet_async( ir, ep_in, NULL, maxp, PHILUSB_INBOUND );
	request_packet_async( ir, ep_in, NULL, maxp, PHILUSB_INBOUND );
	request_packet_async( ir, ep_out, init1, sizeof(init1), PHILUSB_OUTBOUND );
	request_packet_async( ir, ep_in, NULL, maxp, PHILUSB_INBOUND );
	request_packet_async( ir, ep_out, init2, sizeof(init2), PHILUSB_OUTBOUND );
	request_packet_async( ir, ep_in, NULL, maxp, 0);

	usb_set_intfdata(intf, ir);
	return SUCCESS;
}


static void usb_remote_disconnect(struct usb_interface *intf)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct irctl *ir = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);

	if (!ir || !ir->p)
		return;

	ir->usbdev = NULL;
	wake_up_all(&ir->wait_out);

	IRLOCK;
	usb_kill_urb(ir->urb_in);
	usb_free_urb(ir->urb_in);
	usb_buffer_free(dev, ir->len_in, ir->buf_in, ir->dma_in);
	IRUNLOCK;

	unregister_from_lirc(ir);
}

static struct usb_driver usb_remote_driver = {
	LIRC_THIS_MODULE(.owner = THIS_MODULE)
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
	dprintk(DRIVER_NAME ": debug mode enabled\n");

	request_module("lirc_dev");

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

EXPORT_NO_SYMBOLS;

/*
 *   lirc_imon.c:  LIRC/VFD/LCD driver for SoundGraph iMON IR/VFD/LCD
 *		   including the iMON PAD model
 *
 *   $Id: lirc_imon.c,v 1.92 2009/07/06 18:05:11 jarodwilson Exp $
 *
 *   Copyright(C) 2004  Venky Raju(dev@venky.ws)
 *
 *   lirc_imon is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <linux/version.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
#error "*** Sorry, this driver requires a 2.6 kernel"
#endif

#include <linux/autoconf.h>

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 18)
#include <asm/uaccess.h>
#else
#include <linux/uaccess.h>
#endif
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/time.h>
#include <linux/timer.h>

#include "drivers/kcompat.h"
#include "drivers/lirc.h"
#include "drivers/lirc_dev/lirc_dev.h"


#define MOD_AUTHOR	"Venky Raju <dev@venky.ws>"
#define MOD_DESC	"Driver for SoundGraph iMON MultiMedia IR/Display"
#define MOD_NAME	"lirc_imon"
#define MOD_VERSION	"0.6"

#define DISPLAY_MINOR_BASE	144
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 15)
#define DEVFS_MODE	(S_IFCHR | S_IRUSR | S_IWUSR | \
			 S_IRGRP | S_IWGRP | S_IROTH)
#endif
#define DEVICE_NAME	LIRC_DEVFS_PREFIX "lcd%d"

#define BUF_CHUNK_SIZE	4
#define BUF_SIZE	128

#define BIT_DURATION	250	/* each bit received is 250us */

#define dprintk(fmt, args...)						\
	do {								\
		if (debug)						\
			printk(KERN_INFO MOD_NAME ": " fmt, ## args);	\
	} while (0)

/*** P R O T O T Y P E S ***/

/* USB Callback prototypes */
static int imon_probe(struct usb_interface *interface,
		      const struct usb_device_id *id);
static void imon_disconnect(struct usb_interface *interface);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static void usb_rx_callback_intf0(struct urb *urb, struct pt_regs *regs);
static void usb_rx_callback_intf1(struct urb *urb, struct pt_regs *regs);
static void usb_tx_callback(struct urb *urb, struct pt_regs *regs);
#else
static void usb_rx_callback_intf0(struct urb *urb);
static void usb_rx_callback_intf1(struct urb *urb);
static void usb_tx_callback(struct urb *urb);
#endif

/* suspend/resume support */
static int imon_resume(struct usb_interface *intf);
static int imon_suspend(struct usb_interface *intf, pm_message_t message);

/* Display file_operations function prototypes */
static int display_open(struct inode *inode, struct file *file);
static int display_close(struct inode *inode, struct file *file);

/* VFD write operation */
static ssize_t vfd_write(struct file *file, const char *buf,
			 size_t n_bytes, loff_t *pos);

/* LCD file_operations override function prototypes */
static ssize_t lcd_write(struct file *file, const char *buf,
			 size_t n_bytes, loff_t *pos);

/* LIRC driver function prototypes */
static int ir_open(void *data);
static void ir_close(void *data);

/* Driver init/exit prototypes */
static int __init imon_init(void);
static void __exit imon_exit(void);

/*** G L O B A L S ***/

struct imon_context {
	struct usb_device *usbdev_intf0;
	/* Newer devices have two interfaces */
	struct usb_device *usbdev_intf1;
	int display_supported;		/* not all controllers do */
	int display_isopen;		/* display port has been opened */
	int ir_isopen;			/* IR port open	*/
	int ir_isassociating;		/* IR port open for association */
	int dev_present_intf0;		/* USB device presence, interface 0 */
	int dev_present_intf1;		/* USB device presence, interface 1 */
	struct mutex lock;		/* to lock this object */
	wait_queue_head_t remove_ok;	/* For unexpected USB disconnects */

	int vfd_proto_6p;		/* some VFD require a 6th packet */
	int ir_onboard_decode;		/* IR signals decoded onboard */

	struct lirc_driver *driver;
	struct usb_endpoint_descriptor *rx_endpoint_intf0;
	struct usb_endpoint_descriptor *rx_endpoint_intf1;
	struct usb_endpoint_descriptor *tx_endpoint;
	struct urb *rx_urb_intf0;
	struct urb *rx_urb_intf1;
	struct urb *tx_urb;
	int tx_control;
	unsigned char usb_rx_buf[8];
	unsigned char usb_tx_buf[8];

	struct rx_data {
		int count;		/* length of 0 or 1 sequence */
		int prev_bit;		/* logic level of sequence */
		int initial_space;	/* initial space flag */
	} rx;

	struct tx_t {
		unsigned char data_buf[35];	/* user data buffer */
		struct completion finished;	/* wait for write to finish */
		atomic_t busy;			/* write in progress */
		int status;			/* status of tx completion */
	} tx;

	int ffdc_dev;			/* is this the overused ffdc ID? */
	int ir_protocol;		/* iMON or MCE (RC6) IR protocol? */
	struct input_dev *mouse;	/* input device for iMON PAD remote */
	struct input_dev *touch;	/* input device for touchscreen */
	int has_touchscreen;		/* touchscreen present? */
	int pad_mouse;			/* toggle kbd(0)/mouse(1) mode */
	int touch_x;			/* x coordinate on touchscreen */
	int touch_y;			/* y coordinate on touchscreen */
	char name_mouse[128];
	char phys_mouse[64];
	char name_touch[128];
	char phys_touch[64];
	struct timer_list timer;
};

#define TOUCH_TIMEOUT	(HZ/30)

/* display file operations. Nb: lcd_write will be subbed in as needed later */
static struct file_operations display_fops = {
	.owner		= THIS_MODULE,
	.open		= &display_open,
	.write		= &vfd_write,
	.release	= &display_close
};

enum {
	IMON_DISPLAY_TYPE_AUTO = 0,
	IMON_DISPLAY_TYPE_VFD  = 1,
	IMON_DISPLAY_TYPE_LCD  = 2,
	IMON_DISPLAY_TYPE_VGA  = 3,
	IMON_DISPLAY_TYPE_NONE = 4,
};

enum {
	IMON_IR_PROTOCOL_IMON = 0,
	IMON_IR_PROTOCOL_MCE  = 1,
};
/*
 * USB Device ID for iMON USB Control Boards
 *
 * The Windows drivers contain 6 different inf files, more or less one for
 * each new device until the 0x0034-0x0046 devices, which all use the same
 * driver. Some of the devices in the 34-46 range haven't been definitively
 * identified yet. Early devices have either a TriGem Computer, Inc. or a
 * Samsung vendor ID (0x0aa8 and 0x04e8 respectively), while all later
 * devices use the SoundGraph vendor ID (0x15c2).
 */
static struct usb_device_id imon_usb_id_table[] = {
	/* TriGem iMON (IR only) -- TG_iMON.inf */
	{ USB_DEVICE(0x0aa8, 0x8001) },

	/* SoundGraph iMON (IR only) -- sg_imon.inf */
	{ USB_DEVICE(0x04e8, 0xff30) },

	/* SoundGraph iMON VFD (IR & VFD) -- iMON_VFD.inf */
	{ USB_DEVICE(0x0aa8, 0xffda) },

	/* SoundGraph iMON SS (IR & VFD) -- iMON_SS.inf */
	{ USB_DEVICE(0x15c2, 0xffda) },

	/*
	 * Several devices with this same device ID, all use iMON_PAD.inf
	 * SoundGraph iMON PAD (IR & VFD)
	 * SoundGraph iMON PAD (IR & LCD)
	 * SoundGraph iMON Knob (IR only)
	 */
	{ USB_DEVICE(0x15c2, 0xffdc) },

	/*
	 * Newer devices, all driven by the latest iMON Windows driver, full
	 * list of device IDs extracted via 'strings Setup/data1.hdr |grep 15c2'
	 * Need user input to fill in details on unknown devices.
	 */
	/* SoundGraph iMON OEM Touch LCD (IR & 7" VGA LCD) */
	{ USB_DEVICE(0x15c2, 0x0034) },
	/* SoundGraph iMON OEM Touch LCD (IR & 4.3" VGA LCD) */
	{ USB_DEVICE(0x15c2, 0x0035) },
	/* SoundGraph iMON OEM VFD (IR & VFD) */
	{ USB_DEVICE(0x15c2, 0x0036) },
	/* device specifics unknown */
	{ USB_DEVICE(0x15c2, 0x0037) },
	/* SoundGraph iMON OEM LCD (IR & LCD) */
	{ USB_DEVICE(0x15c2, 0x0038) },
	/* device specifics unknown */
	{ USB_DEVICE(0x15c2, 0x0039) },
	/* device specifics unknown */
	{ USB_DEVICE(0x15c2, 0x003a) },
	/* device specifics unknown */
	{ USB_DEVICE(0x15c2, 0x003b) },
	/* SoundGraph iMON OEM Inside (IR only) */
	{ USB_DEVICE(0x15c2, 0x003c) },
	/* device specifics unknown */
	{ USB_DEVICE(0x15c2, 0x003d) },
	/* device specifics unknown */
	{ USB_DEVICE(0x15c2, 0x003e) },
	/* device specifics unknown */
	{ USB_DEVICE(0x15c2, 0x003f) },
	/* device specifics unknown */
	{ USB_DEVICE(0x15c2, 0x0040) },
	/* SoundGraph iMON MINI (IR only) */
	{ USB_DEVICE(0x15c2, 0x0041) },
	/* Antec Veris Multimedia Station EZ External (IR only) */
	{ USB_DEVICE(0x15c2, 0x0042) },
	/* Antec Veris Multimedia Station Basic Internal (IR only) */
	{ USB_DEVICE(0x15c2, 0x0043) },
	/* Antec Veris Multimedia Station Elite (IR & VFD) */
	{ USB_DEVICE(0x15c2, 0x0044) },
	/* Antec Veris Multimedia Station Premiere (IR & LCD) */
	{ USB_DEVICE(0x15c2, 0x0045) },
	/* device specifics unknown */
	{ USB_DEVICE(0x15c2, 0x0046) },
	{}
};

/* Some iMON VFD models requires a 6th packet for VFD writes */
static struct usb_device_id vfd_proto_6p_list[] = {
	{ USB_DEVICE(0x15c2, 0xffda) },
	{ USB_DEVICE(0x15c2, 0xffdc) },
	{ USB_DEVICE(0x15c2, 0x0036) },
	{ USB_DEVICE(0x15c2, 0x0044) },
	{}
};

/* newer iMON models use control endpoints */
static struct usb_device_id ctl_ep_device_list[] = {
	{ USB_DEVICE(0x15c2, 0x0034) },
	{ USB_DEVICE(0x15c2, 0x0035) },
	{ USB_DEVICE(0x15c2, 0x0036) },
	{ USB_DEVICE(0x15c2, 0x0037) },
	{ USB_DEVICE(0x15c2, 0x0038) },
	{ USB_DEVICE(0x15c2, 0x0039) },
	{ USB_DEVICE(0x15c2, 0x003a) },
	{ USB_DEVICE(0x15c2, 0x003b) },
	{ USB_DEVICE(0x15c2, 0x003c) },
	{ USB_DEVICE(0x15c2, 0x003d) },
	{ USB_DEVICE(0x15c2, 0x003e) },
	{ USB_DEVICE(0x15c2, 0x003f) },
	{ USB_DEVICE(0x15c2, 0x0040) },
	{ USB_DEVICE(0x15c2, 0x0041) },
	{ USB_DEVICE(0x15c2, 0x0042) },
	{ USB_DEVICE(0x15c2, 0x0043) },
	{ USB_DEVICE(0x15c2, 0x0044) },
	{ USB_DEVICE(0x15c2, 0x0045) },
	{ USB_DEVICE(0x15c2, 0x0046) },
	{}
};

/* iMON LCD models use a different write op */
static struct usb_device_id lcd_device_list[] = {
	{ USB_DEVICE(0x15c2, 0xffdc) },
	{ USB_DEVICE(0x15c2, 0x0038) },
	{ USB_DEVICE(0x15c2, 0x0045) },
	{}
};

/* iMON devices with front panel buttons or touchscreen need a larger buffer */
static struct usb_device_id large_buffer_list[] = {
	{ USB_DEVICE(0x15c2, 0x0034) },
	{ USB_DEVICE(0x15c2, 0x0035) },
	{ USB_DEVICE(0x15c2, 0x0038) },
	{ USB_DEVICE(0x15c2, 0x0045) },
};

/* Newer iMON models decode the signal onboard */
static struct usb_device_id ir_onboard_decode_list[] = {
	{ USB_DEVICE(0x15c2, 0xffdc) },
	{ USB_DEVICE(0x15c2, 0x0034) },
	{ USB_DEVICE(0x15c2, 0x0035) },
	{ USB_DEVICE(0x15c2, 0x0036) },
	{ USB_DEVICE(0x15c2, 0x0037) },
	{ USB_DEVICE(0x15c2, 0x0038) },
	{ USB_DEVICE(0x15c2, 0x0039) },
	{ USB_DEVICE(0x15c2, 0x003a) },
	{ USB_DEVICE(0x15c2, 0x003b) },
	{ USB_DEVICE(0x15c2, 0x003c) },
	{ USB_DEVICE(0x15c2, 0x003d) },
	{ USB_DEVICE(0x15c2, 0x003e) },
	{ USB_DEVICE(0x15c2, 0x003f) },
	{ USB_DEVICE(0x15c2, 0x0040) },
	{ USB_DEVICE(0x15c2, 0x0041) },
	{ USB_DEVICE(0x15c2, 0x0042) },
	{ USB_DEVICE(0x15c2, 0x0043) },
	{ USB_DEVICE(0x15c2, 0x0044) },
	{ USB_DEVICE(0x15c2, 0x0045) },
	{ USB_DEVICE(0x15c2, 0x0046) },
	{}
};

/* Some iMON devices have no lcd/vfd, don't set one up */
static struct usb_device_id ir_only_list[] = {
	{ USB_DEVICE(0x0aa8, 0x8001) },
	{ USB_DEVICE(0x04e8, 0xff30) },
	/* the first imon lcd and the knob share this device id. :\ */
	/*{ USB_DEVICE(0x15c2, 0xffdc) },*/
	{ USB_DEVICE(0x15c2, 0x003c) },
	{ USB_DEVICE(0x15c2, 0x0041) },
	{ USB_DEVICE(0x15c2, 0x0042) },
	{ USB_DEVICE(0x15c2, 0x0043) },
	{}
};

/* iMON devices with VGA touchscreens */
static struct usb_device_id imon_touchscreen_list[] = {
	{ USB_DEVICE(0x15c2, 0x0034) },
	{ USB_DEVICE(0x15c2, 0x0035) },
	{}
};

/* USB Device data */
static struct usb_driver imon_driver = {
	LIRC_THIS_MODULE(.owner = THIS_MODULE)
	.name		= MOD_NAME,
	.probe		= imon_probe,
	.disconnect	= imon_disconnect,
	.suspend	= imon_suspend,
	.resume		= imon_resume,
	.id_table	= imon_usb_id_table,
};

static struct usb_class_driver imon_class = {
	.name		= DEVICE_NAME,
	.fops		= &display_fops,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 15)
	.mode		= DEVFS_MODE,
#endif
	.minor_base	= DISPLAY_MINOR_BASE,
};

/* to prevent races between open() and disconnect(), probing, etc */
static DEFINE_MUTEX(driver_lock);

static int debug;

/* lcd, vfd, vga or none? should be auto-detected, but can be overridden... */
static int display_type;

/* IR protocol: native iMON or Windows MCE (RC-6) */
static int ir_protocol;

/*
 * In certain use cases, mouse mode isn't really helpful, and could actually
 * cause confusion, so allow disabling it when the IR device is open.
 */
static int nomouse;


/***  M O D U L E   C O D E ***/

MODULE_AUTHOR(MOD_AUTHOR);
MODULE_DESCRIPTION(MOD_DESC);
MODULE_VERSION(MOD_VERSION);
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(usb, imon_usb_id_table);
module_param(debug, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug messages: 0=no, 1=yes(default: no)");
module_param(display_type, int, S_IRUGO);
MODULE_PARM_DESC(display_type, "Type of attached display. 0=autodetect, "
		 "1=vfd, 2=lcd, 3=vga, 4=none (default: autodetect)");
module_param(ir_protocol, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(ir_protocol, "Which IR protocol to use. 0=native iMON, "
		 "1=Windows Media Center Ed. (RC-6) (default: native iMON)");
module_param(nomouse, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(nomouse, "Disable mouse input device mode when IR device is "
		 "open. 0=don't disable, 1=disable. (default: don't disable)");

static void free_imon_context(struct imon_context *context)
{
	usb_free_urb(context->tx_urb);
	usb_free_urb(context->rx_urb_intf0);
	usb_free_urb(context->rx_urb_intf1);
	lirc_buffer_free(context->driver->rbuf);
	kfree(context->driver->rbuf);
	kfree(context->driver);
	kfree(context);

	dprintk("%s: iMON context freed\n", __func__);
}

static void deregister_from_lirc(struct imon_context *context)
{
	int retval;
	int minor = context->driver->minor;

	retval = lirc_unregister_driver(minor);
	if (retval)
		err("%s: unable to deregister from lirc(%d)",
			__func__, retval);
	else
		printk(KERN_INFO MOD_NAME ": Deregistered iMON driver "
		       "(minor:%d)\n", minor);

}

/**
 * Called when the Display device (e.g. /dev/lcd0)
 * is opened by the application.
 */
static int display_open(struct inode *inode, struct file *file)
{
	struct usb_interface *interface;
	struct imon_context *context = NULL;
	int subminor;
	int retval = 0;

	/* prevent races with disconnect */
	mutex_lock(&driver_lock);

	subminor = iminor(inode);
	interface = usb_find_interface(&imon_driver, subminor);
	if (!interface) {
		err("%s: could not find interface for minor %d",
		    __func__, subminor);
		retval = -ENODEV;
		goto exit;
	}
	context = usb_get_intfdata(interface);

	if (!context) {
		err("%s: no context found for minor %d",
					__func__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	mutex_lock(&context->lock);

	if (!context->display_supported) {
		err("%s: display not supported by device", __func__);
		retval = -ENODEV;
	} else if (context->display_isopen) {
		err("%s: display port is already open", __func__);
		retval = -EBUSY;
	} else {
		MOD_INC_USE_COUNT;
		context->display_isopen = 1;
		file->private_data = context;
		printk(KERN_INFO "display port opened\n");
	}

	mutex_unlock(&context->lock);

exit:
	mutex_unlock(&driver_lock);
	return retval;
}

/**
 * Called when the display device (e.g. /dev/lcd0)
 * is closed by the application.
 */
static int display_close(struct inode *inode, struct file *file)
{
	struct imon_context *context = NULL;
	int retval = 0;

	context = (struct imon_context *)file->private_data;

	if (!context) {
		err("%s: no context for device", __func__);
		return -ENODEV;
	}

	mutex_lock(&context->lock);

	if (!context->display_supported) {
		err("%s: display not supported by device", __func__);
		retval = -ENODEV;
	} else if (!context->display_isopen) {
		err("%s: display is not open", __func__);
		retval = -EIO;
	} else {
		context->display_isopen = 0;
		MOD_DEC_USE_COUNT;
		printk(KERN_INFO "display port closed\n");
		if (!context->dev_present_intf0 && !context->ir_isopen) {
			/*
			 * Device disconnected before close and IR port is not
			 * open. If IR port is open, context will be deleted by
			 * ir_close.
			 */
			mutex_unlock(&context->lock);
			free_imon_context(context);
			return retval;
		}
	}

	mutex_unlock(&context->lock);
	return retval;
}

/**
 * Sends a packet to the device
 */
static int send_packet(struct imon_context *context)
{
	unsigned int pipe;
	int interval = 0;
	int retval = 0;
	struct usb_ctrlrequest *control_req = NULL;

	/* Check if we need to use control or interrupt urb */
	if (!context->tx_control) {
		pipe = usb_sndintpipe(context->usbdev_intf0,
				      context->tx_endpoint->bEndpointAddress);
		interval = context->tx_endpoint->bInterval;

		usb_fill_int_urb(context->tx_urb, context->usbdev_intf0, pipe,
				 context->usb_tx_buf,
				 sizeof(context->usb_tx_buf),
				 usb_tx_callback, context, interval);

		context->tx_urb->actual_length = 0;
	} else {
		/* fill request into kmalloc'ed space: */
		control_req = kmalloc(sizeof(struct usb_ctrlrequest),
				      GFP_KERNEL);
		if (control_req == NULL)
			return -ENOMEM;

		/* setup packet is '21 09 0200 0001 0008' */
		control_req->bRequestType = 0x21;
		control_req->bRequest = 0x09;
		control_req->wValue = cpu_to_le16(0x0200);
		control_req->wIndex = cpu_to_le16(0x0001);
		control_req->wLength = cpu_to_le16(0x0008);

		/* control pipe is endpoint 0x00 */
		pipe = usb_sndctrlpipe(context->usbdev_intf0, 0);

		/* build the control urb */
		usb_fill_control_urb(context->tx_urb, context->usbdev_intf0, pipe,
				     (unsigned char *)control_req,
				     context->usb_tx_buf,
				     sizeof(context->usb_tx_buf),
				     usb_tx_callback, context);
		context->tx_urb->actual_length = 0;
	}

	init_completion(&context->tx.finished);
	atomic_set(&(context->tx.busy), 1);

	retval = usb_submit_urb(context->tx_urb, GFP_KERNEL);
	if (retval) {
		atomic_set(&(context->tx.busy), 0);
		err("%s: error submitting urb(%d)", __func__, retval);
	} else {
		/* Wait for transmission to complete (or abort) */
		mutex_unlock(&context->lock);
		retval = wait_for_completion_interruptible(
				&context->tx.finished);
		if (retval)
			err("%s: task interrupted", __func__);
		mutex_lock(&context->lock);

		retval = context->tx.status;
		if (retval)
			err("%s: packet tx failed (%d)", __func__, retval);
	}

	kfree(control_req);

	return retval;
}

/**
 * Sends an associate packet to the iMON 2.4G.
 *
 * This might not be such a good idea, since it has an id collision with
 * some versions of the "IR & VFD" combo. The only way to determine if it
 * is an RF version is to look at the product description string. (Which
 * we currently do not fetch).
 */
static int send_associate_24g(struct imon_context *context)
{
	int retval;
	const unsigned char packet[8] = { 0x01, 0x00, 0x00, 0x00,
					  0x00, 0x00, 0x00, 0x20 };

	if (!context) {
		err("%s: no context for device", __func__);
		return -ENODEV;
	}

	mutex_lock(&context->lock);

	if (!context->dev_present_intf0) {
		err("%s: no iMON device present", __func__);
		retval = -ENODEV;
		goto exit;
	}

	memcpy(context->usb_tx_buf, packet, sizeof(packet));
	retval = send_packet(context);

exit:
	mutex_unlock(&context->lock);

	return retval;
}

/**
 * These are the sysfs functions to handle the association on the iMON 2.4G LT.
 */
static ssize_t show_associate_remote(struct device *d,
				     struct device_attribute *attr,
				     char *buf)
{
	struct imon_context *context = dev_get_drvdata(d);

	if (!context)
		return -ENODEV;

	mutex_lock(&context->lock);
	if (context->ir_isassociating) {
		strcpy(buf, "associating\n");
	} else if (context->ir_isopen) {
		strcpy(buf, "open\n");
	} else {
		strcpy(buf, "closed\n");
	}
	printk(KERN_INFO "Visit http://www.lirc.org/html/imon-24g.html for "
	       "instructions on how to associate your iMON 2.4G DT/LT "
	       "remote\n");
	mutex_unlock(&context->lock);
	return strlen(buf);
}

static ssize_t store_associate_remote(struct device *d,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct imon_context *context;

	context = dev_get_drvdata(d);

	if (!context)
		return -ENODEV;

	mutex_lock(&context->lock);
	if (!context->ir_isopen) {
		mutex_unlock(&context->lock);
		return -EINVAL;
	}

	if (context->ir_isopen) {
		context->ir_isassociating = 1;
		send_associate_24g(context);
	}
	mutex_unlock(&context->lock);

	return count;
}

static DEVICE_ATTR(associate_remote, S_IWUSR | S_IRUGO, show_associate_remote,
		   store_associate_remote);

static struct attribute *imon_sysfs_entries[] = {
	&dev_attr_associate_remote.attr,
	NULL
};

static struct attribute_group imon_attribute_group = {
	.attrs = imon_sysfs_entries
};

/**
 * Writes data to the VFD.  The iMON VFD is 2x16 characters
 * and requires data in 5 consecutive USB interrupt packets,
 * each packet but the last carrying 7 bytes.
 *
 * I don't know if the VFD board supports features such as
 * scrolling, clearing rows, blanking, etc. so at
 * the caller must provide a full screen of data.  If fewer
 * than 32 bytes are provided spaces will be appended to
 * generate a full screen.
 */
static ssize_t vfd_write(struct file *file, const char *buf,
			 size_t n_bytes, loff_t *pos)
{
	int i;
	int offset;
	int seq;
	int retval = 0;
	struct imon_context *context;
	const unsigned char vfd_packet6[] = {
		0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF };

	context = (struct imon_context *)file->private_data;
	if (!context) {
		err("%s: no context for device", __func__);
		return -ENODEV;
	}

	mutex_lock(&context->lock);

	if (!context->dev_present_intf0) {
		err("%s: no iMON device present", __func__);
		retval = -ENODEV;
		goto exit;
	}

	if (n_bytes <= 0 || n_bytes > 32) {
		err("%s: invalid payload size", __func__);
		retval = -EINVAL;
		goto exit;
	}

	if (copy_from_user(context->tx.data_buf, buf, n_bytes)) {
		retval = -EFAULT;
		goto exit;
	}

	/* Pad with spaces */
	for (i = n_bytes; i < 32; ++i)
		context->tx.data_buf[i] = ' ';

	for (i = 32; i < 35; ++i)
		context->tx.data_buf[i] = 0xFF;

	offset = 0;
	seq = 0;

	do {
		memcpy(context->usb_tx_buf, context->tx.data_buf + offset, 7);
		context->usb_tx_buf[7] = (unsigned char) seq;

		retval = send_packet(context);
		if (retval) {
			err("%s: send packet failed for packet #%d",
					__func__, seq/2);
			goto exit;
		} else {
			seq += 2;
			offset += 7;
		}

	} while (offset < 35);

	if (context->vfd_proto_6p) {
		/* Send packet #6 */
		memcpy(context->usb_tx_buf, &vfd_packet6, sizeof(vfd_packet6));
		context->usb_tx_buf[7] = (unsigned char) seq;
		retval = send_packet(context);
		if (retval)
			err("%s: send packet failed for packet #%d",
					__func__, seq/2);
	}

exit:
	mutex_unlock(&context->lock);

	return (!retval) ? n_bytes : retval;
}

/**
 * Writes data to the LCD.  The iMON OEM LCD screen excepts 8-byte
 * packets. We accept data as 16 hexadecimal digits, followed by a
 * newline (to make it easy to drive the device from a command-line
 * -- even though the actual binary data is a bit complicated).
 *
 * The device itself is not a "traditional" text-mode display. It's
 * actually a 16x96 pixel bitmap display. That means if you want to
 * display text, you've got to have your own "font" and translate the
 * text into bitmaps for display. This is really flexible (you can
 * display whatever diacritics you need, and so on), but it's also
 * a lot more complicated than most LCDs...
 */
static ssize_t lcd_write(struct file *file, const char *buf,
			 size_t n_bytes, loff_t *pos)
{
	int retval = 0;
	struct imon_context *context;

	context = (struct imon_context *)file->private_data;
	if (!context) {
		err("%s: no context for device", __func__);
		return -ENODEV;
	}

	mutex_lock(&context->lock);

	if (!context->display_supported) {
		err("%s: no iMON display present", __func__);
		retval = -ENODEV;
		goto exit;
	}

	if (n_bytes != 8) {
		err("%s: invalid payload size: %d (expecting 8)",
		    __func__, (int) n_bytes);
		retval = -EINVAL;
		goto exit;
	}

	if (copy_from_user(context->usb_tx_buf, buf, 8)) {
		retval = -EFAULT;
		goto exit;
	}

	retval = send_packet(context);
	if (retval) {
		err("%s: send packet failed!", __func__);
		goto exit;
	} else {
		dprintk("%s: write %d bytes to LCD\n", __func__, (int) n_bytes);
	}
exit:
	mutex_unlock(&context->lock);
	return (!retval) ? n_bytes : retval;
}

/**
 * Callback function for USB core API: transmit data
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static void usb_tx_callback(struct urb *urb, struct pt_regs *regs)
#else
static void usb_tx_callback(struct urb *urb)
#endif
{
	struct imon_context *context;

	if (!urb)
		return;
	context = (struct imon_context *)urb->context;
	if (!context)
		return;

	context->tx.status = urb->status;

	/* notify waiters that write has finished */
	atomic_set(&context->tx.busy, 0);
	complete(&context->tx.finished);

	return;
}

/**
 * iMON IR receivers support two different signal sets -- those used by
 * the iMON remotes, and those used by the Windows MCE remotes (which is
 * really just RC-6), but only one or the other at a time, as the signals
 * are decoded onboard the receiver.
 */
static void imon_set_ir_protocol(struct imon_context *context)
{
	int retval;
	unsigned char ir_proto_packet[] =
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x86 };

	/* not supported on devices that don't do onboard decoding */
	if (!context->ir_onboard_decode)
		return;

	switch (ir_protocol) {
	case IMON_IR_PROTOCOL_MCE:
		dprintk("Configuring IR receiver for MCE protocol\n");
		ir_proto_packet[0] = 0x01;
		context->ir_protocol = IMON_IR_PROTOCOL_MCE;
		break;
	case IMON_IR_PROTOCOL_IMON:
		dprintk("Configuring IR receiver for iMON protocol\n");
		/* ir_proto_packet[0] = 0x00; // already the default */
		context->ir_protocol = IMON_IR_PROTOCOL_IMON;
		break;
	default:
		printk(KERN_INFO "%s: unknown IR protocol specified, will "
		       "just default to iMON protocol\n", __func__);
		context->ir_protocol = IMON_IR_PROTOCOL_MCE;
		break;
	}
	memcpy(context->usb_tx_buf, &ir_proto_packet,
	       sizeof(ir_proto_packet));
	retval = send_packet(context);
	if (retval)
		printk(KERN_INFO "%s: failed to set remote type\n", __func__);
}


/**
 * Called by lirc_dev when the application opens /dev/lirc
 */
static int ir_open(void *data)
{
	int retval = 0;
	struct imon_context *context;

	/* prevent races with disconnect */
	mutex_lock(&driver_lock);

	context = (struct imon_context *)data;

	/* initial IR protocol decode variables */
	context->rx.count = 0;
	context->rx.initial_space = 1;
	context->rx.prev_bit = 0;

	/* set new IR protocol if it has changed since init or last open */
	if (ir_protocol != context->ir_protocol)
		imon_set_ir_protocol(context);

	context->ir_isopen = 1;
	printk(KERN_INFO MOD_NAME ": IR port opened\n");

	mutex_unlock(&driver_lock);
	return retval;
}

/**
 * Called by lirc_dev when the application closes /dev/lirc
 */
static void ir_close(void *data)
{
	struct imon_context *context;

	context = (struct imon_context *)data;
	if (!context) {
		err("%s: no context for device", __func__);
		return;
	}

	mutex_lock(&context->lock);

	context->ir_isopen = 0;
	context->ir_isassociating = 0;
	MOD_DEC_USE_COUNT;
	printk(KERN_INFO MOD_NAME ": IR port closed\n");

	if (!context->dev_present_intf0) {
		/*
		 * Device disconnected while IR port was still open. Driver
		 * was not deregistered at disconnect time, so do it now.
		 */
		deregister_from_lirc(context);

		if (!context->display_isopen) {
			mutex_unlock(&context->lock);
			free_imon_context(context);
			return;
		}
		/*
		 * If display port is open, context will be deleted by
		 * display_close
		 */
	}

	mutex_unlock(&context->lock);
	return;
}

/**
 * Convert bit count to time duration (in us) and submit
 * the value to lirc_dev.
 */
static void submit_data(struct imon_context *context)
{
	unsigned char buf[4];
	int value = context->rx.count;
	int i;

	dprintk("submitting data to LIRC\n");

	value *= BIT_DURATION;
	value &= PULSE_MASK;
	if (context->rx.prev_bit)
		value |= PULSE_BIT;

	for (i = 0; i < 4; ++i)
		buf[i] = value>>(i*8);

	lirc_buffer_write(context->driver->rbuf, buf);
	wake_up(&context->driver->rbuf->wait_poll);
	return;
}

static inline int tv2int(const struct timeval *a, const struct timeval *b)
{
	int usecs = 0;
	int sec   = 0;

	if (b->tv_usec > a->tv_usec) {
		usecs = 1000000;
		sec--;
	}

	usecs += a->tv_usec - b->tv_usec;

	sec += a->tv_sec - b->tv_sec;
	sec *= 1000;
	usecs /= 1000;
	sec += usecs;

	if (sec < 0)
		sec = 1000;

	return sec;
}

/**
 * The directional pad is overly sensitive in keyboard mode, so we do some
 * interesting contortions to make it less touchy.
 */
#define IMON_PAD_TIMEOUT	1000	/* in msecs */
#define IMON_PAD_THRESHOLD	80	/* 160x160 square */
static int stabilize(int a, int b)
{
	struct timeval ct;
	static struct timeval prev_time = {0, 0};
	static struct timeval hit_time  = {0, 0};
	static int x, y, prev_result, hits;
	int result = 0;
	int msec, msec_hit;

	do_gettimeofday(&ct);
	msec = tv2int(&ct, &prev_time);
	msec_hit = tv2int(&ct, &hit_time);

	if (msec > 100) {
		x = 0;
		y = 0;
		hits = 0;
	}

	x += a;
	y += b;

	prev_time = ct;

	if (abs(x) > IMON_PAD_THRESHOLD || abs(y) > IMON_PAD_THRESHOLD) {
		if (abs(y) > abs(x))
			result = (y > 0) ? 0x7F : 0x80;
		else
			result = (x > 0) ? 0x7F00 : 0x8000;

		x = 0;
		y = 0;

		if (result == prev_result) {
			hits++;

			if (hits > 3) {
				switch (result) {
				case 0x7F:
					y = 17 * IMON_PAD_THRESHOLD / 30;
					break;
				case 0x80:
					y -= 17 * IMON_PAD_THRESHOLD / 30;
					break;
				case 0x7F00:
					x = 17 * IMON_PAD_THRESHOLD / 30;
					break;
				case 0x8000:
					x -= 17 * IMON_PAD_THRESHOLD / 30;
					break;
				}
			}

			if (hits == 2 && msec_hit < IMON_PAD_TIMEOUT) {
				result = 0;
				hits = 1;
			}
		} else {
			prev_result = result;
			hits = 1;
			hit_time = ct;
		}
	}

	return result;
}

/**
 * Process the incoming packet
 */
static void imon_incoming_packet(struct imon_context *context,
				 struct urb *urb, int intf)
{
	int len = urb->actual_length;
	unsigned char *buf = urb->transfer_buffer;
	char rel_x = 0x00, rel_y = 0x00;
	int octet, bit;
	unsigned char mask;
	int i, chunk_num, dir;
	int ts_input = 0;
	int mouse_input;
	struct input_dev *mouse = NULL;
	struct input_dev *touch = NULL;
	const unsigned char toggle_button1[] = { 0x29, 0x91, 0x15, 0xb7 };
	const unsigned char toggle_button2[] = { 0x29, 0x91, 0x35, 0xb7 };
	const unsigned char ch_up[]   = { 0x28, 0x93, 0x95, 0xb7 };
	const unsigned char ch_down[] = { 0x28, 0x87, 0x95, 0xb7 };
	const unsigned char btn_left[]  = { 0x68, 0x83, 0x01, 0xb7 };
	const unsigned char btn_right[] = { 0x68, 0x84, 0x81, 0xb7 };

	mouse = context->mouse;
	if (context->has_touchscreen)
		touch = context->touch;

	/* keyboard/mouse mode toggle button */
	if (memcmp(buf, toggle_button1, 4) == 0 ||
	    memcmp(buf, toggle_button2, 4) == 0) {
		if (!nomouse) {
			context->pad_mouse = ~(context->pad_mouse) & 0x1;
			dprintk("toggling to %s mode\n",
				context->pad_mouse ? "mouse" : "keyboard");
		} else {
			context->pad_mouse = 0;
			dprintk("mouse mode was disabled by modparam\n");
		}
		return;
	}

	/* send touchscreen events through input subsystem if touchpad data */
	if (context->has_touchscreen && len == 8 &&
	    (buf[6] == 0x14 || buf[6] == 0x03) && buf[7] == 0x86) {
		if (touch == NULL) {
			printk(KERN_WARNING "%s: touchscreen input device is "
			       "NULL!\n", __func__);
			return;
		}
		mod_timer(&context->timer, jiffies + TOUCH_TIMEOUT);
		context->touch_x = (buf[0] << 4) | (buf[1] >> 4);
		context->touch_y = 0xfff - ((buf[2] << 4) | (buf[1] & 0xf));
		input_report_abs(touch, ABS_X, context->touch_x);
		input_report_abs(touch, ABS_Y, context->touch_y);
		input_report_key(touch, BTN_TOUCH, 0x01);
		input_sync(touch);
		ts_input = 1;

	/* send mouse events through input subsystem in mouse mode */
	} else if (context->pad_mouse || !context->ir_isopen) {
		/* newer iMON device PAD or mouse button */
		if (!context->ffdc_dev && (buf[0] & 0x01) && len == 5) {
			mouse_input = 1;
			rel_x = buf[2];
			rel_y = buf[3];
		/* 0xffdc iMON PAD input */
		} else if (context->ffdc_dev && (buf[0] & 0x40) &&
			   !((buf[1] & 0x01) || ((buf[1] >> 2) & 0x01))) {
			mouse_input = 1;
			rel_x = (buf[1] & 0x08) | (buf[1] & 0x10) >> 2 |
				(buf[1] & 0x20) >> 4 | (buf[1] & 0x40) >> 6;
			if (buf[0] & 0x02)
				rel_x |= ~0x11;
			rel_x = rel_x + rel_x / 2;
			rel_y = (buf[2] & 0x08) | (buf[2] & 0x10) >> 2 |
				(buf[2] & 0x20) >> 4 | (buf[2] & 0x40) >> 6;
			if (buf[0] & 0x01)
				rel_y |= ~0x11;
			rel_y = rel_y + rel_y / 2;
		/* 0xffdc mouse buttons */
		} else if (context->ffdc_dev && !memcmp(buf, btn_left, 4)) {
			mouse_input = 1;
			buf[1] = 0x01;
		} else if (context->ffdc_dev && !memcmp(buf, btn_right, 4)) {
			mouse_input = 1;
			buf[1] = 0x02;
		/* ch+/- buttons, which we use for an emulated scroll wheel */
		} else if (!memcmp(buf, ch_up, 4) || !memcmp(buf, ch_down, 4))
			mouse_input = 1;
		else
			mouse_input = 0;

		if (mouse_input) {
			if (mouse == NULL) {
				printk(KERN_WARNING "%s: mouse input device "
				       "is NULL!\n", __func__);
				return;
			}
			dprintk("sending mouse data via input subsystem\n");
			if (!memcmp(buf, ch_up, 4))
				dir = 1;
			else if (!memcmp(buf, ch_down, 4))
				dir = -1;
			else
				dir = 0;

			if (dir == 0) {
				input_report_key(mouse, BTN_LEFT,
						 buf[1] & 0x01);
				input_report_key(mouse, BTN_RIGHT,
						 buf[1] >> 1 & 0x01);
				input_report_rel(mouse, REL_X, rel_x);
				input_report_rel(mouse, REL_Y, rel_y);
			} else
				input_report_rel(mouse, REL_WHEEL, dir);
			input_sync(mouse);
			return;
		}
	}

	/*
	 * at this point, mouse and touchscreen input has been handled, so
	 * anything else goes to lirc -- bail out if no listening IR client
	 */
	if (!context->ir_isopen)
		return;

	/*
	 * we need to add some special handling for
	 * the imon's IR mouse events
	 */
	if ((len == 5) && (buf[0] == 0x01) && (buf[4] == 0x00)) {
		/* first, pad to 8 bytes so it conforms with everything else */
		buf[5] = buf[6] = buf[7] = 0;
		len = 8;
		rel_x = buf[2];
		rel_y = buf[3];

		/*
		 * the imon directional pad functions more like a touchpad.
		 * Bytes 3 & 4 contain a position coordinate (x,y), with each
		 * component ranging from -14 to 14. Since this doesn't
		 * cooperate well with the way lirc works (it would appear to
		 * lirc as more than 100 different buttons) we need to map it
		 * to 4 discrete values. Also, when you get too close to
		 * diagonals, it has a tendancy to jump back and forth, so lets
		 * try to ignore when they get too close
		 */
		if ((buf[1] == 0) && ((rel_x != 0) || (rel_y != 0))) {
			dir = stabilize((int)rel_x, (int)rel_y);
			if (!dir)
				return;
			buf[2] = dir & 0xFF;
			buf[3] = (dir >> 8) & 0xFF;
		}

	} else if ((len == 8) && (buf[0] & 0x40) &&
		   !(buf[1] & 0x01 || buf[1] >> 2 & 0x01)) {
		/*
		 * Handle on-board decoded pad events for e.g. older
		 * VFD/iMON-Pad (15c2:ffdc). The remote generates various codes
		 * from 0x68nnnnB7 to 0x6AnnnnB7, the left mouse button
		 * generates 0x688301b7 and the right one 0x688481b7. All other
		 * keys generate 0x2nnnnnnn. Length has been padded to 8
		 * already, position coordinate is encoded in buf[1] and buf[2]
		 * with reversed endianess. Extract direction from buffer,
		 * rotate endianess, adjust sign and feed the values into
		 * stabilize(). The resulting codes will be 0x01008000,
		 * 0x01007F00, ..., so one can use the normal imon-pad config
		 * from the remotes dir.
		 */

		/* buf[1] is x */
		rel_x = (buf[1] & 0x08) | (buf[1] & 0x10) >> 2 |
			(buf[1] & 0x20) >> 4 | (buf[1] & 0x40) >> 6;
		if(buf[0] & 0x02)
			rel_x |= ~0x10+1;
		/* buf[2] is y */
		rel_y = (buf[2] & 0x08) | (buf[2] & 0x10) >> 2 |
			(buf[2] & 0x20) >> 4 | (buf[2] & 0x40) >> 6;
		if(buf[0] & 0x01)
			rel_y |= ~0x10+1;

		buf[0] = 0x01;
		buf[1] = buf[4] = buf[5] = buf[6] = buf[7] = 0;

		dir = stabilize((int)rel_x, (int)rel_y);
		if (!dir)
			return;
		buf[2] = dir & 0xFF;
		buf[3] = (dir >> 8) & 0xFF;

	} else if (ts_input) {
		/*
		 * this is touchscreen input, which we need to down-sample
		 * to a 64 button matrix at the moment...
		 */
		buf[0] = buf[0] >> 5;
		buf[1] = 0x00;
		buf[2] = buf[2] >> 5;
		buf[3] = 0x00;
		buf[4] = 0x00;
		buf[5] = 0x00;
		buf[6] = 0x14;
		buf[7] = 0xff;
	}

	if (len != 8) {
		printk(KERN_WARNING "imon %s: invalid incoming packet "
		       "size (len = %d, intf%d)\n", __func__, len, intf);
		return;
	}

	/* iMON 2.4G associate frame */
	if (buf[0] == 0x00 &&
	    buf[2] == 0xFF &&				/* REFID */
	    buf[3] == 0xFF &&
	    buf[4] == 0xFF &&
	    buf[5] == 0xFF &&				/* iMON 2.4G */
	   ((buf[6] == 0x4E && buf[7] == 0xDF) ||	/* LT */
	    (buf[6] == 0x5E && buf[7] == 0xDF))) {	/* DT */
		printk(KERN_WARNING "%s: remote associated refid=%02X\n",
		       __func__, buf[1]);
		context->ir_isassociating = 0;
	}

	chunk_num = buf[7];

	if (chunk_num == 0xFF && !ts_input)
		return;		/* filler frame, no data here */

	if (buf[0] == 0xFF &&
	    buf[1] == 0xFF &&
	    buf[2] == 0xFF &&
	    buf[3] == 0xFF &&
	    buf[4] == 0xFF &&
	    buf[5] == 0xFF &&				/* iMON 2.4G */
	    ((buf[6] == 0x4E && buf[7] == 0xAF) ||	/* LT */
	     (buf[6] == 0x5E && buf[7] == 0xAF)))	/* DT */
		return;		/* filler frame, no data here */

	if (debug) {
		if (context->ir_onboard_decode)
			printk("intf%d decoded packet: ", intf);
		else
			printk("raw packet: ");
		for (i = 0; i < len; ++i)
			printk("%02x ", buf[i]);
		printk("\n");
	}

	if (context->ir_onboard_decode) {
		/* The signals have been decoded onboard the iMON controller */
		lirc_buffer_write(context->driver->rbuf, buf);
		wake_up(&context->driver->rbuf->wait_poll);
		return;
	}

	/*
	 * Translate received data to pulse and space lengths.
	 * Received data is active low, i.e. pulses are 0 and
	 * spaces are 1.
	 *
	 * My original algorithm was essentially similar to
	 * Changwoo Ryu's with the exception that he switched
	 * the incoming bits to active high and also fed an
	 * initial space to LIRC at the start of a new sequence
	 * if the previous bit was a pulse.
	 *
	 * I've decided to adopt his algorithm.
	 */

	if (chunk_num == 1 && context->rx.initial_space) {
		/* LIRC requires a leading space */
		context->rx.prev_bit = 0;
		context->rx.count = 4;
		submit_data(context);
		context->rx.count = 0;
	}

	for (octet = 0; octet < 5; ++octet) {
		mask = 0x80;
		for (bit = 0; bit < 8; ++bit) {
			int curr_bit = !(buf[octet] & mask);
			if (curr_bit != context->rx.prev_bit) {
				if (context->rx.count) {
					submit_data(context);
					context->rx.count = 0;
				}
				context->rx.prev_bit = curr_bit;
			}
			++context->rx.count;
			mask >>= 1;
		}
	}

	if (chunk_num == 10) {
		if (context->rx.count) {
			submit_data(context);
			context->rx.count = 0;
		}
		context->rx.initial_space = context->rx.prev_bit;
	}
}

/**
 * report touchscreen input
 */
static void imon_touch_display_timeout(unsigned long data)
{
	struct imon_context *context = (struct imon_context *)data;
	struct input_dev *touch;

	if (!context->has_touchscreen)
		return;

	touch = context->touch;
	input_report_abs(touch, ABS_X, context->touch_x);
	input_report_abs(touch, ABS_Y, context->touch_y);
	input_report_key(touch, BTN_TOUCH, 0x00);
	input_sync(touch);

	return;
}

/**
 * Callback function for USB core API: receive data
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static void usb_rx_callback_intf0(struct urb *urb, struct pt_regs *regs)
#else
static void usb_rx_callback_intf0(struct urb *urb)
#endif
{
	struct imon_context *context;
	unsigned char *buf;
	int len;
	int intfnum = 0;

	if (!urb)
		return;

	context = (struct imon_context *)urb->context;
	if (!context)
		return;

	buf = urb->transfer_buffer;
	len = urb->actual_length;

	switch (urb->status) {
	case -ENOENT:		/* usbcore unlink successful! */
		return;

	case 0:
		imon_incoming_packet(context, urb, intfnum);
		break;

	default:
		printk(KERN_WARNING "imon %s: status(%d): ignored\n",
		       __func__, urb->status);
		break;
	}

	usb_submit_urb(context->rx_urb_intf0, GFP_ATOMIC);

	return;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static void usb_rx_callback_intf1(struct urb *urb, struct pt_regs *regs)
#else
static void usb_rx_callback_intf1(struct urb *urb)
#endif
{
	struct imon_context *context;
	unsigned char *buf;
	int len;
	int intfnum = 1;

	if (!urb)
		return;

	context = (struct imon_context *)urb->context;
	if (!context)
		return;

	buf = urb->transfer_buffer;
	len = urb->actual_length;

	switch (urb->status) {
	case -ENOENT:		/* usbcore unlink successful! */
		return;

	case 0:
		imon_incoming_packet(context, urb, intfnum);
		break;

	default:
		printk(KERN_WARNING "imon %s: status(%d): ignored\n",
		       __func__, urb->status);
		break;
	}

	usb_submit_urb(context->rx_urb_intf1, GFP_ATOMIC);

	return;
}

/**
 * Callback function for USB core API: Probe
 */
static int imon_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	struct usb_device *usbdev = NULL;
	struct usb_host_interface *iface_desc = NULL;
	struct usb_endpoint_descriptor *rx_endpoint = NULL;
	struct usb_endpoint_descriptor *tx_endpoint = NULL;
	struct urb *rx_urb = NULL;
	struct urb *tx_urb = NULL;
	struct lirc_driver *driver = NULL;
	struct lirc_buffer *rbuf = NULL;
	struct usb_interface *first_if;
	int ifnum;
	int lirc_minor = 0;
	int num_endpts;
	int retval = 0;
	int display_ep_found = 0;
	int ir_ep_found = 0;
	int alloc_status = 0;
	int vfd_proto_6p = 0;
	int ir_onboard_decode = 0;
	int has_touchscreen = 0;
	int buf_chunk_size = BUF_CHUNK_SIZE;
	int code_length;
	int tx_control = 0;
	struct imon_context *context = NULL;
	struct imon_context *first_if_context = NULL;
	int i;
	u16 vendor, product;
	const unsigned char fp_packet[] = { 0x40, 0x00, 0x00, 0x00,
					    0x00, 0x00, 0x00, 0x88 };

	/*
	 * If it's the LCD, as opposed to the VFD, we just need to replace
	 * the "write" file op.
	 */
	if ((display_type == IMON_DISPLAY_TYPE_AUTO &&
	     usb_match_id(interface, lcd_device_list)) ||
	    display_type == IMON_DISPLAY_TYPE_LCD)
		display_fops.write = &lcd_write;

	/*
	 * To get front panel buttons working properly for newer LCD devices,
	 * we really do need a larger buffer.
	 */
	if (usb_match_id(interface, large_buffer_list))
		buf_chunk_size = 2 * BUF_CHUNK_SIZE;

	code_length = buf_chunk_size * 8;

	usbdev     = usb_get_dev(interface_to_usbdev(interface));
	iface_desc = interface->cur_altsetting;
	num_endpts = iface_desc->desc.bNumEndpoints;
	ifnum      = iface_desc->desc.bInterfaceNumber;
	vendor     = le16_to_cpu(usbdev->descriptor.idVendor);
	product    = le16_to_cpu(usbdev->descriptor.idProduct);

	dprintk("%s: found iMON device (%04x:%04x, intf%d)\n",
		__func__, vendor, product, ifnum);

	/* prevent races probing devices w/multiple interfaces */
	mutex_lock(&driver_lock);

	first_if = usb_ifnum_to_if(usbdev, 0);
	first_if_context = (struct imon_context *)usb_get_intfdata(first_if);

	/*
	 * Scan the endpoint list and set:
	 *	first input endpoint = IR endpoint
	 *	first output endpoint = display endpoint
	 */
	for (i = 0; i < num_endpts && !(ir_ep_found && display_ep_found); ++i) {
		struct usb_endpoint_descriptor *ep;
		int ep_dir;
		int ep_type;
		ep = &iface_desc->endpoint[i].desc;
		ep_dir = ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK;
		ep_type = ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;

		if (!ir_ep_found &&
			ep_dir == USB_DIR_IN &&
			ep_type == USB_ENDPOINT_XFER_INT) {

			rx_endpoint = ep;
			ir_ep_found = 1;
			dprintk("%s: found IR endpoint\n", __func__);

		} else if (!display_ep_found &&
			   ep_dir == USB_DIR_OUT &&
			   ep_type == USB_ENDPOINT_XFER_INT) {
			tx_endpoint = ep;
			display_ep_found = 1;
			dprintk("%s: found display endpoint\n", __func__);
		}
	}

	/*
	 * If we didn't find a display endpoint, this is probably one of the
	 * newer iMON devices that use control urb instead of interrupt
	 */
	if (!display_ep_found) {
		if (usb_match_id(interface, ctl_ep_device_list)) {
			tx_control = 1;
			display_ep_found = 1;
			dprintk("%s: device uses control endpoint, not "
				"interface OUT endpoint\n", __func__);
		}
	}

	/*
	 * Some iMON receivers have no display. Unfortunately, it seems
	 * that SoundGraph recycles device IDs between devices both with
	 * and without... :\
	 */
	if ((display_type == IMON_DISPLAY_TYPE_AUTO &&
	     usb_match_id(interface, ir_only_list)) ||
	    display_type == IMON_DISPLAY_TYPE_NONE) {
		display_ep_found = 0;
		dprintk("%s: device has no display\n", __func__);
	}

	/*
	 * iMON Touch devices have a VGA touchscreen, but no "display", as
	 * that refers to e.g. /dev/lcd0 (a character device LCD or VFD).
	 */
	if ((display_type == IMON_DISPLAY_TYPE_AUTO &&
	     usb_match_id(interface, imon_touchscreen_list)) ||
	    display_type == IMON_DISPLAY_TYPE_VGA) {
		display_ep_found = 0;
		has_touchscreen = 1;
		dprintk("%s: iMON Touch device found\n", __func__);
	}

	/* Input endpoint is mandatory */
	if (!ir_ep_found) {
		err("%s: no valid input (IR) endpoint found.", __func__);
		retval = -ENODEV;
		goto exit;
	} else {
		/* Determine if the IR signals are decoded onboard */
		if (usb_match_id(interface, ir_onboard_decode_list))
			ir_onboard_decode = 1;

		dprintk("%s: ir_onboard_decode: %d\n",
			__func__, ir_onboard_decode);
	}

	/* Determine if display requires 6 packets */
	if (display_ep_found) {
		if (usb_match_id(interface, vfd_proto_6p_list))
			vfd_proto_6p = 1;

		dprintk("%s: vfd_proto_6p: %d\n",
			__func__, vfd_proto_6p);
	}

	if (ifnum == 0) {
		context = kzalloc(sizeof(struct imon_context), GFP_KERNEL);
		if (!context) {
			err("%s: kzalloc failed for context", __func__);
			alloc_status = 1;
			goto alloc_status_switch;
		}
		driver = kzalloc(sizeof(struct lirc_driver), GFP_KERNEL);
		if (!driver) {
			err("%s: kzalloc failed for lirc_driver", __func__);
			alloc_status = 2;
			goto alloc_status_switch;
		}
		rbuf = kmalloc(sizeof(struct lirc_buffer), GFP_KERNEL);
		if (!rbuf) {
			err("%s: kmalloc failed for lirc_buffer", __func__);
			alloc_status = 3;
			goto alloc_status_switch;
		}
		if (lirc_buffer_init(rbuf, buf_chunk_size, BUF_SIZE)) {
			err("%s: lirc_buffer_init failed", __func__);
			alloc_status = 4;
			goto alloc_status_switch;
		}
		rx_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!rx_urb) {
			err("%s: usb_alloc_urb failed for IR urb", __func__);
			alloc_status = 5;
			goto alloc_status_switch;
		}
		tx_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!tx_urb) {
			err("%s: usb_alloc_urb failed for display urb",
			    __func__);
			alloc_status = 6;
			goto alloc_status_switch;
		}

		mutex_init(&context->lock);
		context->vfd_proto_6p = vfd_proto_6p;
		context->ir_onboard_decode = ir_onboard_decode;

		strcpy(driver->name, MOD_NAME);
		driver->minor = -1;
		driver->code_length = ir_onboard_decode ?
			code_length : sizeof(int) * 8;
		driver->sample_rate = 0;
		driver->features = (ir_onboard_decode) ?
			LIRC_CAN_REC_LIRCCODE : LIRC_CAN_REC_MODE2;
		driver->data = context;
		driver->rbuf = rbuf;
		driver->set_use_inc = ir_open;
		driver->set_use_dec = ir_close;
#ifdef LIRC_HAVE_SYSFS
		driver->dev = &interface->dev;
#endif
		driver->owner = THIS_MODULE;

		mutex_lock(&context->lock);

		context->driver = driver;
		/* start out in keyboard mode */
		context->pad_mouse = 0;

		init_timer(&context->timer);
		context->timer.data = (unsigned long)context;
		context->timer.function = imon_touch_display_timeout;

		lirc_minor = lirc_register_driver(driver);
		if (lirc_minor < 0) {
			err("%s: lirc_register_driver failed", __func__);
			alloc_status = 7;
			goto alloc_status_switch;
		} else
			printk(KERN_INFO MOD_NAME ": Registered iMON driver "
			       "(lirc minor: %d)\n", lirc_minor);

		/* Needed while unregistering! */
		driver->minor = lirc_minor;

	} else {
	/* this is the secondary interface on the device */
		if (first_if_context->driver) {
			rx_urb = usb_alloc_urb(0, GFP_KERNEL);
			if (!rx_urb) {
				err("%s: usb_alloc_urb failed for IR urb",
				    __func__);
				alloc_status = 5;
				goto alloc_status_switch;
			}

			context = first_if_context;
		}
		mutex_lock(&context->lock);
	}

	if (ifnum == 0) {
		context->usbdev_intf0 = usbdev;
		context->dev_present_intf0 = 1;
		context->rx_endpoint_intf0 = rx_endpoint;
		context->rx_urb_intf0 = rx_urb;

		/*
		 * tx is used to send characters to lcd/vfd, associate RF
		 * remotes, set IR protocol, and maybe more...
		 */
		context->tx_endpoint = tx_endpoint;
		context->tx_urb = tx_urb;
		context->tx_control = tx_control;

		if (display_ep_found)
			context->display_supported = 1;

		if (product == 0xffdc)
			context->ffdc_dev = 1;

		context->has_touchscreen = has_touchscreen;

		context->mouse = input_allocate_device();

		snprintf(context->name_mouse, sizeof(context->name_mouse),
			 "iMON PAD IR Mouse (%04x:%04x)",
			 vendor, product);
		context->mouse->name = context->name_mouse;

		usb_make_path(usbdev, context->phys_mouse, sizeof(context->phys_mouse));
		strlcat(context->phys_mouse, "/input0", sizeof(context->phys_mouse));
		context->mouse->phys = context->phys_mouse;

		context->mouse->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
		context->mouse->keybit[BIT_WORD(BTN_MOUSE)] =
			BIT_MASK(BTN_LEFT) | BIT_MASK(BTN_RIGHT) |
			BIT_MASK(BTN_MIDDLE) | BIT_MASK(BTN_SIDE) |
			BIT_MASK(BTN_EXTRA);
		context->mouse->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y) |
			BIT_MASK(REL_WHEEL);

		input_set_drvdata(context->mouse, context);

		usb_to_input_id(usbdev, &context->mouse->id);
		context->mouse->dev.parent = &interface->dev;
		retval = input_register_device(context->mouse);
		if (retval)
			printk(KERN_INFO "%s: pad mouse input device setup failed\n",
			       __func__);

		usb_fill_int_urb(context->rx_urb_intf0, context->usbdev_intf0,
			usb_rcvintpipe(context->usbdev_intf0,
					context->rx_endpoint_intf0->bEndpointAddress),
			context->usb_rx_buf, sizeof(context->usb_rx_buf),
			usb_rx_callback_intf0, context,
			context->rx_endpoint_intf0->bInterval);

		retval = usb_submit_urb(context->rx_urb_intf0, GFP_KERNEL);

		if (retval) {
			err("%s: usb_submit_urb failed for intf0 (%d)",
			    __func__, retval);
			mutex_unlock(&context->lock);
			goto exit;
		}

	} else {
		context->usbdev_intf1 = usbdev;
		context->dev_present_intf1 = 1;
		context->rx_endpoint_intf1 = rx_endpoint;
		context->rx_urb_intf1 = rx_urb;

		if (context->has_touchscreen) {
			context->touch = input_allocate_device();

			snprintf(context->name_touch, sizeof(context->name_touch),
				 "iMON USB Touchscreen (%04x:%04x)",
				 vendor, product);
			context->touch->name = context->name_touch;

			usb_make_path(usbdev, context->phys_touch,
				      sizeof(context->phys_touch));
			strlcat(context->phys_touch, "/input1",
				sizeof(context->phys_touch));
			context->touch->phys = context->phys_touch;

			context->touch->evbit[0] =
				BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
			context->touch->keybit[BIT_WORD(BTN_TOUCH)] =
				BIT_MASK(BTN_TOUCH);
			input_set_abs_params(context->touch, ABS_X,
					     0x00, 0xfff, 0, 0);
			input_set_abs_params(context->touch, ABS_Y,
					     0x00, 0xfff, 0, 0);

			input_set_drvdata(context->touch, context);

			usb_to_input_id(usbdev, &context->touch->id);
			context->touch->dev.parent = &interface->dev;
			retval = input_register_device(context->touch);
			if (retval)
				printk(KERN_INFO "%s: touchscreen input device setup failed\n",
				       __func__);
		} else
			context->touch = NULL;

		usb_fill_int_urb(context->rx_urb_intf1, context->usbdev_intf1,
			usb_rcvintpipe(context->usbdev_intf1,
				context->rx_endpoint_intf1->bEndpointAddress),
			context->usb_rx_buf, sizeof(context->usb_rx_buf),
			usb_rx_callback_intf1, context,
			context->rx_endpoint_intf1->bInterval);

		retval = usb_submit_urb(context->rx_urb_intf1, GFP_KERNEL);

		if (retval) {
			err("%s: usb_submit_urb failed for intf1 (%d)",
			    __func__, retval);
			mutex_unlock(&context->lock);
			goto exit;
		}
	}

	usb_set_intfdata(interface, context);

	/* RF products *also* use 0xffdc... sigh... */
	if (context->ffdc_dev) {
		int err;

		err = sysfs_create_group(&interface->dev.kobj,
					 &imon_attribute_group);
		if (err)
			err("%s: Could not create sysfs entries(%d)",
			    __func__, err);
	}

	if (context->display_supported && ifnum == 0) {
		dprintk("%s: Registering iMON display with sysfs\n", __func__);
		if (usb_register_dev(interface, &imon_class)) {
			/* Not a fatal error, so ignore */
			printk(KERN_INFO "%s: could not get a minor number for "
			       "display\n", __func__);
		}

		/* Enable front-panel buttons and/or knobs */
		memcpy(context->usb_tx_buf, &fp_packet, sizeof(fp_packet));
		retval = send_packet(context);
		/* Not fatal, but warn about it */
		if (retval)
			printk(KERN_INFO "%s: failed to enable front-panel "
			       "buttons and/or knobs\n", __func__);
	}

	/* set IR protocol/remote type */
	imon_set_ir_protocol(context);

	printk(KERN_INFO MOD_NAME ": iMON device (%04x:%04x, intf%d) on "
	       "usb<%d:%d> initialized\n", vendor, product, ifnum,
	       usbdev->bus->busnum, usbdev->devnum);

alloc_status_switch:
	mutex_unlock(&context->lock);

	switch (alloc_status) {
	case 7:
		usb_free_urb(tx_urb);
	case 6:
		usb_free_urb(rx_urb);
	case 5:
		if (rbuf)
			lirc_buffer_free(rbuf);
	case 4:
		kfree(rbuf);
	case 3:
		kfree(driver);
	case 2:
		kfree(context);
		context = NULL;
	case 1:
		retval = -ENOMEM;
		break;
	case 0:
		retval = 0;
	}

exit:
	mutex_unlock(&driver_lock);

	return retval;
}

/**
 * Callback function for USB core API: disconnect
 */
static void imon_disconnect(struct usb_interface *interface)
{
	struct imon_context *context;
	int ifnum;

	/* prevent races with ir_open()/display_open() */
	mutex_lock(&driver_lock);

	context = usb_get_intfdata(interface);
	ifnum = interface->cur_altsetting->desc.bInterfaceNumber;

	mutex_lock(&context->lock);

	/*
	 * sysfs_remove_group is safe to call even if sysfs_create_group
	 * hasn't been called
	 */
	sysfs_remove_group(&interface->dev.kobj,
			   &imon_attribute_group);

	usb_set_intfdata(interface, NULL);

	/* Abort ongoing write */
	if (atomic_read(&context->tx.busy)) {
		usb_kill_urb(context->tx_urb);
		complete_all(&context->tx.finished);
	}

	if (ifnum == 0) {
		context->dev_present_intf0 = 0;
		usb_kill_urb(context->rx_urb_intf0);
		input_unregister_device(context->mouse);
		if (context->display_supported)
			usb_deregister_dev(interface, &imon_class);
	} else {
		context->dev_present_intf1 = 0;
		usb_kill_urb(context->rx_urb_intf1);
		if (context->has_touchscreen)
			input_unregister_device(context->touch);
	}

	if (!context->ir_isopen && !context->dev_present_intf0 &&
	    !context->dev_present_intf1) {
		del_timer_sync(&context->timer);
		deregister_from_lirc(context);
		mutex_unlock(&context->lock);
		if (!context->display_isopen)
			free_imon_context(context);
	} else
		mutex_unlock(&context->lock);

	mutex_unlock(&driver_lock);

	printk(KERN_INFO "%s: iMON device (intf%d) disconnected\n",
	       __func__, ifnum);
}

static int imon_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct imon_context *context = usb_get_intfdata(intf);
	int ifnum = intf->cur_altsetting->desc.bInterfaceNumber;

	if (ifnum == 0)
		usb_kill_urb(context->rx_urb_intf0);
	else
		usb_kill_urb(context->rx_urb_intf1);

	return 0;
}

static int imon_resume(struct usb_interface *intf)
{
	int rc = 0;
	struct imon_context *context = usb_get_intfdata(intf);
	int ifnum = intf->cur_altsetting->desc.bInterfaceNumber;

	if (ifnum == 0) {
		usb_fill_int_urb(context->rx_urb_intf0, context->usbdev_intf0,
			usb_rcvintpipe(context->usbdev_intf0,
					context->rx_endpoint_intf0->bEndpointAddress),
			context->usb_rx_buf, sizeof(context->usb_rx_buf),
			usb_rx_callback_intf0, context,
			context->rx_endpoint_intf0->bInterval);

		rc = usb_submit_urb(context->rx_urb_intf0, GFP_ATOMIC);

	} else {
		usb_fill_int_urb(context->rx_urb_intf1, context->usbdev_intf1,
			usb_rcvintpipe(context->usbdev_intf1,
				context->rx_endpoint_intf1->bEndpointAddress),
			context->usb_rx_buf, sizeof(context->usb_rx_buf),
			usb_rx_callback_intf1, context,
			context->rx_endpoint_intf1->bInterval);

		rc = usb_submit_urb(context->rx_urb_intf1, GFP_ATOMIC);
	}

	return rc;
}

static int __init imon_init(void)
{
	int rc;

	printk(KERN_INFO MOD_NAME ": " MOD_DESC ", v" MOD_VERSION "\n");

	rc = usb_register(&imon_driver);
	if (rc) {
		err("%s: usb register failed(%d)", __func__, rc);
		return -ENODEV;
	}

	return 0;
}

static void __exit imon_exit(void)
{
	usb_deregister(&imon_driver);
	printk(KERN_INFO MOD_NAME ": module removed. Goodbye!\n");
}

module_init(imon_init);
module_exit(imon_exit);

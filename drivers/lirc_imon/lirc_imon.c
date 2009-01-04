/*
 *   lirc_imon.c:  LIRC driver/VFD driver for Ahanix/Soundgraph IMON IR/VFD
 *
 *   $Id: lirc_imon.c,v 1.35 2009/01/04 12:12:54 lirc Exp $
 *
 *   Version 0.3
 *		Supports newer iMON models that send decoded IR signals.
 *			This includes the iMON PAD model.
 *		Removed module option for vfd_proto_6p. This driver supports
 *			multiple iMON devices so it is meaningless to have
 *			a global option to set protocol variants.
 *
 *   Version 0.2 beta 2 [January 31, 2005]
 *		USB disconnect/reconnect no longer causes problems for lircd
 *
 *   Version 0.2 beta 1 [January 29, 2005]
 *		Added support for original iMON receiver(ext USB)
 *
 *   Version 0.2 alpha 2 [January 24, 2005]
 *		Added support for VFDs with 6-packet protocol
 *
 *   Version 0.2 alpha 1 [January 23, 2005]
 *		Added support for 2.6 kernels
 *		Reworked disconnect handling
 *		Incorporated Changwoo Ryu's algorithm
 *
 *   Version 0.1 alpha 1[July 5, 2004]
 *
 *   Copyright(C) 2004  Venky Raju(dev@venky.ws)
 *
 *   lirc_imon is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
#include <linux/version.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 22)
#error "*** Sorry, this driver requires kernel version 2.4.22 or higher"
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

#include "drivers/kcompat.h"
#include "drivers/lirc.h"
#include "drivers/lirc_dev/lirc_dev.h"


#define MOD_AUTHOR	"Venky Raju <dev@venky.ws>"
#define MOD_DESC	"Driver for Soundgraph iMON MultiMedia IR/VFD"
#define MOD_NAME	"lirc_imon"
#define MOD_VERSION	"0.5"

#define DISPLAY_MINOR_BASE	144	/* Same as LCD */
#define DEVFS_MODE	(S_IFCHR | S_IRUSR | S_IWUSR | \
			 S_IRGRP | S_IWGRP | S_IROTH)
#define DEVFS_NAME	LIRC_DEVFS_PREFIX "lcd%d"

#define BUF_CHUNK_SIZE	4
#define BUF_SIZE	128

#define BIT_DURATION	250	/* each bit received is 250us */

#define SUCCESS		0
#define	TRUE		1
#define FALSE		0


/* ------------------------------------------------------------
 *		     P R O T O T Y P E S
 * ------------------------------------------------------------
 */

/* USB Callback prototypes */
#ifdef KERNEL_2_5
static int imon_probe(struct usb_interface *interface,
		      const struct usb_device_id *id);
static void imon_disconnect(struct usb_interface *interface);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static void usb_rx_callback(struct urb *urb, struct pt_regs *regs);
static void usb_tx_callback(struct urb *urb, struct pt_regs *regs);
#else
static void usb_rx_callback(struct urb *urb);
static void usb_tx_callback(struct urb *urb);
#endif
#else
static void *imon_probe(struct usb_device *dev, unsigned int intf,
				const struct usb_device_id *id);
static void imon_disconnect(struct usb_device *dev, void *data);
static void usb_rx_callback(struct urb *urb);
static void usb_tx_callback(struct urb *urb);
#endif

static int imon_resume(struct usb_interface *intf);
static int imon_suspend(struct usb_interface *intf, pm_message_t message);

/* Display file_operations function prototypes */
static int display_open(struct inode *inode, struct file *file);
static int display_close(struct inode *inode, struct file *file);
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

/* ------------------------------------------------------------
 *		     G L O B A L S
 * ------------------------------------------------------------
 */

struct imon_context {
	struct usb_device *dev;
	int display_supported;		/* not all controllers do */
	int display_isopen;		/* VFD port has been opened */
#if !defined(KERNEL_2_5)
	int subminor;			/* index into minor_table */
	devfs_handle_t devfs;
#endif
	int ir_isopen;			/* IR port open	*/
	int ir_isassociating;		/* IR port open for association */
	int dev_present;		/* USB device presence */
	struct semaphore sem;		/* to lock this object */
	wait_queue_head_t remove_ok;	/* For unexpected USB disconnects */

	int display_proto_6p;		/* VFD requires 6th packet */
	int ir_onboard_decode;		/* IR signals decoded onboard */

	struct lirc_driver *driver;
	struct usb_endpoint_descriptor *rx_endpoint;
	struct usb_endpoint_descriptor *tx_endpoint;
	struct urb *rx_urb;
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
};

#define LOCK_CONTEXT	down(&context->sem)
#define UNLOCK_CONTEXT	up(&context->sem)

/* VFD file operations */
static struct file_operations display_fops = {
	.owner		= THIS_MODULE,
	.open		= &display_open,
	.write		= &vfd_write,
	.release	= &display_close
};

enum {
	IMON_DISPLAY_TYPE_AUTO,
	IMON_DISPLAY_TYPE_VFD,
	IMON_DISPLAY_TYPE_LCD,
	IMON_DISPLAY_TYPE_NONE,
};

/* USB Device ID for IMON USB Control Board */
static struct usb_device_id imon_usb_id_table[] = {
	/* IMON USB Control Board (IR & VFD) */
	{ USB_DEVICE(0x0aa8, 0xffda) },
	/* IMON USB Control Board (IR only) */
	{ USB_DEVICE(0x0aa8, 0x8001) },
	/* IMON USB Control Board (ext IR only) */
	{ USB_DEVICE(0x04e8, 0xff30) },
	/* IMON USB Control Board (IR & VFD) */
	{ USB_DEVICE(0x15c2, 0xffda) },
	/* IMON USB Control Board (IR & LCD) *and* iMon Knob (IR only) */
	{ USB_DEVICE(0x15c2, 0xffdc) },
	/* IMON USB Control Board (IR & LCD) */
	{ USB_DEVICE(0x15c2, 0x0034) },
	/* IMON USB Control Board (IR & VFD) */
	{ USB_DEVICE(0x15c2, 0x0036) },
	/* IMON USB Control Board (IR & LCD) */
	{ USB_DEVICE(0x15c2, 0x0038) },
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
	{}
};

/* Some iMON models requires a 6th packet */
static struct usb_device_id display_proto_6p_list[] = {
	{ USB_DEVICE(0x15c2, 0xffda) },
	{ USB_DEVICE(0x15c2, 0xffdc) },
	{ USB_DEVICE(0x15c2, 0x0034) },
	{ USB_DEVICE(0x15c2, 0x0036) },
	{ USB_DEVICE(0x15c2, 0x0038) },
	{ USB_DEVICE(0x15c2, 0x0041) },
	{ USB_DEVICE(0x15c2, 0x0042) },
	{ USB_DEVICE(0x15c2, 0x0043) },
	{ USB_DEVICE(0x15c2, 0x0044) },
	{ USB_DEVICE(0x15c2, 0x0045) },
	{}
};
static unsigned char display_packet6[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF };

/* newer iMON models use control endpoints */
static struct usb_device_id ctl_ep_device_list[] = {
	{ USB_DEVICE(0x15c2, 0x0034) },
	{ USB_DEVICE(0x15c2, 0x0036) },
	{ USB_DEVICE(0x15c2, 0x0038) },
	{ USB_DEVICE(0x15c2, 0x0041) },
	{ USB_DEVICE(0x15c2, 0x0042) },
	{ USB_DEVICE(0x15c2, 0x0043) },
	{ USB_DEVICE(0x15c2, 0x0044) },
	{ USB_DEVICE(0x15c2, 0x0045) },
	{}
};

/* iMon LCD modules use a different write op */
static struct usb_device_id lcd_device_list[] = {
	{ USB_DEVICE(0x15c2, 0xffdc) },
	{ USB_DEVICE(0x15c2, 0x0034) },
	{ USB_DEVICE(0x15c2, 0x0038) },
	{ USB_DEVICE(0x15c2, 0x0045) },
	{}
};

/* iMon devices with front panel buttons need a larger buffer */
static struct usb_device_id large_buffer_list[] = {
	{ USB_DEVICE(0x15c2, 0x0038) },
	{ USB_DEVICE(0x15c2, 0x0045) },
};

/* Newer iMON models decode the signal onboard */
static struct usb_device_id ir_onboard_decode_list[] = {
	{ USB_DEVICE(0x15c2, 0xffdc) },
	{ USB_DEVICE(0x15c2, 0x0034) },
	{ USB_DEVICE(0x15c2, 0x0036) },
	{ USB_DEVICE(0x15c2, 0x0038) },
	{ USB_DEVICE(0x15c2, 0x0041) },
	{ USB_DEVICE(0x15c2, 0x0042) },
	{ USB_DEVICE(0x15c2, 0x0043) },
	{ USB_DEVICE(0x15c2, 0x0044) },
	{ USB_DEVICE(0x15c2, 0x0045) },
	{}
};

/* Some iMon devices have no lcd/vfd */
static struct usb_device_id ir_only_list[] = {
	{ USB_DEVICE(0x0aa8, 0x8001) },
	{ USB_DEVICE(0x04e8, 0xff30) },
	/* the first imon lcd and the knob share this device id. :\ */
	/*{ USB_DEVICE(0x15c2, 0xffdc) },*/
	{ USB_DEVICE(0x15c2, 0x0041) },
	{ USB_DEVICE(0x15c2, 0x0042) },
	{ USB_DEVICE(0x15c2, 0x0043) },
	{}
};

/* USB Device data */
static struct usb_driver imon_driver = {
	LIRC_THIS_MODULE(.owner = THIS_MODULE)
	.name		= MOD_NAME,
	.probe		= imon_probe,
	.disconnect	= imon_disconnect,
	.suspend        = imon_suspend,
	.resume         = imon_resume,
	.id_table	= imon_usb_id_table,
#if !defined(KERNEL_2_5)
	.fops		= &display_fops,
	.minor		= DISPLAY_MINOR_BASE,
#endif
};

#ifdef KERNEL_2_5
static struct usb_class_driver imon_class = {
	.name		= DEVFS_NAME,
	.fops		= &display_fops,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 15)
	.mode		= DEVFS_MODE,
#endif
	.minor_base	= DISPLAY_MINOR_BASE,
};
#endif

/* to prevent races between open() and disconnect() */
static DECLARE_MUTEX(disconnect_sem);

static int debug;

/* lcd, vfd or none? should be auto-detected, but can be overridden... */
static int display_type;

#if !defined(KERNEL_2_5)

#define MAX_DEVICES	4	/* In case there's more than one iMON device */
static struct imon_context *minor_table[MAX_DEVICES];

/*
static DECLARE_MUTEX(minor_table_sem);
#define LOCK_MINOR_TABLE	down(&minor_table_sem)
#define UNLOCK_MINOR_TABLE	up(&minor_table_sem)
*/

/* the global usb devfs handle */
extern devfs_handle_t usb_devfs_handle;

#endif

/* ------------------------------------------------------------
 *		     M O D U L E   C O D E
 * ------------------------------------------------------------
 */

MODULE_AUTHOR(MOD_AUTHOR);
MODULE_DESCRIPTION(MOD_DESC);
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(usb, imon_usb_id_table);

module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug messages: 0=no, 1=yes(default: no)");

module_param(display_type, int, 0);
MODULE_PARM_DESC(display_type, "Type of attached display. 0=autodetect, "
		 "1=vfd, 2=lcd, 3=none (default: autodetect)");

static inline void delete_context(struct imon_context *context)
{
	if (context->display_supported)
		usb_free_urb(context->tx_urb);
	usb_free_urb(context->rx_urb);
	lirc_buffer_free(context->driver->rbuf);
	kfree(context->driver->rbuf);
	kfree(context->driver);
	kfree(context);

	if (debug)
		info("%s: context deleted", __func__);
}

static inline void deregister_from_lirc(struct imon_context *context)
{
	int retval;
	int minor = context->driver->minor;

	retval = lirc_unregister_driver(minor);
	if (retval)
		err("%s: unable to deregister from lirc(%d)",
			__func__, retval);
	else
		info("Deregistered iMON driver(minor:%d)", minor);

}

/**
 * Called when the Display device (e.g. /dev/lcd0)
 * is opened by the application.
 */
static int display_open(struct inode *inode, struct file *file)
{
#ifdef KERNEL_2_5
	struct usb_interface *interface;
#endif
	struct imon_context *context = NULL;
	int subminor;
	int retval = SUCCESS;

	/* prevent races with disconnect */
	down(&disconnect_sem);

#ifdef KERNEL_2_5
	subminor = iminor(inode);
	interface = usb_find_interface(&imon_driver, subminor);
	if (!interface) {
		err("%s: could not find interface for minor %d",
		    __func__, subminor);
		retval = -ENODEV;
		goto exit;
	}
	context = usb_get_intfdata(interface);
#else
	subminor = MINOR(inode->i_rdev) - DISPLAY_MINOR_BASE;
	if (subminor < 0 || subminor >= MAX_DEVICES) {
		err("%s: no record of minor %d", __func__, subminor);
		retval = -ENODEV;
		goto exit;
	}
	context = minor_table[subminor];
#endif

	if (!context) {
		err("%s: no context found for minor %d",
					__func__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	LOCK_CONTEXT;

	if (!context->display_supported) {
		err("%s: VFD not supported by device", __func__);
		retval = -ENODEV;
	} else if (context->display_isopen) {
		err("%s: VFD port is already open", __func__);
		retval = -EBUSY;
	} else {
		MOD_INC_USE_COUNT;
		context->display_isopen = TRUE;
		file->private_data = context;
		info("VFD port opened");
	}

	UNLOCK_CONTEXT;

exit:
	up(&disconnect_sem);
	return retval;
}

/**
 * Called when the VFD device(e.g. /dev/usb/lcd)
 * is closed by the application.
 */
static int display_close(struct inode *inode, struct file *file)
{
	struct imon_context *context = NULL;
	int retval = SUCCESS;

	context = (struct imon_context *) file->private_data;

	if (!context) {
		err("%s: no context for device", __func__);
		return -ENODEV;
	}

	LOCK_CONTEXT;

	if (!context->display_supported) {
		err("%s: VFD not supported by device", __func__);
		retval = -ENODEV;
	} else if (!context->display_isopen) {
		err("%s: VFD is not open", __func__);
		retval = -EIO;
	} else {
		context->display_isopen = FALSE;
		MOD_DEC_USE_COUNT;
		info("VFD port closed");
		if (!context->dev_present && !context->ir_isopen) {
			/* Device disconnected before close and IR port is not
			 * open. If IR port is open, context will be deleted by
			 * ir_close. */
			UNLOCK_CONTEXT;
			delete_context(context);
			return retval;
		}
	}

	UNLOCK_CONTEXT;
	return retval;
}

/**
 * Sends a packet to the VFD.
 */
static inline int send_packet(struct imon_context *context)
{
	unsigned int pipe;
	int interval = 0;
	int retval = SUCCESS;
	struct usb_ctrlrequest *control_req = NULL;

	/* Check if we need to use control or interrupt urb */
	if (!context->tx_control) {
		pipe = usb_sndintpipe(context->dev,
				context->tx_endpoint->bEndpointAddress);
#ifdef KERNEL_2_5
		interval = context->tx_endpoint->bInterval;
#endif	/* Use 0 for 2.4 kernels */

		usb_fill_int_urb(context->tx_urb, context->dev, pipe,
			context->usb_tx_buf, sizeof(context->usb_tx_buf),
			usb_tx_callback, context, interval);

		context->tx_urb->actual_length = 0;
	} else {
		/* fill request into kmalloc'ed space: */
		control_req = kmalloc(sizeof(struct usb_ctrlrequest), GFP_NOIO);
		if (control_req == NULL)
			return -ENOMEM;

		/* setup packet is '21 09 0200 0001 0008' */
		control_req->bRequestType = 0x21;
		control_req->bRequest = 0x09;
		control_req->wValue = cpu_to_le16(0x0200);
		control_req->wIndex = cpu_to_le16(0x0001);
		control_req->wLength = cpu_to_le16(0x0008);

		/* control pipe is endpoint 0x00 */
		pipe = usb_sndctrlpipe(context->dev, 0);

		/* build the control urb */
		usb_fill_control_urb(context->tx_urb, context->dev, pipe,
			(unsigned char *)control_req,
			context->usb_tx_buf, sizeof(context->usb_tx_buf),
			usb_tx_callback, context);

		context->tx_urb->actual_length = 0;
	}

	init_completion(&context->tx.finished);
	atomic_set(&(context->tx.busy), 1);

#ifdef KERNEL_2_5
	retval =  usb_submit_urb(context->tx_urb, GFP_KERNEL);
#else
	retval =  usb_submit_urb(context->tx_urb);
#endif
	if (retval != SUCCESS) {
		atomic_set(&(context->tx.busy), 0);
		err("%s: error submitting urb(%d)", __func__, retval);
	} else {
		/* Wait for tranmission to complete(or abort) */
		UNLOCK_CONTEXT;
		wait_for_completion(&context->tx.finished);
		LOCK_CONTEXT;

		retval = context->tx.status;
		if (retval != SUCCESS)
			err("%s: packet tx failed(%d)", __func__, retval);
	}

	kfree(control_req);

	return retval;
}

/**
 * Sends an associate packet to the iMON 2.4G.
 *
 * This might not be such a good idea, since it has an id
 * collition with some versions of the "IR & VFD" combo.
 * The only way to determine if it is a RF version is to look
 * at the product description string.(Which we currently do
 * not fetch).
 */
static inline int send_associate_24g(struct imon_context *context)
{
	int retval;
	const unsigned char packet[8] = { 0x01, 0x00, 0x00, 0x00,
					  0x00, 0x00, 0x00, 0x20 };

	if (!context) {
		err("%s: no context for device", __func__);
		return -ENODEV;
	}

	LOCK_CONTEXT;

	if (!context->dev_present) {
		err("%s: no iMON device present", __func__);
		retval = -ENODEV;
		goto exit;
	}

	memcpy(context->usb_tx_buf, packet, sizeof(packet));
	retval = send_packet(context);

exit:
	UNLOCK_CONTEXT;

	return retval;
}

#ifdef KERNEL_2_5
/**
 * This is the sysfs functions to handle the association og the iMON 2.4G LT.
 *
 *
 */

static ssize_t show_associate_remote(struct device *d,
				     struct device_attribute *attr,
				     char *buf)
{
	struct imon_context *context = dev_get_drvdata(d);

	if (!context)
		return -ENODEV;

	if (context->ir_isassociating) {
		strcpy(buf, "The device it associating press some button "
			    "on the remote.\n");
	} else if (context->ir_isopen) {
		strcpy(buf, "Device is open and ready to associate.\n"
			    "Echo something into this file to start "
			    "the process.\n");
	} else {
		strcpy(buf, "Device is closed, you need to open it to "
			    "associate the remote(you can use irw).\n");
	}
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

	if (!context->ir_isopen)
		return -EINVAL;

	if (context->ir_isopen) {
		context->ir_isassociating = TRUE;
		send_associate_24g(context);
	}

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

#endif



/**
 * Writes data to the VFD.  The IMON VFD is 2x16 characters
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
	int retval = SUCCESS;
	struct imon_context *context;

	context = (struct imon_context *) file->private_data;
	if (!context) {
		err("%s: no context for device", __func__);
		return -ENODEV;
	}

	LOCK_CONTEXT;

	if (!context->dev_present) {
		err("%s: no iMON device present", __func__);
		retval = -ENODEV;
		goto exit;
	}

	if (n_bytes <= 0 || n_bytes > 32) {
		err("%s: invalid payload size", __func__);
		retval = -EINVAL;
		goto exit;
	}

	if (copy_from_user(context->tx.data_buf, buf, n_bytes))
		return -EFAULT;

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
		if (retval != SUCCESS) {
			err("%s: send packet failed for packet #%d",
					__func__, seq/2);
			goto exit;
		} else {
			seq += 2;
			offset += 7;
		}

	} while (offset < 35);

	if (context->display_proto_6p) {
		/* Send packet #6 */
		memcpy(context->usb_tx_buf, display_packet6, 7);
		context->usb_tx_buf[7] = (unsigned char) seq;
		retval = send_packet(context);
		if (retval != SUCCESS)
			err("%s: send packet failed for packet #%d",
					__func__, seq/2);
	}

exit:
	UNLOCK_CONTEXT;

	return(retval == SUCCESS) ? n_bytes : retval;
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
	int retval = SUCCESS;
	struct imon_context *context;

	context = (struct imon_context *) file->private_data;
	if (!context) {
		err("%s: no context for device", __func__);
		return -ENODEV;
	}

	LOCK_CONTEXT;

	if (!context->dev_present) {
		err("%s: no iMON device present", __func__);
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
	if (retval != SUCCESS) {
		err("%s: send packet failed!", __func__);
		goto exit;
	} else if (debug) {
		info("%s: write %d bytes to LCD", __func__, (int) n_bytes);
	}
exit:
	UNLOCK_CONTEXT;
	return (retval == SUCCESS) ? n_bytes : retval;
}

/**
 * Callback function for USB core API: transmit data
 */
#if defined(KERNEL_2_5) && LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static void usb_tx_callback(struct urb *urb, struct pt_regs *regs)
#else
static void usb_tx_callback(struct urb *urb)
#endif
{
	struct imon_context *context;

	if (!urb)
		return;
	context = (struct imon_context *) urb->context;
	if (!context)
		return;

	context->tx.status = urb->status;

	/* notify waiters that write has finished */
	atomic_set(&context->tx.busy, 0);
	complete(&context->tx.finished);

	return;
}

/**
 * Called by lirc_dev when the application opens /dev/lirc
 */
static int ir_open(void *data)
{
	int retval = SUCCESS;
	struct imon_context *context;

	/* prevent races with disconnect */
	down(&disconnect_sem);

	context = (struct imon_context *) data;

	LOCK_CONTEXT;

	if (context->ir_isopen) {
		err("%s: IR port is already open", __func__);
		retval = -EBUSY;
		goto exit;
	}

	/* initial IR protocol decode variables */
	context->rx.count = 0;
	context->rx.initial_space = 1;
	context->rx.prev_bit = 0;

	usb_fill_int_urb(context->rx_urb, context->dev,
		usb_rcvintpipe(context->dev,
				context->rx_endpoint->bEndpointAddress),
		context->usb_rx_buf, sizeof(context->usb_rx_buf),
		usb_rx_callback, context, context->rx_endpoint->bInterval);

#ifdef KERNEL_2_5
	retval = usb_submit_urb(context->rx_urb, GFP_KERNEL);
#else
	retval = usb_submit_urb(context->rx_urb);
#endif

	if (retval)
		err("%s: usb_submit_urb failed for ir_open(%d)",
		    __func__, retval);
	else {
		MOD_INC_USE_COUNT;
		context->ir_isopen = TRUE;
		info("IR port opened");
	}

exit:
	UNLOCK_CONTEXT;

	up(&disconnect_sem);
	return SUCCESS;
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

	LOCK_CONTEXT;

	usb_kill_urb(context->rx_urb);
	context->ir_isopen = FALSE;
	context->ir_isassociating = FALSE;
	MOD_DEC_USE_COUNT;
	info("IR port closed");

	if (!context->dev_present) {
		/* Device disconnected while IR port was
		 * still open. Driver was not deregistered
		 * at disconnect time, so do it now. */
		deregister_from_lirc(context);

		if (!context->display_isopen) {
			UNLOCK_CONTEXT;
			delete_context(context);
			return;
		}
		/*
		 * If display port is open, context will be deleted by
		 * display_close
		 */
	}

	UNLOCK_CONTEXT;
	return;
}

/**
 * Convert bit count to time duration(in us) and submit
 * the value to lirc_dev.
 */
static inline void submit_data(struct imon_context *context)
{
	unsigned char buf[4];
	int value = context->rx.count;
	int i;

	if (debug)
		info("submitting data to LIRC\n");

	value *= BIT_DURATION;
	value &= PULSE_MASK;
	if (context->rx.prev_bit)
		value |= PULSE_BIT;

	for (i = 0; i < 4; ++i)
		buf[i] = value>>(i*8);

	lirc_buffer_write_1(context->driver->rbuf, buf);
	wake_up(&context->driver->rbuf->wait_poll);
	return;
}

/**
 * Process the incoming packet
 */
static inline void incoming_packet(struct imon_context *context,
				   struct urb *urb)
{
	int len = urb->actual_length;
	unsigned char *buf = urb->transfer_buffer;
	int octet, bit;
	unsigned char mask;
	int chunk_num;
#ifdef DEBUG
	int i;
#endif

	/*
	 * we need to add some special handling for
	 * the imon's IR mouse events
	 */
	if ((len == 5) && (buf[0] == 0x01) && (buf[4] == 0x00)) {
		/* first, pad to 8 bytes so it conforms with everything else */
		buf[5] = buf[6] = buf[7] = 0;
		len = 8;

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
		if ((buf[1] == 0) && ((buf[2] != 0) || (buf[3] != 0))) {
			int y = (int)(char)buf[2];
			int x = (int)(char)buf[3];
			if (abs(abs(x) - abs(y)) < 3) {
				return;
			} else if (abs(y) > abs(x)) {
				buf[2] = 0x00;
				buf[3] = (y > 0) ? 0x7f : 0x80;
			} else {
				buf[3] = 0x00;
				buf[2] = (x > 0) ? 0x7f : 0x80;
			}
		}
	}

	if (len != 8) {
		warn("%s: invalid incoming packet size(%d)",
		     __func__, len);
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
		warn("%s: remote associated refid=%02X", __func__, buf[1]);
		context->ir_isassociating = FALSE;
	}

	chunk_num = buf[7];

	if (chunk_num == 0xFF)
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

#ifdef DEBUG
	for (i = 0; i < 8; ++i)
		printk(KERN_INFO "%02x ", buf[i]);
	printk(KERN_INFO "\n");
#endif

	if (context->ir_onboard_decode) {
		/* The signals have been decoded onboard the iMON controller */
		lirc_buffer_write_1(context->driver->rbuf, buf);
		wake_up(&context->driver->rbuf->wait_poll);
		return;
	}

	/* Translate received data to pulse and space lengths.
	 * Received data is active low, i.e. pulses are 0 and
	 * spaces are 1.
	 *
	 * My original algorithm was essentially similar to
	 * Changwoo Ryu's with the exception that he switched
	 * the incoming bits to active high and also fed an
	 * initial space to LIRC at the start of a new sequence
	 * if the previous bit was a pulse.
	 *
	 * I've decided to adopt his algorithm. */

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
 * Callback function for USB core API: receive data
 */
#if defined(KERNEL_2_5) && LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static void usb_rx_callback(struct urb *urb, struct pt_regs *regs)
#else
static void usb_rx_callback(struct urb *urb)
#endif
{
	struct imon_context *context;

	if (!urb)
		return;
	context = (struct imon_context *) urb->context;
	if (!context)
		return;

	switch (urb->status) {
	case -ENOENT:		/* usbcore unlink successful! */
		return;
	case SUCCESS:
		if (context->ir_isopen)
			incoming_packet(context, urb);
		break;
	default	:
		warn("%s: status(%d): ignored", __func__, urb->status);
		break;
	}

#ifdef KERNEL_2_5
	usb_submit_urb(context->rx_urb, GFP_ATOMIC);
#endif
	return;
}



/**
 * Callback function for USB core API: Probe
 */
#ifdef KERNEL_2_5
static int imon_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
#else
static void *imon_probe(struct usb_device *dev, unsigned int intf,
			const struct usb_device_id *id)
#endif
{
#ifdef KERNEL_2_5
	struct usb_device *dev = NULL;
	struct usb_host_interface *iface_desc = NULL;
#else
	struct usb_interface *interface = NULL;
	struct usb_interface_descriptor *iface_desc = NULL;
	char name[10];
	int subminor = 0;
#endif
	struct usb_endpoint_descriptor *rx_endpoint = NULL;
	struct usb_endpoint_descriptor *tx_endpoint = NULL;
	struct urb *rx_urb = NULL;
	struct urb *tx_urb = NULL;
	struct lirc_driver *driver = NULL;
	struct lirc_buffer *rbuf = NULL;
	int lirc_minor = 0;
	int num_endpoints;
	int retval = SUCCESS;
	int display_ep_found;
	int ir_ep_found;
	int alloc_status;
	int display_proto_6p = FALSE;
	int ir_onboard_decode = FALSE;
	int buf_chunk_size = BUF_CHUNK_SIZE;
	int code_length;
	int tx_control = FALSE;
	struct imon_context *context = NULL;
	int i;

	info("%s: found IMON device", __func__);

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

#if !defined(KERNEL_2_5)
	for (subminor = 0; subminor < MAX_DEVICES; ++subminor) {
		if (minor_table[subminor] == NULL)
			break;
	}
	if (subminor == MAX_DEVICES) {
		err("%s: allowed max number of devices already present",
		    __func__);
		retval = -ENOMEM;
		goto exit;
	}
#endif

#ifdef KERNEL_2_5
	dev = usb_get_dev(interface_to_usbdev(interface));
	iface_desc = interface->cur_altsetting;
	num_endpoints = iface_desc->desc.bNumEndpoints;
#else
	interface = &dev->actconfig->interface[intf];
	iface_desc = &interface->altsetting[interface->act_altsetting];
	num_endpoints = iface_desc->bNumEndpoints;
#endif

	/*
	 * Scan the endpoint list and set:
	 *	first input endpoint = IR endpoint
	 *	first output endpoint = VFD endpoint
	 */

	ir_ep_found = FALSE;
	display_ep_found = FALSE;

	for (i = 0; i < num_endpoints && !(ir_ep_found && display_ep_found);
	     ++i) {
		struct usb_endpoint_descriptor *ep;
		int ep_dir;
		int ep_type;
#ifdef KERNEL_2_5
		ep = &iface_desc->endpoint[i].desc;
#else
		ep = &iface_desc->endpoint[i];
#endif
		ep_dir = ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK;
		ep_type = ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;

		if (!ir_ep_found &&
			ep_dir == USB_DIR_IN &&
			ep_type == USB_ENDPOINT_XFER_INT) {

			rx_endpoint = ep;
			ir_ep_found = TRUE;
			if (debug)
				info("%s: found IR endpoint", __func__);

		} else if (!display_ep_found &&
			   ep_dir == USB_DIR_OUT &&
			   ep_type == USB_ENDPOINT_XFER_INT) {
			tx_endpoint = ep;
			display_ep_found = TRUE;
			if (debug)
				info("%s: found VFD endpoint", __func__);
		}
	}

	/*
	 * If we didn't find a display endpoint, this is probably one of the
	 * newer iMon devices that use control urb instead of interrupt
	 */
	if (!display_ep_found) {
		if (usb_match_id(interface, ctl_ep_device_list)) {
			tx_control = 1;
			display_ep_found = TRUE;
			if (debug)
				info("%s: LCD device uses control endpoint, "
				     "not interface OUT endpoint", __func__);
		}
	}

	/*
	 * Some iMon receivers have no display. Unfortunately, it seems
	 * that SoundGraph recycles device IDs between devices both with
	 * and without... :\
	 */
	if ((display_type == IMON_DISPLAY_TYPE_AUTO &&
	     usb_match_id(interface, ir_only_list)) ||
	    display_type == IMON_DISPLAY_TYPE_NONE) {
		tx_control = 0;
		display_ep_found = 0;
		if (debug)
			info("%s: device has no display", __func__);
	}

	/* Input endpoint is mandatory */
	if (!ir_ep_found) {
		err("%s: no valid input(IR) endpoint found.", __func__);
		retval = -ENODEV;
		goto exit;
	} else {
		/* Determine if the IR signals are decoded onboard */
		if (usb_match_id(interface, ir_onboard_decode_list))
			ir_onboard_decode = TRUE;

		if (debug)
			info("ir_onboard_decode: %d", ir_onboard_decode);
	}

	/* Determine if VFD requires 6 packets */
	if (display_ep_found) {
		if (usb_match_id(interface, display_proto_6p_list))
			display_proto_6p = TRUE;

		if (debug)
			info("display_proto_6p: %d", display_proto_6p);
	}

	/* Allocate memory */

	alloc_status = SUCCESS;

	context = kmalloc(sizeof(struct imon_context), GFP_KERNEL);
	if (!context) {
		err("%s: kmalloc failed for context", __func__);
		alloc_status = 1;
		goto alloc_status_switch;
	}
	driver = kmalloc(sizeof(struct lirc_driver), GFP_KERNEL);
	if (!driver) {
		err("%s: kmalloc failed for lirc_driver", __func__);
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
#ifdef KERNEL_2_5
	rx_urb = usb_alloc_urb(0, GFP_KERNEL);
#else
	rx_urb = usb_alloc_urb(0);
#endif
	if (!rx_urb) {
		err("%s: usb_alloc_urb failed for IR urb", __func__);
		alloc_status = 5;
		goto alloc_status_switch;
	}
	if (display_ep_found) {
#ifdef KERNEL_2_5
		tx_urb = usb_alloc_urb(0, GFP_KERNEL);
#else
		tx_urb = usb_alloc_urb(0);
#endif
		if (!tx_urb) {
			err("%s: usb_alloc_urb failed for VFD urb",
			    __func__);
			alloc_status = 6;
			goto alloc_status_switch;
		}
	}

	/* clear all members of imon_context and lirc_driver */
	memset(context, 0, sizeof(struct imon_context));
	init_MUTEX(&context->sem);
	context->display_proto_6p = display_proto_6p;
	context->ir_onboard_decode = ir_onboard_decode;

	memset(driver, 0, sizeof(struct lirc_driver));

	strcpy(driver->name, MOD_NAME);
	driver->minor = -1;
	driver->code_length = ir_onboard_decode ?
		buf_chunk_size * 8 : sizeof(lirc_t) * 8;
	driver->sample_rate = 0;
	driver->features = (ir_onboard_decode) ?
		LIRC_CAN_REC_LIRCCODE : LIRC_CAN_REC_MODE2;
	driver->data = context;
	driver->rbuf = rbuf;
	driver->set_use_inc = ir_open;
	driver->set_use_dec = ir_close;
#ifdef LIRC_HAVE_SYSFS
	driver->dev = &dev->dev;
#endif
	driver->owner = THIS_MODULE;

	LOCK_CONTEXT;

	lirc_minor = lirc_register_driver(driver);
	if (lirc_minor < 0) {
		err("%s: lirc_register_driver failed", __func__);
		alloc_status = 7;
		UNLOCK_CONTEXT;
		goto alloc_status_switch;
	} else
		info("%s: Registered iMON driver(minor:%d)",
		     __func__, lirc_minor);

	/* Needed while unregistering! */
	driver->minor = lirc_minor;

	context->dev = dev;
	context->dev_present = TRUE;
	context->rx_endpoint = rx_endpoint;
	context->rx_urb = rx_urb;
	if (display_ep_found) {
		context->display_supported = TRUE;
		context->tx_endpoint = tx_endpoint;
		context->tx_urb = tx_urb;
		context->tx_control = tx_control;
	}
	context->driver = driver;

#ifdef KERNEL_2_5
	usb_set_intfdata(interface, context);

	if (cpu_to_le16(dev->descriptor.idProduct) == 0xffdc) {
		int err;

		err = sysfs_create_group(&interface->dev.kobj,
					 &imon_attribute_group);
		if (err)
			err("%s: Could not create sysfs entries(%d)",
			    __func__, err);
	}
#else
	minor_table[subminor] = context;
	context->subminor = subminor;
#endif

	if (display_ep_found) {
		if (debug)
			info("Registering VFD with devfs");
#ifdef KERNEL_2_5
		if (usb_register_dev(interface, &imon_class)) {
			/* Not a fatal error, so ignore */
			info("%s: could not get a minor number for VFD",
				__func__);
		}
#else
		sprintf(name, DEVFS_NAME, subminor);
		if (!(context->devfs = devfs_register(usb_devfs_handle, name,
					DEVFS_FL_DEFAULT, USB_MAJOR,
					DISPLAY_MINOR_BASE + subminor,
					DEVFS_MODE, &display_fops, NULL))) {
			/* not a fatal error so ignore */
			info("%s: devfs register failed for VFD",
					__func__);
		}
#endif
	}

	info("%s: iMON device on usb<%d:%d> initialized",
			__func__, dev->bus->busnum, dev->devnum);

	UNLOCK_CONTEXT;

alloc_status_switch:

	switch (alloc_status) {
	case 7:
		if (display_ep_found)
			usb_free_urb(tx_urb);
	case 6:
		usb_free_urb(rx_urb);
	case 5:
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
	case SUCCESS:
		;
	}

 exit:
#ifdef KERNEL_2_5
	return retval;
#else
	return (retval == SUCCESS) ? context : NULL;
#endif
}

/**
 * Callback function for USB core API: disonnect
 */
#ifdef KERNEL_2_5
static void imon_disconnect(struct usb_interface *interface)
#else
static void imon_disconnect(struct usb_device *dev, void *data)
#endif
{
	struct imon_context *context;

	/* prevent races with ir_open()/display_open() */
	down(&disconnect_sem);

#ifdef KERNEL_2_5
	context = usb_get_intfdata(interface);
#else
	context = (struct imon_context *)data;
#endif
	LOCK_CONTEXT;

	info("%s: iMON device disconnected", __func__);

#ifdef KERNEL_2_5
	/* sysfs_remove_group is safe to call even if sysfs_create_group
	 * hasn't been called */
	sysfs_remove_group(&interface->dev.kobj,
			   &imon_attribute_group);
	usb_set_intfdata(interface, NULL);
#else
	minor_table[context->subminor] = NULL;
#endif
	context->dev_present = FALSE;

	/* Stop reception */
	usb_kill_urb(context->rx_urb);

	/* Abort ongoing write */
	if (atomic_read(&context->tx.busy)) {
		usb_kill_urb(context->tx_urb);
		wait_for_completion(&context->tx.finished);
	}

	/* De-register from lirc_dev if IR port is not open */
	if (!context->ir_isopen)
		deregister_from_lirc(context);

	if (context->display_supported)
#ifdef KERNEL_2_5
		usb_deregister_dev(interface, &imon_class);
#else
		if (context->devfs)
			devfs_unregister(context->devfs);
#endif

	UNLOCK_CONTEXT;

	if (!context->ir_isopen && !context->display_isopen)
		delete_context(context);

	up(&disconnect_sem);
}

static int imon_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct imon_context *context = usb_get_intfdata(intf);

	if (context->ir_isopen)
		usb_kill_urb(context->rx_urb);
	return 0;
}

static int imon_resume(struct usb_interface *intf)
{
	int r = 0;
	struct imon_context *context = usb_get_intfdata(intf);

	if (context->ir_isopen)
		usb_submit_urb(context->rx_urb, GFP_ATOMIC);

	return r;
}

static int __init imon_init(void)
{
	int rc;

	info(MOD_DESC ", v" MOD_VERSION);
	info(MOD_AUTHOR);

	rc = usb_register(&imon_driver);
	if (rc) {
		err("%s: usb register failed(%d)", __func__, rc);
		return -ENODEV;
	}
	return SUCCESS;
}

static void __exit imon_exit(void)
{
	usb_deregister(&imon_driver);
	info("module removed. Goodbye!");
}


module_init(imon_init);
module_exit(imon_exit);

#if !defined(KERNEL_2_5)
EXPORT_NO_SYMBOLS;
#endif

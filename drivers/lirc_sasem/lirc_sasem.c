/*      $Id: lirc_sasem.c,v 1.6 2005/03/28 17:05:40 lirc Exp $      */

/* lirc_sasem.c - USB remote support for LIRC
 * Version 0.3  [beta status]
 *
 * Copyright (C) 2004 Oliver Stabel <oliver.stabel@gmx.de>
 *
 * This driver was derived from:
 *   Paul Miller <pmiller9@users.sourceforge.net>'s 2003-2004
 *      "lirc_atiusb - USB remote support for LIRC"
 *   Culver Consulting Services <henry@culcon.com>'s 2003
 *      "Sasem OnAir VFD/IR USB driver"
 *
 *
 * 2004/06/13   -   0.1
 *                  initial version
 *
 * 2004/06/28   -   0.2
 *                  added file system support to write data to VFD device (used  
 *                  in conjunction with LCDProc)
 *
 * 2004/11/22   -   0.3
 *                  Ported to 2.6 kernel - Tim Davies <tim@opensystems.net.au>
 *
 * TODO
 *	- keypresses seem to be rather sluggish sometimes; check
 *	  intervall and timing
 *	- check USB Minor allocation
 *	- param to enable/disable LIRC communication (??)
 *
 * About naming
 *
 *  variables use the following naming rule: [scope]_[type]Name
 *  functions use the following naming rule: [scope]_[returntype]Name
 *
 *  scope:
 *      c : const
 *      g : global
 *      l : local
 *      m : member
 *      p : parameter
 *      s : static
 *
 *  type:
 *      a : array
 *      c : char
 *      d : double
 *      f : float
 *      i : integer
 *      k : short
 *      l : long
 *      p : pointer
 *      s : struct
 *      u : unsigned
 *      v : signed
 *
 *  example:    p_iCount = Parameter, Integer
 *              s_acBuf  = Static, Array of chars
 *
 */

/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <linux/poll.h>
#include <linux/version.h>
#include <linux/devfs_fs_kernel.h>

#include "lirc_sasem.h"
#include "drivers/lirc.h"
#include "drivers/lirc_dev/lirc_dev.h"

//#define dbg(format, arg...) printk(KERN_DEBUG "%s: " format "\n" , __FILE__ , ## arg)

MODULE_AUTHOR( DRIVER_AUTHOR );
MODULE_DESCRIPTION( DRIVER_DESC );
MODULE_LICENSE("GPL");

static int debug = 0;

MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "enable debug = 1, disable = 0 (default)");

static t_usb_device_id s_sSasemID [] = {
	{ USB_DEVICE(0x11ba, 0x0101) },
	{ }
};

MODULE_DEVICE_TABLE (usb, s_sSasemID);

static t_file_operations s_sSasemFileOps =
{
	owner:      THIS_MODULE,
	read:       s_sSasemFSRead,
	write:      s_sSasemFSWrite,
	ioctl:      s_iSasemFSIoctl,
	open:       s_iSasemFSOpen,
	release:    s_iSasemFSRelease,
	poll:       s_uSasemFSPoll,
};

#ifdef KERNEL_2_5
static t_usb_class_driver s_sSasemClass =
{
	name:		"usb/lcd",
	fops:		&s_sSasemFileOps,
	mode:		S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH,
	minor_base:	SASEM_MINOR,
};
#endif

static t_usb_driver s_sSasemDriver =
{
	owner:			THIS_MODULE,
	name:			"Sasem",
	probe:          s_SasemProbe,
	disconnect:     s_SasemDisconnect,
#ifndef KERNEL_2_5
	fops:           &s_sSasemFileOps,
	minor:			SASEM_MINOR,
#endif
	id_table:       s_sSasemID,
};

static t_sasemDevice *s_psSasemDevice = NULL;

static int __init s_iSasemInit (void)
{
	printk(BANNER);

	if (usb_register(&s_sSasemDriver)) {
		err("USB registration failed");
		return -ENOSYS;
	}

	return 0;
}

static void __exit s_SasemExit (void)
{
	usb_deregister (&s_sSasemDriver);
}

module_init (s_iSasemInit);
module_exit (s_SasemExit);

#ifndef KERNEL_2_5
static void * s_SasemProbe(struct usb_device *p_psDevice,
			   unsigned p_uInterfaceNum,
			   const struct usb_device_id *p_psID)
{
	struct usb_interface_descriptor *l_psCurrentInterfaceDescriptor;
#else
static int s_SasemProbe(t_usb_interface *p_psInt, const t_usb_device_id *p_psID)
{
	struct usb_device *p_psDevice = NULL;
	t_usb_host_interface *iface_host = NULL;
#endif
	t_sasemDevice *l_psSasemDevice = NULL;
	struct usb_endpoint_descriptor *l_psEndpoint, *l_psEndpoint2;
	int l_iPipe;
	int l_iDevnum;
	t_lirc_plugin *l_psLircPlugin = NULL;
	t_lirc_buffer *l_psLircBuffer = NULL;
	int l_iLircMinor = -1;
	int l_iMemFailure;
	char l_acBuf[63], l_acName[128]="";

	dbg(" {\n");
	
#ifndef KERNEL_2_5
	l_psCurrentInterfaceDescriptor = p_psDevice->actconfig->interface->
		altsetting;
	l_psEndpoint = l_psCurrentInterfaceDescriptor->endpoint;
	l_psEndpoint2 = l_psEndpoint + 1;
#else
	p_psDevice = interface_to_usbdev(p_psInt);
	iface_host = p_psInt->cur_altsetting;
	l_psEndpoint = &(iface_host->endpoint[0].desc);
	l_psEndpoint2 = &(iface_host->endpoint[1].desc);
#endif
	
	if (!(l_psEndpoint->bEndpointAddress & 0x80) ||
		((l_psEndpoint->bmAttributes & 3) != 0x03)) {
		err("OnAir config endpoint error");
#ifndef KERNEL_2_5
		return NULL;
#else
		return -ENODEV;
#endif
	}

	l_iDevnum = p_psDevice->devnum;

	l_iMemFailure = 0;
	if (!(l_psSasemDevice = kmalloc(sizeof(t_sasemDevice), GFP_KERNEL))) {
		err("kmalloc(sizeof(t_sasemDevice), GFP_KERNEL)) failed");
		l_iMemFailure = 1;
	}
	else {
		memset(l_psSasemDevice, 0, sizeof(t_sasemDevice));
		if (!(l_psLircPlugin = 
			kmalloc(sizeof(t_lirc_plugin), GFP_KERNEL))) {
			err("kmalloc(sizeof(t_lirc_plugin), GFP_KERNEL))"
				"failed");
			l_iMemFailure = 2;
		}
		else if (!(l_psLircBuffer = 
			kmalloc(sizeof(t_lirc_buffer), GFP_KERNEL))) {
			err("kmalloc(sizeof(t_lirc_buffer), GFP_KERNEL))"  
				" failed");
			l_iMemFailure = 3;
		}
		else if (lirc_buffer_init(l_psLircBuffer, MAX_INTERRUPT_DATA,
				4)) {
			err("lirc_buffer_init failed");
			l_iMemFailure = 4;
		}
#ifndef KERNEL_2_5
		else if (!(l_psSasemDevice->m_psUrbIn = usb_alloc_urb(0))) {
#else
		else if (!(l_psSasemDevice->m_psUrbIn = usb_alloc_urb(0, GFP_KERNEL))) {
#endif
			err("usb_alloc_urb(0) failed");
			l_iMemFailure = 5;
		} else {

			memset(l_psLircPlugin, 0, sizeof(t_lirc_plugin));
			strcpy(l_psLircPlugin->name, DRIVER_NAME " ");
			l_psLircPlugin->minor = -1;
			l_psLircPlugin->code_length = MAX_INTERRUPT_DATA*8;
			l_psLircPlugin->features = LIRC_CAN_REC_LIRCCODE;
			l_psLircPlugin->data = l_psSasemDevice;
			
			l_psLircPlugin->rbuf = l_psLircBuffer;
			l_psLircPlugin->set_use_inc = &s_iLircSetUseInc;
			l_psLircPlugin->set_use_dec = &s_iLircSetUseDec;
			l_psLircPlugin->owner = THIS_MODULE;

			if ((l_iLircMinor = 
				lirc_register_plugin(l_psLircPlugin)) < 0) {
				err("lirc_register_plugin(l_psLircPlugin))"  
					" failed");
				l_iMemFailure = 9;
			}
		}
	}
	switch (l_iMemFailure) {
	case 9:
		usb_free_urb(l_psSasemDevice->m_psUrbIn);
	case 5:
	case 4:
		kfree(l_psLircBuffer);
	case 3:
		kfree(l_psLircPlugin);
	case 2:
		kfree(l_psSasemDevice);
	case 1:
#ifndef KERNEL_2_5
		return NULL;
#else
		return -ENOMEM;
#endif
	}
	
	l_psLircPlugin->minor = l_iLircMinor; 
	
	dbg(": init device structure\n");
	init_MUTEX(&l_psSasemDevice->m_sSemLock);
	down_interruptible(&l_psSasemDevice->m_sSemLock);
	l_psSasemDevice->m_psDescriptorIn = l_psEndpoint;
	l_psSasemDevice->m_psDescriptorOut = l_psEndpoint2;    
	l_psSasemDevice->m_psDevice = p_psDevice;
	l_psSasemDevice->m_psLircPlugin = l_psLircPlugin;
	l_psSasemDevice->m_iOpen = 0;
	l_psSasemDevice->m_psUrbOut = NULL;
	l_psSasemDevice->m_sPressTime.tv_sec = 0; 
	l_psSasemDevice->m_sPressTime.tv_usec = 0;
	init_waitqueue_head(&l_psSasemDevice->m_sQueueOpen);
	init_waitqueue_head(&l_psSasemDevice->m_sQueueWrite);

	dbg(": init inbound URB\n");
	l_iPipe = usb_rcvintpipe(l_psSasemDevice->m_psDevice,
		l_psSasemDevice->m_psDescriptorIn->bEndpointAddress);
	
	usb_fill_int_urb(l_psSasemDevice->m_psUrbIn, 
			 l_psSasemDevice->m_psDevice,
			 l_iPipe, l_psSasemDevice->m_aucBufferIn,
			 sizeof(l_psSasemDevice->m_aucBufferIn),
			 s_SasemCallbackIn, l_psSasemDevice, 
			 l_psSasemDevice->m_psDescriptorIn->bInterval);

	dbg(": get USB device info\n");
	if (p_psDevice->descriptor.iManufacturer &&
			usb_string(p_psDevice, 
			p_psDevice->descriptor.iManufacturer, 
			l_acBuf, 63) > 0) {
		strncpy(l_acName, l_acBuf, 128);
	}
	if (p_psDevice->descriptor.iProduct &&
			usb_string(p_psDevice, p_psDevice->descriptor.iProduct, 
			l_acBuf, 63) > 0) {
		snprintf(l_acName, 128, "%s %s", l_acName, l_acBuf);
	}
	printk(DRIVER_NAME "[%d]: %s on usb%d\n", l_iDevnum, l_acName,
		p_psDevice->bus->busnum);

	s_psSasemDevice = l_psSasemDevice;
	up(&l_psSasemDevice->m_sSemLock);
	dbg(" }\n");
#ifndef KERNEL_2_5
	return l_psSasemDevice;
#else
	usb_set_intfdata(p_psInt, l_psSasemDevice);
	if (usb_register_dev(p_psInt, &s_sSasemClass)) {
		dbg(": Can't get minor for this device\n");
		usb_set_intfdata(p_psInt, NULL);
		return -ENODEV;
	}
	return 0;
#endif
}


#ifndef KERNEL_2_5
static void s_SasemDisconnect(t_usb_device *p_psDevice, void *p_pPtr) {
	t_sasemDevice *l_psSasemDevice = p_pPtr;
#else
static void s_SasemDisconnect(t_usb_interface *p_psInt) {
	t_sasemDevice *l_psSasemDevice = usb_get_intfdata(p_psInt);
	usb_set_intfdata(p_psInt, NULL);
#endif

	dbg(" {\n");

	down_interruptible(&l_psSasemDevice->m_sSemLock);

#ifdef KERNEL_2_5
	usb_deregister_dev(p_psInt, &s_sSasemClass);
#endif

	dbg(": free inbound URB\n");    
	usb_unlink_urb(l_psSasemDevice->m_psUrbIn);
	usb_free_urb(l_psSasemDevice->m_psUrbIn);
	s_iUnregisterFromLirc(l_psSasemDevice);

	if (l_psSasemDevice->m_psUrbOut != NULL) {
		dbg(": free outbound URB\n");    
		usb_unlink_urb(l_psSasemDevice->m_psUrbOut);
		usb_free_urb(l_psSasemDevice->m_psUrbOut);
}
	up(&l_psSasemDevice->m_sSemLock);
	kfree (l_psSasemDevice);
	dbg(" }\n");
}

#ifndef KERNEL_2_5
static void s_SasemCallbackIn(t_urb *p_psUrb) {
#else
static void s_SasemCallbackIn(t_urb *p_psUrb, struct pt_regs *regs) {
#endif
	t_sasemDevice *l_psSasemDevice;
	int l_iDevnum;
	int l_iLen;
	char l_acBuf[MAX_INTERRUPT_DATA];
	int l_i;
	struct timeval l_stv;
	long l_lms;

	dbg(" {\n");
	
	if (!p_psUrb) {
		dbg(": p_psUrb == NULL\n");
		return;
	}

	if (!(l_psSasemDevice = p_psUrb->context)) {
		dbg(": no context\n");
#ifdef KERNEL_2_5
		p_psUrb->transfer_flags |= URB_ASYNC_UNLINK;
#endif
		usb_unlink_urb(p_psUrb);
		return;
	}

	l_iDevnum = l_psSasemDevice->m_iDevnum;
	if (debug) {
		printk(DRIVER_NAME "[%d]: data received (length %d)\n",
		       l_iDevnum, p_psUrb->actual_length);
		printk(DRIVER_NAME 
		       " intr_callback called %x %x %x %x %x %x %x %x\n", 
		       l_psSasemDevice->m_aucBufferIn[0],
		       l_psSasemDevice->m_aucBufferIn[1],
		       l_psSasemDevice->m_aucBufferIn[2],
		       l_psSasemDevice->m_aucBufferIn[3],
		       l_psSasemDevice->m_aucBufferIn[4],
		       l_psSasemDevice->m_aucBufferIn[5],
		       l_psSasemDevice->m_aucBufferIn[6],
		       l_psSasemDevice->m_aucBufferIn[7]);
	}

	switch (p_psUrb->status) {

	/* success */
	case 0:
		l_iLen = p_psUrb->actual_length;
		if (l_iLen > MAX_INTERRUPT_DATA) return;

		memcpy(l_acBuf,p_psUrb->transfer_buffer,l_iLen);

		// is this needed? The OnAir device should always
		// return 8 bytes
		for (l_i = l_iLen; l_i < MAX_INTERRUPT_DATA; l_i++) 
			l_acBuf[l_i] = 0;

		// the OnAir device seems not to be able to signal a
		// pressed button by repeating its code. Keeping a
		// button pressed first sends the real code (e.g. 0C
		// 80 7F 41 BE 00 00 00) and then keeps sending 08 00
		// 00 00 00 00 00 00 as long as the button is pressed
		// (notice that in the real key code 80 = !7F and 41 =
		// !BE is this important? maybe for validation?) maybe
		// 08 00 00 00 00 00 00 00 is the number of presses?
		// who knows ...
		// so lets do the following: if a code != the 08 code
		// arrives, store it to repeat it if necessary for
		// LIRC. If an 08 code follows afterwards, send the
		// old code again to the buffer do this as long as the
		// 08 code is being sent
		// example:
		// Code from Remote          Lirc Buffer
		//  0C 80 7F 41 BE 00 00 00   0C 80 7F 41 BE 00 00 00
		//  08 00 00 00 00 00 00 00   0C 80 7F 41 BE 00 00 00
		//  08 00 00 00 00 00 00 00   0C 80 7F 41 BE 00 00 00
		//  08 00 00 00 00 00 00 00   0C 80 7F 41 BE 00 00 00
		//  08 00 00 00 00 00 00 00   0C 80 7F 41 BE 00 00 00
		//  0C 80 7F 40 BF 00 00 00   0C 80 7F 40 BF 00 00 00
		//  08 00 00 00 00 00 00 00   0C 80 7F 40 BF 00 00 00
		//  08 00 00 00 00 00 00 00   0C 80 7F 40 BF 00 00 00
		//  0C 80 7F 41 BE 00 00 00   0C 80 7F 41 BE 00 00 00
		
		// get the time since the last button press
		do_gettimeofday(&l_stv);
		l_lms = (l_stv.tv_sec - l_psSasemDevice->m_sPressTime.tv_sec) * 1000 + (l_stv.tv_usec - l_psSasemDevice->m_sPressTime.tv_usec) / 1000;

		if (memcmp(l_acBuf, sc_cSasemCode, MAX_INTERRUPT_DATA) == 0) {
			// the repeat code is being sent, so we copy
			// the old code to LIRC
			
			// NOTE: Only if the last code was less than 250ms ago
			// - no one should be able to push another (undetected) button
			//   in that time and then get a false repeat of the previous press
			// - but it is long enough for a genuine repeat
			if ((l_lms < 250) && (l_psSasemDevice->m_iCodeSaved != 0)) {
				memcpy(l_acBuf, &l_psSasemDevice->m_acLastCode,
				       MAX_INTERRUPT_DATA);
				l_psSasemDevice->m_sPressTime.tv_sec = l_stv.tv_sec; 
				l_psSasemDevice->m_sPressTime.tv_usec = l_stv.tv_usec;
			}
			// there was no old code
			else {
				// Do Nothing!
			}
		}
		else {
			// save the current valid code for repeats
			memcpy(&l_psSasemDevice->m_acLastCode, l_acBuf,
			       MAX_INTERRUPT_DATA);
			// set flag to signal a valid code was save;
			// just for safety reasons
			l_psSasemDevice->m_iCodeSaved = 1;
			l_psSasemDevice->m_sPressTime.tv_sec = l_stv.tv_sec; 
			l_psSasemDevice->m_sPressTime.tv_usec = l_stv.tv_usec;
		}
		
		/* copy 1 code to lirc_buffer */
		lirc_buffer_write_1(l_psSasemDevice->m_psLircPlugin->rbuf,
			l_acBuf);
		wake_up(&l_psSasemDevice->m_psLircPlugin->rbuf->wait_poll);
		break;

	/* unlink */
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		err(": urb failed, status %d\n", p_psUrb->status);
#ifdef KERNEL_2_5
		p_psUrb->transfer_flags |= URB_ASYNC_UNLINK;
#endif
		usb_unlink_urb(p_psUrb);
		return;
	}

#ifdef KERNEL_2_5
	/* resubmit urb */
	usb_submit_urb(p_psUrb, SLAB_ATOMIC);
#endif
	dbg(" }\n");
}

/* lirc stuff */

static int s_iUnregisterFromLirc(t_sasemDevice *p_psSasemDevice) {
	t_lirc_plugin *l_psLircPlugin = p_psSasemDevice->m_psLircPlugin;
	int l_iDevnum;

	dbg(" {\n");
	l_iDevnum = p_psSasemDevice->m_iDevnum;
	
	lirc_unregister_plugin(l_psLircPlugin->minor);
	
	printk(DRIVER_NAME "[%d]: usb remote disconnected\n", l_iDevnum);
	
	lirc_buffer_free(l_psLircPlugin->rbuf);
	kfree(l_psLircPlugin->rbuf);
	kfree(l_psLircPlugin);
	dbg(" }\n");
	return 0;
}

static int s_iLircSetUseInc(void *p_pData) {
	t_sasemDevice *l_psSasemDevice = p_pData;
	int l_iDevnum;

	dbg(" {\n");
	if (!l_psSasemDevice) {
		err(" no context\n");
		return -EIO;
	}
	
	l_iDevnum = l_psSasemDevice->m_iDevnum;

	if (!l_psSasemDevice->m_iConnected) {
		
		/*
			this is the trigger from LIRC to start
			transfering data so the URB is being submitted
		*/

		if (!l_psSasemDevice->m_psDevice)
			return -ENOENT;
		
		/* set USB device in URB */
		l_psSasemDevice->m_psUrbIn->dev = l_psSasemDevice->m_psDevice;
		
		/* start communication by submitting URB */
#ifndef KERNEL_2_5
		if (usb_submit_urb(l_psSasemDevice->m_psUrbIn)) {
#else
		if (usb_submit_urb(l_psSasemDevice->m_psUrbIn, SLAB_ATOMIC)) {
#endif
			err(" URB submit failed\n");
			return -EIO;
		}
		
		/* indicate that URB has been submitted */
		l_psSasemDevice->m_iConnected = 1;
	}

	dbg(" }\n");
	return 0;
}

static void s_iLircSetUseDec(void *p_pData) {
	t_sasemDevice *l_psSasemDevice = p_pData;
	int l_iDevnum;
	
	dbg(" {\n");
	if (!l_psSasemDevice) {
		err(" no context\n");
		return;
	}

	l_iDevnum = l_psSasemDevice->m_iDevnum;

	if (l_psSasemDevice->m_iConnected) {

		/*
			URB has been submitted before so it can be unlinked
		*/

		down_interruptible(&l_psSasemDevice->m_sSemLock);
		usb_unlink_urb(l_psSasemDevice->m_psUrbIn);
		l_psSasemDevice->m_iConnected = 0;
		up(&l_psSasemDevice->m_sSemLock);
	}
	dbg(" }\n");
}

/* FS Operations for LCDProc */

static int s_iSasemFSOpen(t_inode *p_psInode, t_file *p_psFile) {
	t_sasemDevice *l_psSasemDevice;
	int l_iReturn = 0;
	int l_iPipe;

	dbg(" {\n");
	if (s_psSasemDevice == NULL) {
		err(" no device\n");
		return -ENODEV;
	}

	l_psSasemDevice = s_psSasemDevice;
	down(&l_psSasemDevice->m_sSemLock);

	if (l_psSasemDevice->m_iOpen) {
		// already open
		dbg(": already open\n");

		// return error immediately
		if (p_psFile->f_flags & O_NONBLOCK) {
			up(&l_psSasemDevice->m_sSemLock);
			return -EAGAIN;
		}

		dbg(": open & block\n");
		// wait for release, on global variable
		if ((l_iReturn = 
			wait_event_interruptible(l_psSasemDevice->m_sQueueOpen, 
				s_psSasemDevice !=NULL))) {
			up(&l_psSasemDevice->m_sSemLock);
			return l_iReturn;
		}
	}

	// indicate status as open
	l_psSasemDevice->m_iOpen=1;

	// handle URB for Out Interface
#ifndef KERNEL_2_5
	l_psSasemDevice->m_psUrbOut = usb_alloc_urb(0);
#else
	l_psSasemDevice->m_psUrbOut = usb_alloc_urb(0, GFP_KERNEL);
#endif

/*	l_iPipe = usb_sndintpipe(l_psSasemDevice->m_psDevice,
			l_psSasemDevice->m_psDescriptorOut->bEndpointAddress);
*/    
	dbg(": init outbound URB\n");
	l_iPipe = usb_sndbulkpipe(l_psSasemDevice->m_psDevice,
			l_psSasemDevice->m_psDescriptorOut->bEndpointAddress);

	usb_fill_int_urb(l_psSasemDevice->m_psUrbOut, 
			l_psSasemDevice->m_psDevice,
			l_iPipe, l_psSasemDevice->m_aucBufferOut,
			sizeof(l_psSasemDevice->m_aucBufferOut),
			s_SasemCallbackOut, l_psSasemDevice, 
			l_psSasemDevice->m_psDescriptorOut->bInterval);

	// store pointer to device in file handle
	p_psFile->private_data = l_psSasemDevice;
	up(&l_psSasemDevice->m_sSemLock);
	dbg(" }\n");
	return 0;
}

static int s_iSasemFSRelease(t_inode *p_psInode, t_file *p_psFile) {
	t_sasemDevice *lp_sasemDevice;

	dbg(" {\n");
	// get pointer to device
	lp_sasemDevice = (t_sasemDevice *)p_psFile->private_data;
	down(&lp_sasemDevice->m_sSemLock);

	// is the device open?
	if ((lp_sasemDevice) && (lp_sasemDevice->m_iOpen)) {
		dbg(": check open\n");

		// yes, free URB and set status to closed
		usb_unlink_urb(lp_sasemDevice->m_psUrbOut);
		usb_free_urb(lp_sasemDevice->m_psUrbOut);
		lp_sasemDevice->m_psUrbOut = NULL;
		lp_sasemDevice->m_iOpen = 0;
	}
	up(&lp_sasemDevice->m_sSemLock);

	// wake up any anybody who is waiting for release
	dbg(": wake up\n");
	wake_up_interruptible(&lp_sasemDevice->m_sQueueOpen);
	dbg(" }\n");
	return 0;
}

static ssize_t s_sSasemFSWrite(t_file *p_psFile, const char *p_pcBuffer,
				size_t p_psCount, loff_t *p_psPos) {
	t_sasemDevice *l_psSasemDevice;
	int l_iCount = 0;
	int l_iResult;

	dbg(" {\n");
	// sanity check
	l_psSasemDevice = (t_sasemDevice *)p_psFile->private_data;
	dbg(": lock\n");
	down(&l_psSasemDevice->m_sSemLock);

	dbg(": check device \n");
	if (l_psSasemDevice->m_psDevice == NULL) {
		dbg(": device is null \n");
		up(&l_psSasemDevice->m_sSemLock);
		return -ENODEV;
	}

	// is device open?
	dbg(": check open \n");
	if (l_psSasemDevice->m_iOpen == 0) {
		dbg(": device not open\n");
		up(&l_psSasemDevice->m_sSemLock);
		return -EBADF;
	}

	if (p_psCount == 0) {
		up(&l_psSasemDevice->m_sSemLock);
		return 0;
	}

	if (l_psSasemDevice->m_psUrbOut->status == -EINPROGRESS) {
		dbg(": status -EINPROGRESS \n");
		up(&l_psSasemDevice->m_sSemLock);
		return 0;
	}

	if (l_psSasemDevice->m_psUrbOut->status) {
		err(": status %d \n", l_psSasemDevice->m_psUrbOut->status);
		up(&l_psSasemDevice->m_sSemLock);
		return -EAGAIN;
	}

	memset(l_psSasemDevice->m_aucBufferOut, 0, 
			sizeof(l_psSasemDevice->m_aucBufferOut));
	l_iCount = (p_psCount>MAX_INTERRUPT_DATA)?MAX_INTERRUPT_DATA:p_psCount;
	copy_from_user(l_psSasemDevice->m_aucBufferOut, p_pcBuffer, l_iCount);

#ifndef KERNEL_2_5
	l_iResult = usb_submit_urb(l_psSasemDevice->m_psUrbOut);
#else
	l_iResult = usb_submit_urb(l_psSasemDevice->m_psUrbOut, SLAB_ATOMIC);
#endif

	if (l_iResult) {
		err(": usb_submit_urb failed %d\n", l_iResult);
		l_iCount = l_iResult;
	}
	else {
		// wait for write to finish
		//interruptible_sleep_on(&l_psSasemDevice->m_sQueueWrite);
		wait_event_interruptible(l_psSasemDevice->m_sQueueWrite, l_psSasemDevice->m_psUrbOut->status != -EINPROGRESS);
	}

	up(&l_psSasemDevice->m_sSemLock);
	dbg(" }\n");
	return l_iCount;
}

static ssize_t s_sSasemFSRead(t_file *p_psFile, char *p_pcBuffer,
				size_t p_psCount, loff_t *p_psUnused_pos) {
	dbg(" {}\n");
	// no read support
	return -EINVAL;
}

static int s_iSasemFSIoctl(t_inode *p_psInode, t_file *p_psFile,
				unsigned p_uCmd, unsigned long p_ulArg) {
	int l_i;
	char l_acBuf[30];
	t_sasemDevice *l_psSasemDevice;

	dbg(" {\n");
	// sanity check
	l_psSasemDevice = (t_sasemDevice *)p_psFile->private_data;
	if (!l_psSasemDevice->m_psDevice)
	return -ENOLINK;

	switch (p_uCmd) {

	case IOCTL_GET_HARD_VERSION:
		// return device information
		dbg(": IOCTL_GET_HARD_VERSION\n");
		l_i = (l_psSasemDevice->m_psDevice)->descriptor.bcdDevice;
		sprintf(l_acBuf,"%1d%1d.%1d%1d",
				(l_i & 0xF000)>>12,(l_i & 0xF00)>>8,
				(l_i & 0xF0)>>4,(l_i & 0xF));
		if (copy_to_user((void *)p_ulArg, l_acBuf, strlen(l_acBuf))!=0)
			return -EFAULT;
		break;

	case IOCTL_GET_DRV_VERSION:
		// return driver information
		// sprintf(l_acBuf,"USBLCD Driver Version 1.03");
		dbg(": IOCTL_GET_DRV_VERSION\n");
		sprintf(l_acBuf,DRIVER_DESC);
		if (copy_to_user((void *)p_ulArg, l_acBuf, strlen(l_acBuf))!=0)
			return -EFAULT;
		break;  

	default:
		dbg(": unknown command\n");
		// command not supported
		return -ENOIOCTLCMD;
		break;
	}
	dbg(" }\n");
	return 0;
}

static unsigned s_uSasemFSPoll(t_file *p_psFile, poll_table *p_psWait) {

	dbg(" {}\n");
	// no poll support
	return -EINVAL;
}

#ifndef KERNEL_2_5
static void s_SasemCallbackOut(t_urb *p_psUrb) {
#else
static void s_SasemCallbackOut(t_urb *p_psUrb, struct pt_regs *regs) {
#endif
	t_sasemDevice *l_psSasemDevice;

	dbg(" {\n");

	l_psSasemDevice = p_psUrb->context;  

	// sanity check
	if (p_psUrb->status != 0) {
		err(": urb failed, status %d\n", p_psUrb->status);
	}
	if (waitqueue_active(&l_psSasemDevice->m_sQueueWrite)) {
		dbg(": wake up \n");
		l_psSasemDevice->m_psUrbOut->dev = l_psSasemDevice->m_psDevice;
		wake_up(&l_psSasemDevice->m_sQueueWrite);
	}
	dbg(" }\n");
}

/*
 * USB Microsoft IR Transceiver driver - 0.2
 *
 * Copyright (c) 2003-2004 Dan Conti (dconti@acm.wwu.edu)
 *
 * This driver is based on the USB skeleton driver packaged with the
 * kernel, and the notice from that package has been retained below.
 *
 * The Microsoft IR Transceiver is a neat little IR receiver with two
 * emitters on it designed for Windows Media Center. This driver might
 * work for all media center remotes, but I have only tested it with
 * the philips model. The first revision of this driver only supports
 * the receive function - the transmit function will be much more
 * tricky due to the nature of the hardware. Microsoft chose to build
 * this device inexpensively, therefore making it extra dumb.
 * There is no interrupt endpoint on this device; all usb traffic
 * happens over two bulk endpoints. As a result of this, poll() for
 * this device is an actual hardware poll (instead of a receive queue
 * check) and is rather expensive.
 *
 * All trademarks property of their respective owners. This driver was
 * originally based on the USB skeleton driver, although significant
 * portions of that code have been removed as the driver has evolved.
 *
 * 2003_11_11 - Restructured to minimalize code interpretation in the
 *              driver. The normal use case will be with lirc.
 *
 * 2004_01_01 - Removed all code interpretation. Generate mode2 data
 *              for passing off to lirc. Cleanup
 *
 * 2004_01_04 - Removed devfs handle. Put in a temporary workaround
 *              for a known issue where repeats generate two
 *              sequential spaces (last_was_repeat_gap)
 *
 * 2004_02_17 - Changed top level api to no longer use fops, and
 *              instead use new interface for polling via
 *              lirc_thread. Restructure data read/mode2 generation to
 *              a single pass, reducing number of buffers. Rev to .2
 *
 * 2004_02_27 - Last of fixups to plugin->add_to_buf API. Properly
 *              handle broken fragments from the receiver. Up the
 *              sample rate and remove any pacing from
 *              fetch_more_data. Fixes all known issues.
 * 
 * TODO
 *   - Fix up minor number, registration of major/minor with usb subsystem
 *
 */
/*
 * USB Skeleton driver - 0.6
 *
 * Copyright (c) 2001 Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation; either version 2 of
 *	the License, or (at your option) any later version.
 *
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fcntl.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/smp_lock.h>
#include <linux/usb.h>

#ifdef CONFIG_USB_DEBUG
	static int debug = 1;
#else
	static int debug = 1;
#endif

#include "drivers/lirc.h"
#include "drivers/lirc_dev/lirc_dev.h"

/* Version Information */
#define DRIVER_VERSION "v0.2"
#define DRIVER_AUTHOR "Dan Conti, dconti@acm.wwu.edu"
#define DRIVER_DESC "USB Microsoft IR Transceiver Driver"

/* Module paramaters */
MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "Debug enabled or not");

/* Define these values to match your device */
#define USB_MCEUSB_VENDOR_ID	0x045e
#define USB_MCEUSB_PRODUCT_ID	0x006d

/* table of devices that work with this driver */
static struct usb_device_id mceusb_table [] = {
	{ USB_DEVICE(USB_MCEUSB_VENDOR_ID, USB_MCEUSB_PRODUCT_ID) },
	{ }					/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, mceusb_table);

/* XXX TODO, 244 is likely unused but not reserved */
/* Get a minor range for your devices from the usb maintainer */
#define USB_MCEUSB_MINOR_BASE	244


/* we can have up to this number of device plugged in at once */
#define MAX_DEVICES		16

/* Structure to hold all of our device specific stuff */
struct usb_skel {
	struct usb_device *	    udev;		/* save off the usb device pointer */
	struct usb_interface *	interface;		/* the interface for this device */
	unsigned char   minor;				/* the starting minor number for this device */
	unsigned char   num_ports;			/* the number of ports this device has */
	char            num_interrupt_in;		/* number of interrupt in endpoints we have */
	char            num_bulk_in;			/* number of bulk in endpoints we have */
	char            num_bulk_out;			/* number of bulk out endpoints we have */

	unsigned char *    bulk_in_buffer;		/* the buffer to receive data */
	int                bulk_in_size;		/* the size of the receive buffer */
	__u8               bulk_in_endpointAddr;	/* the address of the bulk in endpoint */

	unsigned char *    bulk_out_buffer;		/* the buffer to send data */
	int	               bulk_out_size;		/* the size of the send buffer */
	struct urb *       write_urb;			/* the urb used to send data */
	__u8               bulk_out_endpointAddr;	/* the address of the bulk out endpoint */

	wait_queue_head_t  wait_q;			/* for timeouts */
	int                open_count;			/* number of times this port has been opened */
	struct semaphore   sem;				/* locks this structure */

	struct lirc_plugin* plugin;
	
	lirc_t lircdata[256];          			/* place to store values until lirc processes them */
	int    lircidx;                			/* current index */
	int    lirccnt;                			/* remaining values */
	
	int    usb_valid_bytes_in_bulk_buffer;		/* leftover data from a previous read */
	int    mce_bytes_left_in_packet;		/* for packets split across multiple reads */
	
	/* Value to hold the last received space; 0 if last value
	 * received was a pulse
	 */
	int    last_space;
	
};

#define MCE_TIME_UNIT 50


/* driver api */
static ssize_t mceusb_write	(struct file *file, const char *buffer,
				 size_t count, loff_t *ppos);

static int mceusb_open		(struct inode *inode, struct file *file);
static int mceusb_release	(struct inode *inode, struct file *file);

static void * mceusb_probe	(struct usb_device *dev, unsigned int ifnum,
				 const struct usb_device_id *id);
static void mceusb_disconnect	(struct usb_device *dev, void *ptr);

static void mceusb_write_bulk_callback	(struct urb *urb);

/* read data from the usb bus; convert to mode2 */
static int msir_fetch_more_data( struct usb_skel* dev, int dont_block );

/* helper functions */
static void msir_cleanup( struct usb_skel* dev );
static int set_use_inc(void* data);
static void set_use_dec(void* data);
    
/* array of pointers to our devices that are currently connected */
static struct usb_skel		*minor_table[MAX_DEVICES];

/* lock to protect the minor_table structure */
static DECLARE_MUTEX (minor_table_mutex);

/*
 * File operations needed when we register this driver.
 * This assumes that this driver NEEDS file operations,
 * of course, which means that the driver is expected
 * to have a node in the /dev directory. If the USB
 * device were for a network interface then the driver
 * would use "struct net_driver" instead, and a serial
 * device would use "struct tty_driver". 
 */
static struct file_operations mceusb_fops = {
	/*
	 * The owner field is part of the module-locking
	 * mechanism. The idea is that the kernel knows
	 * which module to increment the use-counter of
	 * BEFORE it calls the device's open() function.
	 * This also means that the kernel can decrement
	 * the use-counter again before calling release()
	 * or should the open() function fail.
	 *
	 * Not all device structures have an "owner" field
	 * yet. "struct file_operations" and "struct net_device"
	 * do, while "struct tty_driver" does not. If the struct
	 * has an "owner" field, then initialize it to the value
	 * THIS_MODULE and the kernel will handle all module
	 * locking for you automatically. Otherwise, you must
	 * increment the use-counter in the open() function
	 * and decrement it again in the release() function
	 * yourself.
	 */
	owner:		THIS_MODULE,
	
	write:		mceusb_write,
	ioctl:		NULL,
	open:		mceusb_open,
	release:	mceusb_release,
};      


/* usb specific object needed to register this driver with the usb
   subsystem */
static struct usb_driver mceusb_driver = {
	name:		"ir_transceiver",
	probe:		mceusb_probe,
	disconnect:	mceusb_disconnect,
	fops:		NULL, //&mceusb_fops,
	minor:		USB_MCEUSB_MINOR_BASE,
	id_table:	mceusb_table,
};





/**
 *	usb_mceusb_debug_data
 */
static inline void usb_mceusb_debug_data (const char *function, int size,
					  const unsigned char *data)
{
	int i;

	if (!debug)
		return;
	
	printk (KERN_DEBUG __FILE__": %s - length = %d, data = ", 
		function, size);
	for (i = 0; i < size; ++i) {
		printk ("%.2x ", data[i]);
	}
	printk ("\n");
}


/**
 *	mceusb_delete
 */
static inline void mceusb_delete (struct usb_skel *dev)
{
	minor_table[dev->minor] = NULL;
	if (dev->bulk_in_buffer != NULL)
		kfree (dev->bulk_in_buffer);
	if (dev->bulk_out_buffer != NULL)
		kfree (dev->bulk_out_buffer);
	if (dev->write_urb != NULL)
		usb_free_urb (dev->write_urb);
	kfree (dev);
}

static void mceusb_setup( struct usb_device *udev )
{
	char data[8];
	int res;
	
	memset( data, 0, 8 );

	/* Get Status */
	res = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      USB_REQ_GET_STATUS, USB_DIR_IN,
			      0, 0, data, 2, HZ * 3);
    
	/*    res = usb_get_status( udev, 0, 0, data ); */
	dbg(__FUNCTION__ " res = %d status = 0x%x 0x%x",
	    res, data[0], data[1] );
    
	/* This is a strange one. They issue a set address to the device
	 * on the receive control pipe and expect a certain value pair back
	 */
	memset( data, 0, 8 );

	res = usb_control_msg( udev, usb_rcvctrlpipe(udev, 0),
			       5, USB_TYPE_VENDOR, 0, 0,
			       data, 2, HZ * 3 );
	dbg(__FUNCTION__ " res = %d, devnum = %d", res, udev->devnum);
	dbg(__FUNCTION__ " data[0] = %d, data[1] = %d", data[0], data[1] );

    
	/* set feature */
	res = usb_control_msg( udev, usb_sndctrlpipe(udev, 0),
			       USB_REQ_SET_FEATURE, USB_TYPE_VENDOR,
			       0xc04e, 0x0000, NULL, 0, HZ * 3 );
    
	dbg(__FUNCTION__ " res = %d", res);

	/* These two are sent by the windows driver, but stall for
	 * me. I dont have an analyzer on the linux side so i can't
	 * see what is actually different and why the device takes
	 * issue with them
	 */
#if 0
	/* this is some custom control message they send */
	res = usb_control_msg( udev, usb_sndctrlpipe(udev, 0),
			       0x04, USB_TYPE_VENDOR,
			       0x0808, 0x0000, NULL, 0, HZ * 3 );
    
	dbg(__FUNCTION__ " res = %d", res);
    
	/* this is another custom control message they send */
	res = usb_control_msg( udev, usb_sndctrlpipe(udev, 0),
			       0x02, USB_TYPE_VENDOR,
			       0x0000, 0x0100, NULL, 0, HZ * 3 );
    
	dbg(__FUNCTION__ " res = %d", res);
#endif
}

/**
 *	mceusb_open
 */
static int mceusb_open (struct inode *inode, struct file *file)
{
	struct usb_skel *dev = NULL;
	struct usb_device* udev = NULL;
	int subminor;
	int retval = 0;
	
	dbg(__FUNCTION__);

	/* This is a very sucky point. On lirc, we get passed the minor
	 * number of the lirc device, which is totally retarded. We want
	 * to support people opening /dev/usb/msir0 directly though, so
	 * try and determine who the hell is calling us here
	 */
	if( MAJOR( inode->i_rdev ) != USB_MAJOR )
	{
		/* This is the lirc device just passing on the
		 * request. We probably mismatch minor numbers here,
		 * but the lucky fact is that nobody will ever use two
		 * of the exact same remotes with two recievers on one
		 * machine
		 */
		subminor = 0;
	}
	else {
		subminor = MINOR (inode->i_rdev) - USB_MCEUSB_MINOR_BASE;
	}
	if ((subminor < 0) ||
	    (subminor >= MAX_DEVICES)) {
		dbg("subminor %d asldkjfasdkfj", subminor);
		return -ENODEV;
	}

	/* Increment our usage count for the module.
	 * This is redundant here, because "struct file_operations"
	 * has an "owner" field. This line is included here soley as
	 * a reference for drivers using lesser structures... ;-)
	 */

	MOD_INC_USE_COUNT;

	/* lock our minor table and get our local data for this minor */
	down (&minor_table_mutex);
	dev = minor_table[subminor];
	if (dev == NULL) {
		dbg("dev == NULL");
		up (&minor_table_mutex);
		MOD_DEC_USE_COUNT;
		return -ENODEV;
	}
	udev = dev->udev;

	/* lock this device */
	down (&dev->sem);

	/* unlock the minor table */
	up (&minor_table_mutex);

	/* increment our usage count for the driver */
	++dev->open_count;

	/* save our object in the file's private structure */
	file->private_data = dev;

	/* init the waitq */
	init_waitqueue_head( &dev->wait_q );
    
	/* clear off the first few messages. these look like
	 * calibration or test data, i can't really tell
	 * this also flushes in case we have random ir data queued up
	 */
	{
		char junk[64];
		int partial = 0, retval, i;
		for( i = 0; i < 40; i++ )
		{
			retval = usb_bulk_msg 
				(udev, usb_rcvbulkpipe 
				 (udev, dev->bulk_in_endpointAddr),
				 junk, 64,
				 &partial, HZ*10);
		}
	}

	msir_cleanup( dev );
    
	/* unlock this device */
	up (&dev->sem);

	return retval;
}


/**
 *	mceusb_release
 */
static int mceusb_release (struct inode *inode, struct file *file)
{
	struct usb_skel *dev;
	int retval = 0;

	dev = (struct usb_skel *)file->private_data;
	if (dev == NULL) {
		printk (__FUNCTION__ " - object is NULL\n");
		return -ENODEV;
	}

	/* lock our minor table */
	down (&minor_table_mutex);

	/* lock our device */
	down (&dev->sem);

	/* XXX TODO disabled while debugging; there is an issue where
	   the open_count becomes invalid */
#if 0
	if (dev->open_count <= 0) {
		printk (__FUNCTION__ " - device not opened\n");
		retval = -ENODEV;
		goto exit_not_opened;
	}
#endif
	if (dev->udev == NULL) {
		/* the device was unplugged before the file was released */
		up (&dev->sem);
		mceusb_delete (dev);
		up (&minor_table_mutex);
		MOD_DEC_USE_COUNT;
		return 0;
	}

	/* decrement our usage count for the device */
	--dev->open_count;
	if (dev->open_count <= 0) {
		/* shutdown any bulk writes that might be going on */
		usb_unlink_urb (dev->write_urb);
		dev->open_count = 0;
	}

	/* decrement our usage count for the module */
	MOD_DEC_USE_COUNT;

	//exit_not_opened:
	up (&dev->sem);
	up (&minor_table_mutex);

	return retval;
}

static void msir_cleanup( struct usb_skel* dev )
{
	memset( dev->bulk_in_buffer, 0, dev->bulk_in_size );

	dev->usb_valid_bytes_in_bulk_buffer = 0;

	dev->last_space = PULSE_MASK;
    
	dev->mce_bytes_left_in_packet = 0;
	dev->lircidx = 0;
	dev->lirccnt = 0;
	memset( dev->lircdata, 0, sizeof(dev->lircdata) );
}

static int set_use_inc(void* data)
{
	/*    struct usb_skel* skel = (struct usb_skel*)data; */
	MOD_INC_USE_COUNT;
	return 0;
}

static void set_use_dec(void* data)
{
	/* check for unplug here */
	struct usb_skel* dev = (struct usb_skel*) data;
	if( !dev->udev )
	{ 
		lirc_unregister_plugin( dev->minor );
		lirc_buffer_free( dev->plugin->rbuf );
		kfree( dev->plugin->rbuf );
		kfree( dev->plugin );
	}
    
	MOD_DEC_USE_COUNT;
}

/*
 * msir_fetch_more_data
 *
 * The goal here is to read in more remote codes from the remote. In
 * the event that the remote isn't sending us anything, the caller
 * will block until a key is pressed (i.e. this performs phys read,
 * filtering, and queueing of data) unless dont_block is set to 1; in
 * this situation, it will perform a few reads and will exit out if it
 * does not see any appropriate data
 *
 * dev->sem should be locked when this function is called - fine grain
 * locking isn't really important here anyways
 *
 * This routine always returns the number of words available
 *
 */
static int msir_fetch_more_data( struct usb_skel* dev, int dont_block )
{
	int retries = 0;
	int sequential_empty_reads = 0;
	int words_to_read = 
		(sizeof(dev->lircdata)/sizeof(lirc_t)) - dev->lirccnt;
	int partial, this_read = 0;
	int bulkidx = 0;
	int bytes_left_in_packet = 0;
	signed char* signedp = (signed char*)dev->bulk_in_buffer;

	if( words_to_read == 0 )
		return dev->lirccnt;

	/* this forces all existing data to be read by lirc before we
	 * issue another usb command. this is the only form of
	 * throttling we have
	 */
	if( dev->lirccnt )
	{
		return dev->lirccnt;
	}

	/* reserve room for our leading space */
	if( dev->last_space )
		words_to_read--;

	while( words_to_read )
	{
		/* handle signals and USB disconnects */
		if( signal_pending(current) )
		{
			return dev->lirccnt ? dev->lirccnt : -EINTR;
		}
		if( !dev->udev )
		{
			return -ENODEV;
		}

		bulkidx = 0;

		/*
		 * perform data read (phys or from previous buffer)
		 */
        
		/* use leftovers if present, otherwise perform a read */
		if( dev->usb_valid_bytes_in_bulk_buffer )
		{
			this_read = partial = 
				dev->usb_valid_bytes_in_bulk_buffer;
			dev->usb_valid_bytes_in_bulk_buffer = 0;
		}
		else
		{
			int retval;
            
			this_read = dev->bulk_in_size;
			partial = 0;
			retval = usb_bulk_msg
				(dev->udev,
				 usb_rcvbulkpipe
				 (dev->udev, dev->bulk_in_endpointAddr),
				 (unsigned char*)dev->bulk_in_buffer,
				 this_read, &partial, HZ*10);
			
			/* retry a few times on overruns; map all
			   other errors to -EIO */
			if( retval )
			{
				if( retval == USB_ST_DATAOVERRUN && 
				    retries < 5 )
				{
					retries++;
					interruptible_sleep_on_timeout
						( &dev->wait_q, HZ );
					continue;
				}
				else
				{
					return -EIO;
				}
			}
            
			retries = 0;
			if( partial )
				this_read = partial;

			/* skip the header */
			bulkidx += 2;
            
			/* check for empty reads (header only) */
			if( this_read == 2 )
			{
				sequential_empty_reads++;

				/* assume no data */
				/* XXX cleanup */
				if( dont_block &&
				    sequential_empty_reads == 1 )
				{
					break;
				}

				/* sleep for a bit before performing
				   another read */
				interruptible_sleep_on_timeout
					( &dev->wait_q, 1 );
				continue;
			}
			else
			{
				sequential_empty_reads = 0;
			}
            
		}

		/*
		 * process data
		 */
        
		/* at this point this_read is > 0 */
		while( bulkidx < this_read &&
		       (words_to_read > (dev->last_space ? 1 : 0)) )
			//while( bulkidx < this_read && words_to_read )
		{
			int keycode;
			int pulse = 0;
            
			/* read packet length if needed */
			if( !bytes_left_in_packet )
			{
				
				/* we assume we are on a packet length
				 * value. it is possible, in some
				 * cases, to get a packet that does
				 * not start with a length, apparently
				 * due to some sort of fragmenting,
				 * but occaisonally we do not receive
				 * the second half of a fragment
				 */
				bytes_left_in_packet = 
					128 + signedp[bulkidx++];

				/* unfortunately rather than keep all
				 * the data in the packetized format,
				 * the transceiver sends a trailing 8
				 * bytes that aren't part of the
				 * transmittion from the remote,
				 * aren't packetized, and dont really
				 * have any value. we can basically
				 * tell we have hit them if 1) we have
				 * a loooong space currently stored
				 * up, and 2) the bytes_left value for
				 * this packet is obviously wrong
				 */
				if( bytes_left_in_packet > 4  )
				{
					if( dev->mce_bytes_left_in_packet )
					{
						bytes_left_in_packet = dev->mce_bytes_left_in_packet;
						bulkidx--;
					}
					bytes_left_in_packet = 0;
					bulkidx = this_read;
				}

				/* always clear this if we have a
				   valid packet */
				dev->mce_bytes_left_in_packet = 0;
                    
				/* continue here to verify we haven't
				   hit the end of the bulk_in */
				continue;
				
			}

			/*
			 * generate mode2
			 */
            
			keycode = signedp[bulkidx++];
			if( keycode < 0 )
			{
				pulse = 1;
				keycode += 128;
			}
			keycode *= MCE_TIME_UNIT;

			bytes_left_in_packet--;
            
			if( pulse )
			{
				if( dev->last_space )
				{
					/* XXX short term hack */
					if( dev->last_space > 40000 &&
					    dev->last_space < 60000 )
					{
						dev->last_space = 68500;
					}
					
					dev->lircdata[dev->lirccnt++] =
						dev->last_space;
					dev->last_space = 0;
					words_to_read--;

					/* clear the lirc_t for the pulse */
					dev->lircdata[dev->lirccnt] = 0;
				}
				dev->lircdata[dev->lirccnt] += keycode;
				dev->lircdata[dev->lirccnt] |= PULSE_BIT;
			}
			else
			{
				/* on pulse->space transition, add one
				   for the existing pulse */
				if( dev->lircdata[dev->lirccnt] &&
				    !dev->last_space )
				{
					dev->lirccnt++;
					words_to_read--;
				}
                
				dev->last_space += keycode;
			}
		}
	}
	
	/* save off some info if we are exiting mid-packet, or with
	   leftovers */
	if( bytes_left_in_packet )
	{
		dev->mce_bytes_left_in_packet = bytes_left_in_packet;
	}
	if( bulkidx < this_read )
	{
		dev->usb_valid_bytes_in_bulk_buffer = (this_read - bulkidx);
		memcpy( dev->bulk_in_buffer, &(dev->bulk_in_buffer[bulkidx]),
			dev->usb_valid_bytes_in_bulk_buffer );
	}
	return dev->lirccnt;
}

/**
 *	mceusb_write
 */
static ssize_t mceusb_write (struct file *file, const char *buffer,
			     size_t count, loff_t *ppos)
{
	struct usb_skel *dev;
	ssize_t bytes_written = 0;
	int retval = 0;

	dev = (struct usb_skel *)file->private_data;

	dbg(__FUNCTION__ " - minor %d, count = %d", dev->minor, count);

	/* lock this object */
	down (&dev->sem);

	/* verify that the device wasn't unplugged */
	if (dev->udev == NULL) {
		retval = -ENODEV;
		goto exit;
	}

	/* verify that we actually have some data to write */
	if (count == 0) {
		dbg(__FUNCTION__ " - write request of 0 bytes");
		goto exit;
	}

	/* see if we are already in the middle of a write */
	if (dev->write_urb->status == -EINPROGRESS) {
		dbg (__FUNCTION__ " - already writing");
		goto exit;
	}

	/* we can only write as much as 1 urb will hold */
	bytes_written = (count > dev->bulk_out_size) ? 
				dev->bulk_out_size : count;

	/* copy the data from userspace into our urb */
	if (copy_from_user(dev->write_urb->transfer_buffer, buffer, 
			   bytes_written)) {
		retval = -EFAULT;
		goto exit;
	}

	usb_mceusb_debug_data (__FUNCTION__, bytes_written, 
			     dev->write_urb->transfer_buffer);

	/* set up our urb */
	FILL_BULK_URB(dev->write_urb, dev->udev, 
		      usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
		      dev->write_urb->transfer_buffer, bytes_written,
		      mceusb_write_bulk_callback, dev);

	/* send the data out the bulk port */
	retval = usb_submit_urb(dev->write_urb);
	if (retval) {
		err(__FUNCTION__ " - failed submitting write urb, error %d",
		    retval);
	} else {
		retval = bytes_written;
	}

 exit:
	/* unlock the device */
	up (&dev->sem);

	return retval;
}

/* mceusb_add_to_buf: called by lirc_dev to fetch all available keys
 * this is used as a polling interface for us: since we set
 * plugin->sample_rate we will periodically get the below call to
 * check for new data returns 0 on success, or -ENODATA if nothing is
 * available
 */
static int mceusb_add_to_buf(void* data, struct lirc_buffer* buf )
{
	struct usb_skel* dev = (struct usb_skel*)data;

	down( &dev->sem );

	/* verify device still present */
	if( dev->udev == NULL )
	{
		up( &dev->sem );
		return -ENODEV;
	}

	if( !dev->lirccnt )
	{
		int res;
		dev->lircidx = 0;
        
		res = msir_fetch_more_data( dev, 1 );

		if( res == 0 )
			res = -ENODATA;
		if( res < 0 ) {
			up( &dev->sem );
			return res;
		}
	}
	if( dev->lirccnt )
	{
		int keys_to_copy;

		/* determine available buffer space and available data */
		keys_to_copy = lirc_buffer_available( buf );
		if( keys_to_copy > dev->lirccnt )
		{
			keys_to_copy = dev->lirccnt;
		}
        
		lirc_buffer_write_n( buf, (unsigned char*) &(dev->lircdata[dev->lircidx]), keys_to_copy );
		dev->lircidx += keys_to_copy;
		dev->lirccnt -= keys_to_copy;
        
		up( &dev->sem );
		return 0;
	}
    
	up( &dev->sem );
	return -ENODATA;
}

/**
 *	mceusb_write_bulk_callback
 */

static void mceusb_write_bulk_callback (struct urb *urb)
{
	struct usb_skel *dev = (struct usb_skel *)urb->context;

	dbg(__FUNCTION__ " - minor %d", dev->minor);

	if ((urb->status != -ENOENT) && 
	    (urb->status != -ECONNRESET)) {
		dbg(__FUNCTION__ " - nonzero write bulk status received: %d",
		    urb->status);
		return;
	}

	return;
}

/**
 *	mceusb_probe
 *
 *	Called by the usb core when a new device is connected that it
 *	thinks this driver might be interested in.
 */
static void * mceusb_probe(struct usb_device *udev, unsigned int ifnum,
			   const struct usb_device_id *id)
{
	struct usb_skel *dev = NULL;
	struct usb_interface *interface;
	struct usb_interface_descriptor *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	struct lirc_plugin* plugin;
	struct lirc_buffer* rbuf;

	int minor;
	int buffer_size;
	int i;
	
	/* See if the device offered us matches what we can accept */
	if ((udev->descriptor.idVendor != USB_MCEUSB_VENDOR_ID) ||
	    (udev->descriptor.idProduct != USB_MCEUSB_PRODUCT_ID)) {
		return NULL;
	}

	/* select a "subminor" number (part of a minor number) */
	down (&minor_table_mutex);
	for (minor = 0; minor < MAX_DEVICES; ++minor) {
		if (minor_table[minor] == NULL)
			break;
	}
	if (minor >= MAX_DEVICES) {
		info ("Too many devices plugged in, "
		      "can not handle this device.");
		goto exit;
	}

	/* allocate memory for our device state and intialize it */
	dev = kmalloc (sizeof(struct usb_skel), GFP_KERNEL);
	if (dev == NULL) {
		err ("Out of memory");
		goto exit;
	}
	minor_table[minor] = dev;

	interface = &udev->actconfig->interface[ifnum];

	init_MUTEX (&dev->sem);
	dev->udev = udev;
	dev->interface = interface;
	dev->minor = minor;

	/* set up the endpoint information */
	/* check out the endpoints */
	iface_desc = &interface->altsetting[0];
	for (i = 0; i < iface_desc->bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i];

		if ((endpoint->bEndpointAddress & 0x80) &&
		    ((endpoint->bmAttributes & 3) == 0x02)) {
			/* we found a bulk in endpoint */
			buffer_size = endpoint->wMaxPacketSize;
			dev->bulk_in_size = buffer_size;
			dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_in_buffer = kmalloc (buffer_size, GFP_KERNEL);
			if (!dev->bulk_in_buffer) {
				err("Couldn't allocate bulk_in_buffer");
				goto error;
			}
		}
		
		if (((endpoint->bEndpointAddress & 0x80) == 0x00) &&
		    ((endpoint->bmAttributes & 3) == 0x02)) {
			/* we found a bulk out endpoint */
			dev->write_urb = usb_alloc_urb(0);
			if (!dev->write_urb) {
				err("No free urbs available");
				goto error;
			}
			buffer_size = endpoint->wMaxPacketSize;
			dev->bulk_out_size = buffer_size;
			dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_out_buffer = kmalloc (buffer_size, GFP_KERNEL);
			if (!dev->bulk_out_buffer) {
				err("Couldn't allocate bulk_out_buffer");
				goto error;
			}
			FILL_BULK_URB(dev->write_urb, udev, 
				      usb_sndbulkpipe
				      (udev, endpoint->bEndpointAddress),
				      dev->bulk_out_buffer, buffer_size,
				      mceusb_write_bulk_callback, dev);
		}
	}

	/* init the waitq */
	init_waitqueue_head( &dev->wait_q );


	/* Set up our lirc plugin */
	if(!(plugin = kmalloc(sizeof(struct lirc_plugin), GFP_KERNEL))) {
		err("out of memory");
		goto error;
	}
	memset( plugin, 0, sizeof(struct lirc_plugin) );

	if(!(rbuf = kmalloc(sizeof(struct lirc_buffer), GFP_KERNEL))) {
		err("out of memory");
		kfree( plugin );
		goto error;
	}
    
	/* the lirc_atiusb module doesn't memset rbuf here ... ? */
	if( lirc_buffer_init( rbuf, sizeof(lirc_t), 128)) {
		err("out of memory");
		kfree( plugin );
		kfree( rbuf );
		goto error;
	}
    
	strcpy(plugin->name, "lirc_mceusb ");
	plugin->minor       = minor;
	plugin->code_length = sizeof(lirc_t) * 8;
	plugin->features    = LIRC_CAN_REC_MODE2; // | LIRC_CAN_SEND_MODE2;
	plugin->data        = dev;
	plugin->rbuf        = rbuf;
	plugin->ioctl       = NULL;
	plugin->set_use_inc = &set_use_inc;
	plugin->set_use_dec = &set_use_dec;
	plugin->sample_rate = 80;   // sample at 100hz (10ms)
	plugin->add_to_buf  = &mceusb_add_to_buf;
	//    plugin->fops        = &mceusb_fops;
	if( lirc_register_plugin( plugin ) < 0 )
	{
		kfree( plugin );
		lirc_buffer_free( rbuf );
		kfree( rbuf );
		goto error;
	}
	dev->plugin = plugin;

	/* clear off the first few messages. these look like
	 * calibration or test data, i can't really tell
	 * this also flushes in case we have random ir data queued up
	 */
	{
		char junk[64];
		int partial = 0, retval, i;
		for( i = 0; i < 40; i++ )
		{
			retval = usb_bulk_msg
				(udev, usb_rcvbulkpipe
				 (udev, dev->bulk_in_endpointAddr),
				 junk, 64,
				 &partial, HZ*10);
		}
	}
    
	msir_cleanup( dev );
	mceusb_setup( udev );

	/* let the user know what node this device is now attached to */
	//info ("USB Microsoft IR Transceiver device now attached to msir%d", dev->minor);
	goto exit;
	
 error:
	mceusb_delete (dev);
	dev = NULL;

 exit:
	up (&minor_table_mutex);
	return dev;
}

/**
 *	mceusb_disconnect
 *
 *	Called by the usb core when the device is removed from the system.
 */
static void mceusb_disconnect(struct usb_device *udev, void *ptr)
{
	struct usb_skel *dev;
	int minor;

	dev = (struct usb_skel *)ptr;
	
	down (&minor_table_mutex);
	down (&dev->sem);
		
	minor = dev->minor;

	/* unhook lirc things */
	lirc_unregister_plugin( dev->minor );
	lirc_buffer_free( dev->plugin->rbuf );
	kfree( dev->plugin->rbuf );
	kfree( dev->plugin );

	/* if the device is not opened, then we clean up right now */
	if (!dev->open_count) {
		up (&dev->sem);
		mceusb_delete (dev);
	} else {
		dev->udev = NULL;
		up (&dev->sem);
	}

	info("USB Skeleton #%d now disconnected", minor);
	up (&minor_table_mutex);
}



/**
 *	usb_mceusb_init
 */
static int __init usb_mceusb_init(void)
{
	int result;

	/* register this driver with the USB subsystem */
	result = usb_register(&mceusb_driver);
	if (result < 0) {
		err("usb_register failed for the "__FILE__" driver. "
		    "error number %d",
		    result);
		return -1;
	}

	info(DRIVER_DESC " " DRIVER_VERSION);
	return 0;
}


/**
 *	usb_mceusb_exit
 */
static void __exit usb_mceusb_exit(void)
{
	/* deregister this driver with the USB subsystem */
	usb_deregister(&mceusb_driver);
}


module_init (usb_mceusb_init);
module_exit (usb_mceusb_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

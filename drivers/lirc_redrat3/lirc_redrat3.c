/*
 * USB RedRat3 IR Transceiver driver - 0.3
 *
 * This driver is based on the lirc_mceusb driver packaged with
 * the lirc 0.7.2 distribution, which in turn was based upon the
 * usb skeleton driver within the kernel, and the notice from
 * that package has been retained below.
 *
 * Lirc mceusb 0.7.2 Driver Copyright (c) 2003-2004 Dan Conti
 * (dconti@acm.wwu.edu)
 *
 * With thanks to Chris Dodge for his assistance in details for
 * this driver.
 *
 * The RedRat3 is a USB tranceiver with both send & receive,
 * with 2 seperate sensors available for receive to enable
 * both good long range reception for general use, and good
 * short range reception when required for learning a signal.
 *
 * http://www.redrat.co.uk/
 *
 * It uses its own little protocol to communicate, the required
 * parts of which are embedded within this driver.
 *
 * Although originally based upon lirc_mceusb, this resultant
 * driver evolved into something quite different. It has been
 * tested as working with the authors system for the purposes
 * of send, and recv, and recording new remotes, and xmode2
 *
 * For the interested, this driver lives in a file called
 * lirc_mceusb.c to avoid having to make changes to lirc's
 * build system to use it. I guess if it ever gets adopted
 * by lirc, that lirc_rr3.c would be far more appropriate.
 *
 * There are likely unresolved kernel difference issues
 * remaining, and potential issues related to improper
 * shutdown, due to the author moving onto another project
 * and abandoning mainentance of this code.
 *
 * Hopefully there is enough information included that
 * fixes should not be too tricky.
 *
*/
/*
* USB Skeleton driver - 1.1
*
* Copyright (C) 2001-2003 Greg Kroah-Hartman (greg@kroah.com)
*
*	This program is free software; you can redistribute it and/or
*	modify it under the terms of the GNU General Public License as
*	published by the Free Software Foundation, version 2.
*
*
* This driver is to be used as a skeleton driver to be able to create a
* USB driver quickly.  The design of it is based on the usb-serial and
* dc2xx drivers.
*
* Thanks to Oliver Neukum, David Brownell, and Alan Stern for their help
* in debugging this driver.
*
*
* History:
*
* 2003-05-06 - 1.1 - changes due to usb core changes with usb_register_dev()
* 2003-02-25 - 1.0 - fix races involving urb->status, unlink_urb(), and
*			disconnect.  Fix transfer amount in read().  Use
*			macros instead of magic numbers in probe().  Change
*			size variables to size_t.  Show how to eliminate
*			DMA bounce buffer.
* 2002_12_12 - 0.9 - compile fixes and got rid of fixed minor array.
* 2002_09_26 - 0.8 - changes due to USB core conversion to struct device
*			driver.
* 2002_02_12 - 0.7 - zero out dev in probe function for devices that do
*			not have both a bulk in and bulk out endpoint.
*			Thanks to Holger Waechtler for the fix.
* 2001_11_05 - 0.6 - fix minor locking problem in skel_disconnect.
*			Thanks to Pete Zaitcev for the fix.
* 2001_09_04 - 0.5 - fix devfs bug in skel_disconnect. Thanks to wim delvaux
* 2001_08_21 - 0.4 - more small bug fixes.
* 2001_05_29 - 0.3 - more bug fixes based on review from linux-usb-devel
* 2001_05_24 - 0.2 - bug fixes based on review from linux-usb-devel people
* 2001_05_01 - 0.1 - first version
*
 */
#include <linux/autoconf.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/smp_lock.h>
#include <linux/usb.h>
#include <linux/uaccess.h>

#ifdef KERNEL_2_5
#include <linux/completion.h>
#include <asm/uaccess.h>
#else
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/signal.h>
#endif

#ifdef CONFIG_USB_DEBUG
	static int debug = 1;
#else
	static int debug = 1; /* TODO: set this back to 0 */
#endif

#include "drivers/kcompat.h"
#include "drivers/lirc.h"
#include "drivers/lirc_dev/lirc_dev.h"

/* Use our own dbg macro */
#define dprintk(fmt, args...)                             \
	do{                                               \
		if(debug) printk(KERN_DEBUG __FILE__ ": " \
				 fmt "\n", ## args);      \
	}while(0)


#define RR3_MAX_BUF_SIZE       4096
#define RR3_TX_BUFFER_LEN     4096   /* transfer buffer size on bulk transfers*/

#define RR3_MOD_SIGNAL_IN      0x20

#define RR3_RC_DET_ENABLE      0xBB /* Start capture with the RC receiver */
#define RR3_RC_DET_DISABLE     0xBC /* Stop capture with the RC receiver */
#define RR3_RC_DET_STATUS      0xBD /* Return the status of RC detector
                                       capture */

#define RR3_GET_IR_PARAM       0xB8
#define RR3_SET_IR_PARAM       0xB7
#define RR3_IR_IO_MAX_LENGTHS  0x01 /* Max number of lengths in the signal. */
#define RR3_IR_IO_PERIODS_MF   0x02 /* Periods to measure mod. freq. */
#define RR3_IR_IO_SIG_MEM_SIZE 0x03 /* Size of memory for main signal data */
#define RR3_IR_IO_LENGTH_FUZZ  0x04 /* Delta value when measuring lengths */
#define RR3_IR_IO_SIG_TIMEOUT  0x05 /* Timeout for end of signal detection */
#define RR3_IR_IO_MIN_PAUSE    0x06 /* Minumum value for pause recognition. */

#define RR3_CLK                24000000 /* Clock freq. of EZ-USB chip */
#define RR3_CLK_PER_COUNT      12       /* Clock periods per timer count */
#define RR3_CLK_CONV_FACTOR    2000000  /* (RR3_CLK / RR3_CLK_PER_COUNT) */

/* Raw Modulated signal data value offsets */
#define RR3_RAW_MSIG_PAUSE_OFFSET       0
#define RR3_RAW_MSIG_FREQ_COUNT_OFFSET  4
#define RR3_RAW_MSIG_NUM_PERIOD_OFFSET  6
#define RR3_RAW_MSIG_MAX_LENGTHS_OFFSET 8
#define RR3_RAW_MSIG_NUM_LEGNTHS_OFFSET 9
#define RR3_RAW_MSIG_MAX_SIGS_OFFSET    10
#define RR3_RAW_MSIG_NUM_SIGS_OFFSET    12
#define RR3_RAW_MSIG_REPEATS_OFFSET     14

/* Size of the fixed-length portion of the signal */
#define RR3_RAW_MSIG_HEADER_LENGTH      15

/* Raw Modulated signal data value lengths */
#define RR3_RAW_MSIG_PAUSE_LENGTH       4
#define RR3_RAW_MSIG_FREQ_COUNT_LENGTH  2
#define RR3_RAW_MSIG_NUM_PERIOD_LENGTH  2
#define RR3_RAW_MSIG_MAX_LENGTHS_LENGTH 1
#define RR3_RAW_MSIG_NUM_LEGNTHS_LENGTH 1
#define RR3_RAW_MSIG_MAX_SIGS_LENGTH    2
#define RR3_RAW_MSIG_NUM_SIGS_LENGTH    2
#define RR3_RAW_MSIG_REPEATS_LENGTH     1

#define RR3_CPUCS_REG_ADDR     0x7f92 /* The 8051's CPUCS Register address */
#define RR3_RESET              0xA0 /* Reset redrat */
#define RR3_FW_VERSION         0xB1 /* Get the RR firmware version */

#define RR3_DRIVER_MAXLENS 128

/* Version Information */
#define DRIVER_VERSION "v0.0"
#define DRIVER_AUTHOR "The Dweller"
#define DRIVER_DESC "USB RedRat3 IR Transceiver Driver"
#define DRIVER_NAME "lirc_redrat3"

/* Define these values to match your device */
#define USB_RR3USB_VENDOR_ID	0x112a //112a
#define USB_RR3USB_PRODUCT_ID	0x0001

/* table of devices that work with this driver */
static struct usb_device_id rr3usb_table [] = {
	{ USB_DEVICE(USB_RR3USB_VENDOR_ID, USB_RR3USB_PRODUCT_ID) },
	{ }/* Terminating entry */
};

/* we can have up to this number of device plugged in at once */
#define MAX_DEVICES		16

/* Structure to hold all of our device specific stuff */
struct usb_skel {
	struct usb_device *	    udev;		    /* save off the usb device pointer */
	struct usb_interface *	interface;		/* the interface for this device */
	unsigned char   minor;				    /* the starting minor no for this device */
	unsigned char   num_ports;			    /* the number of ports this device has */
	char            num_interrupt_in;		/* number of interrupt in endpoints we have */
	char            num_bulk_in;			/* number of bulk in endpoints we have */
	char            num_bulk_out;			/* number of bulk out endpoints we have */

	unsigned char *    bulk_in_buffer;	    	/* the buffer to receive data */
	int                bulk_in_size;	     	/* the size of the receive buffer */
    struct urb *       read_urb;                /* urb used to read ir data */
	__u8               bulk_in_endpointAddr;	/* the address of the bulk in endpoint */

	unsigned char *    bulk_out_buffer;		    /* the buffer to send data */
	int                bulk_out_size;		    /* the size of the send buffer */
	struct urb *       write_urb;			    /* the urb used to send data */
	__u8               bulk_out_endpointAddr;	/* the address of the bulk out endpoint */

	atomic_t	        write_busy;		    /* true if write urb is busy */
	struct completion	write_finished;		/* wait for the write to finish */

	wait_queue_head_t  wait_q;			/* for timeouts */
	int                open_count;		/* no of times this port has been opened */
	struct semaphore   sem;				/* locks this structure */

	int			present;		/* if the device is not disconnected */

	struct lirc_plugin* plugin;

    atomic_t rr3_recv_in_progress;
    atomic_t rr3_det_enabled;
    atomic_t writing_data;

    char rr3buf[RR3_MAX_BUF_SIZE]; /* store for current packet */
    uint16_t txLength;
    uint16_t txType;
    uint16_t bytesRead;
    int  buftoosmall;
    char *dataPtr;

    int maxLens;

	int    usb_valid_bytes_in_bulk_buffer;		/* leftover data from a previous read */
	int    rr3_bytes_left_in_packet;		    /* for packets split across multiple reads */

	struct timeval last_time;

    int    last_space;

#ifdef KERNEL_2_5
	dma_addr_t dma_in;
	dma_addr_t dma_out;
#endif

};


#define RR3_TIME_UNIT 50

/* driver api */
#ifdef KERNEL_2_5
static int  rr3usb_probe	(struct usb_interface *interface, const struct usb_device_id *id);
static void rr3usb_disconnect	(struct usb_interface *interface);
static void rr3ir_handle_async	(struct urb *urb, struct pt_regs *regs);
#else
static void *rr3usb_probe	(struct usb_device *dev, unsigned int ifnum, const struct usb_device_id *id);
static void rr3usb_disconnect	(struct usb_device *dev, void *ptr);
static void rr3ir_handle_async	(struct urb *urb);
#endif

/* read data from the usb bus; convert to mode2 */
//static int rr3ir_fetch_more_data( struct usb_skel* dev );
static int rr3_enable_detector( struct usb_skel* dev );
static int rr3_disable_detector( struct usb_skel* dev );
static uint8_t rr3_send_cmd( int rr3cmd, struct usb_device *dev );

static ssize_t lirc_write(struct file * file, const char * buf, size_t n, loff_t * pos);
static int lirc_ioctl(struct inode *node,struct file *filep,unsigned int cmd,
		unsigned long arg);


/* helper functions */
static void rrir_cleanup( struct usb_skel* dev );
static void  set_use_dec( void* data );
static int   set_use_inc( void* data );

/* array of pointers to our devices that are currently connected */
static struct usb_skel	*minor_table[MAX_DEVICES];

/* lock to protect the minor_table structure */
static DECLARE_MUTEX (minor_table_mutex);

static void rr3usb_setup( struct usb_device *udev );
static int rr3ir_issue_async( struct usb_skel* dev );

/* usb specific object needed to register this driver with the usb subsystem */
static struct usb_driver rr3usb_driver = {
//        LIRC_THIS_MODULE(.owner = THIS_MODULE)
	.name =		DRIVER_NAME,
	.probe =	rr3usb_probe,
	.disconnect =	rr3usb_disconnect,
	.id_table =	rr3usb_table,
};


void _rr3TranslateFirmwareError(int errCode)
{
  switch (errCode)
  {
  case 0x00:
    dprintk( "No Error");
    break;

    /* Codes 0x20 through 0x2F are reserved for IR Firmware Errors */
  case 0x20:
    dprintk( "Initial signal pulse not long enough "
             "to measure carrier frequency.");
    break;
  case 0x21:
    dprintk( "Not enough length values allocated for signal.");
    break;
  case 0x22:
    dprintk( "Not enough memory allocated for signal data.");
    break;
  case 0x23:
    dprintk( "Too many signal repeats.");
    break;
  case 0x28:
    dprintk( "Insufficient memory available for IR signal "
             "data memory allocation.");
    break;
  case 0x29:
    dprintk( "Insufficient memory available for IrDa signal "
             "data memory allocation.");
    break;

    /* Codes 0x30 through 0x3F are reserved for USB Firmware Errors */
  case 0x30:
    dprintk( "Insufficient memory available for bulk transfer structure.");
    break;

    /* Other error codes... These are primarily errors that can occur in the
     * control messages sent to the redrat
     */
  case 0x40:
    dprintk( "Signal capture has been terminated.");
    break;
  case 0x41:
    dprintk( "Attempt to set/get and unknown signal I/O "
             "algorithm parameter.");
    break;
  case 0x42:
    dprintk( "Signal capture already started.");
    break;

  default:
    dprintk( "Unknown Error");
    break;
  }
}

/* *******************************************************************
 */
static uint32_t
valToModFreq(uint16_t modFreqCount, uint16_t numPeriods)
{
  uint32_t modFreq = 0;

  if (modFreqCount != 0)
  {
    modFreq = (RR3_CLK * numPeriods) / (modFreqCount * RR3_CLK_PER_COUNT);
  }
  else
  {
    modFreq = 0;
  }

  return modFreq;
}

/* *******************************************************************
 */
static uint32_t
lengthToMicrosec(uint32_t length)
{
  //this has int overflow issues..
  //uint32_t biglen = length * 1000000;
  //uint32_t divisor = (RR3_CLK_CONV_FACTOR);
  //return (biglen / divisor)+1; //guarantee a non zero return..

  //this code scales down the figures for the same result...
  uint32_t biglen = length * 1000;
  uint32_t divisor = (RR3_CLK_CONV_FACTOR) / 1000;
  uint32_t result = (uint32_t)(biglen / divisor);

  if(result==0)result=1;//dont allow zero lengths to go back!! lirc breaks!

  //dprintk("input of %lu output of %lu, biglen %lu, divisor %lu",length,result,biglen,divisor);

  return  result;
}

static uint32_t microsecToLen(uint32_t microsec)
{
    //my math is poor!

    //length * 1000   length * 1000000
    //------------- = ---------------- = micro
    //rr3clk / 1000       rr3clk

    //6 * 2       4 * 3        micro * rr3clk          micro * rr3clk / 1000
    //----- = 4   ----- = 6    -------------- = len    ---------------------
    //  3           2             1000000                    1000


      uint32_t biglen = microsec;
      uint32_t divisor = (RR3_CLK_CONV_FACTOR / 1000);
      uint32_t result = (uint32_t)(biglen * divisor)/1000;

      if(result==0)result=1;//dont allow zero lengths to go back!!

      return result;

}

/* *******************************************************************
 */
//struct RR3ModulatedSignal *
void rr3DecodeSignalData(struct usb_skel *dev, uint8_t * sigData, int sigLength)
{
  uint16_t * lengthVals = NULL;
  uint8_t * dataVals = NULL;
  int i = 0;

  uint32_t pause = 0;
  uint16_t modFreqCount = 0;
  uint16_t noPeriods = 0;
  uint8_t maxLengths = 0;
  uint8_t noLengths = 0;
  uint16_t maxSigSize = 0;
  uint16_t sigSize = 0;
  uint8_t noRepeats = 0;

  uint32_t modFreq = 0;
  uint32_t intraSigPause = 0;
  uint32_t * lengths = NULL;
  uint8_t * data = NULL;

  lirc_t singledata = 0;
  lirc_t *multidata = NULL;

  struct timeval now;

  /* Sanity check */
  if(!(sigLength >= RR3_RAW_MSIG_HEADER_LENGTH))
  {
      dprintk("Eh? read returned less than rr3 header len");
  }

  memcpy(&pause, sigData, RR3_RAW_MSIG_PAUSE_LENGTH);
  //dprintk("[pre ntohs] pause %d",pause);
  pause = ntohl(pause);

  memcpy(&modFreqCount, sigData + RR3_RAW_MSIG_FREQ_COUNT_OFFSET,
         RR3_RAW_MSIG_FREQ_COUNT_LENGTH);
  //dprintk("[pre ntohs] modFreqCount %d",modFreqCount);
  modFreqCount = ntohs(modFreqCount);

  memcpy(&noPeriods, sigData + RR3_RAW_MSIG_NUM_PERIOD_OFFSET,
         RR3_RAW_MSIG_NUM_PERIOD_LENGTH);
  noPeriods = ntohs(noPeriods);

  maxLengths = sigData[RR3_RAW_MSIG_MAX_LENGTHS_OFFSET];

  noLengths = sigData[RR3_RAW_MSIG_NUM_LEGNTHS_OFFSET];

  memcpy(&maxSigSize, sigData + RR3_RAW_MSIG_MAX_SIGS_OFFSET,
         RR3_RAW_MSIG_MAX_SIGS_LENGTH);
  maxSigSize = ntohs(maxSigSize);

  memcpy(&sigSize, sigData + RR3_RAW_MSIG_NUM_SIGS_OFFSET,
         RR3_RAW_MSIG_NUM_SIGS_LENGTH);
  sigSize = ntohs(sigSize);

  noRepeats = sigData[RR3_RAW_MSIG_REPEATS_OFFSET];

  /* Calculate values */
  modFreq = valToModFreq(modFreqCount, noPeriods);
  intraSigPause = lengthToMicrosec(pause);

  /* Sanity check(s)... Make sure we actually got a well-formed signal
   * from the redrat... */
  //assert(sigLength >= (15 + (noLengths * sizeof(uint16_t))));

  //TODO: disabled this assert, as some received packets failed on it.
  //assert(sigLength == (15 + (maxLengths * sizeof(uint16_t)) + sigSize));

  /* Here we pull out the 'length' values from the signal */
  lengthVals = (uint16_t *) (sigData + RR3_RAW_MSIG_HEADER_LENGTH);

  lengths = (uint32_t *) kmalloc(noLengths * sizeof(uint32_t), GFP_KERNEL);
  memset(lengths, 0, noLengths * sizeof(uint32_t));

  //dprintk("Got %d lengths",noLengths);
  for (i = 0; i < noLengths; ++i)
  {
    lengths[i] = lengthToMicrosec((uint32_t) ntohs(lengthVals[i]));

    //dprintk("  Length %d was %d microsec (or %d originally)",i,lengths[i], ntohs(lengthVals[i]));

    //cap the value to pulse_mask
    if(lengths[i] > PULSE_MASK)
        lengths[i] = PULSE_MASK;
  }

  dataVals = ( sigData + RR3_RAW_MSIG_HEADER_LENGTH +
               ( maxLengths * sizeof(uint16_t) ) );
  data = (uint8_t *) kmalloc(sigSize, GFP_KERNEL);
  memcpy(data, dataVals, sigSize);

  dprintk("modfreqcount %d, pause %d, modfreq %d, intrasigpause %d, noLengths %d, sizSize %d, noRepeats %d",
          modFreqCount, pause, modFreq,intraSigPause,noLengths,sigSize,noRepeats);

  //now process data into the lirc buffer...
  /* allocate & blank some temporary structures */
  multidata = (lirc_t *)kmalloc((sigSize+1) * sizeof(lirc_t), GFP_KERNEL);
  singledata = 0;

  /* process each rr3 encoded byte into a lirc_t */
  for (i = 0; i < sigSize; i++)
  {
    //dprintk("  length index [%d] is %d",i,data[i]);
    /* set lirc_t length */
    singledata = lengths[ data[i] ];
    if (i % 2)
    {
      /* spaces dont need any additional work..
       * we use this opportunity to 'tune' the
       * detected data a little, to compensate for
       * the detection hardware
       */
      singledata += 130;// + fudge[dev->fudge_index++];
      if (singledata > PULSE_MASK)
        singledata = PULSE_MASK;

      //if(i<2)dprintk("space %ld, %ld, %ld, %ld",singledata, lengths[data[i]], (uint32_t) ntohs(lengthVals[data[i]]), lengthToMicrosec((uint32_t) ntohs(lengthVals[i])));
    }
    else
    {
      /* pulses need the pulsebit setting...
       * we use this opportunity to 'tune' the
       * detected data a little, to compensate for
       * the detection hardware
       */
      singledata = (singledata | PULSE_BIT);
      singledata -= 120;// + fudge[dev->fudge_index++];

      //if(i<2)dprintk("pulse %ld, %ld, %ld, %ld",singledata, lengths[data[i]], (uint32_t) ntohs(lengthVals[data[i]]), lengthToMicrosec((uint32_t) ntohs(lengthVals[i])));
    }
    /* store the lirc_t into the output data array */
    multidata[i+1] = singledata;
  }

  //dprintk("data converted, cloning it to dev struct storage");

  //down(&dev->sem);

  //write the space since the last data...
      do_gettimeofday(&now);
      singledata = now.tv_sec - dev->last_time.tv_sec;
      if(singledata+1 > PULSE_MASK/1000000)
      	singledata = PULSE_MASK;
      else {
      	singledata *= 1000000;
      	singledata += now.tv_usec - dev->last_time.tv_usec;
      }
      dev->last_time.tv_sec = now.tv_sec;
      dev->last_time.tv_usec = now.tv_usec;
      multidata[0]=singledata;

      lirc_buffer_write_n( dev->plugin->rbuf, (unsigned char*) &(multidata[0]), (sigSize+1) );

        //clear up the temp structs.
  kfree(multidata);
  kfree(lengths);
  kfree(data);

  //up(&dev->sem);

  wake_up(&dev->plugin->rbuf->wait_poll);


  return; // result;
}


/* Util fn to send rr3 cmds */
static uint8_t rr3_send_cmd(int rr3cmd, struct usb_device *udev)
{
  uint8_t retval;
  int res=0;

  res = usb_control_msg( udev, usb_rcvctrlpipe(udev, 0),
                               rr3cmd, USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN,
                               0x0000, 0x0000, &retval, 1, HZ * 10 );

  if(res>=0)
  {
    //dprintk("rr3_send_cmd: Success sending rr3 cmd res %d, retval %d", res, retval);
    return retval;
  }
  else
  {
    //dprintk("rr3_send_cmd: Error sending rr3 cmd res %d, retval %d", res, retval);
    return -1;
  }

}

/* Enables the rr3 long range detector, and starts the listen thread */
static int rr3_enable_detector(struct usb_skel *dev)
{
  uint8_t ret = 1;
  if (! atomic_read(&dev->rr3_det_enabled))
  {
    ret = rr3_send_cmd(RR3_RC_DET_STATUS, dev->udev);
    dprintk("rr3: current detector status... %d", ret);

    dprintk("rr3: enabling detector");
    ret = rr3_send_cmd(RR3_RC_DET_ENABLE, dev->udev);
    if ( ret != 0 )
    {
      dprintk("rr3: problem enabling detector");
      //return 0;
    }
    else
    {
      dprintk("rr3: setting detector as enabled within driver");
      atomic_set(&dev->rr3_det_enabled, 1);
      dprintk("rr3: issuing async read against rr3");
      rr3ir_issue_async(dev);
    }

    ret = rr3_send_cmd(RR3_RC_DET_STATUS, dev->udev);
    dprintk("rr3: detector status... %d", ret);
  }
  else
  {
      dprintk("rr3: not enabling detector, as driver state says its enabled already");
  }
  return 1;
}

/* Disables the rr3 long range detector */
static int rr3_disable_detector(struct usb_skel *dev)
{
  uint8_t ret = 1;
  if (atomic_read(&dev->rr3_det_enabled))
  {
    ret = rr3_send_cmd(RR3_RC_DET_STATUS, dev->udev);
    dprintk("rr3: current detector status... %d", ret);

    ret = rr3_send_cmd(RR3_RC_DET_DISABLE, dev->udev);
    if ( ret != 0 )
    {
      dprintk("rr3: problem disabling detector");
    }

    ret = rr3_send_cmd(RR3_RC_DET_STATUS, dev->udev);
    dprintk("rr3: detector status... %d", ret);

    atomic_set(&dev->rr3_det_enabled,ret);
  }
  else
  {
      dprintk("rr3: not disabling detector, as driver state says its disabled already");
  }
  return 1;
}


/**
 *rr3usb_delete
 */
static inline void rr3usb_delete (struct usb_skel *dev)
{
	dprintk("%s", __func__);

    //rr3_disable_detector(dev);

	minor_table[dev->minor] = NULL;
#ifdef KERNEL_2_5
	usb_buffer_free(dev->udev, dev->bulk_in_size, dev->bulk_in_buffer, dev->dma_in);
	usb_buffer_free(dev->udev, dev->bulk_out_size, dev->bulk_out_buffer, dev->dma_out);
#else
	if (dev->bulk_in_buffer != NULL)
		kfree (dev->bulk_in_buffer);
	if (dev->bulk_out_buffer != NULL)
		kfree (dev->bulk_out_buffer);
#endif
	if (dev->write_urb != NULL)
		usb_free_urb (dev->write_urb);

    atomic_set(&dev->rr3_det_enabled,0);

    //TODO: properly handle this..
    if (dev->read_urb != NULL)
    {
        //usb_kill_urb( dev->read_urb );
        usb_unlink_urb( dev->read_urb );
        usb_free_urb (dev->read_urb);
    }

	kfree (dev);
}

static void rr3usb_reset( struct usb_device *udev )
{
    int rc = 0;
    uint8_t flag = 0;
    uint32_t value32 = 0;
    uint16_t value16 = 0;
    uint8_t  value8 =0;

    //dprintk("%s", __func__);

    flag = 0x01;
    rc = usb_control_msg(udev,
                         usb_rcvctrlpipe(udev, 0),
                         RR3_RESET,
                         USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN,
                         RR3_CPUCS_REG_ADDR,
                         0,
                         &flag,
                         sizeof flag,
                         HZ * 25);
    dprintk("rr3: reset returned 0x%02X ",rc);

    value8 = 5;
    rc = usb_control_msg(udev,
                         usb_sndctrlpipe(udev, 0),
                         RR3_SET_IR_PARAM,
                         USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_OUT,
                         RR3_IR_IO_LENGTH_FUZZ,
                         0,
                         &value8,
                         sizeof(value8),
                         HZ * 25);
    dprintk("rr3: set ir parm len fuzz %d rc 0x%02X",value8,rc);
    value8 = RR3_DRIVER_MAXLENS;
    rc = usb_control_msg(udev,
                         usb_sndctrlpipe(udev, 0),
                         RR3_SET_IR_PARAM,
                         USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_OUT,
                         RR3_IR_IO_MAX_LENGTHS,
                         0,
                         &value8,
                         sizeof(value8),
                         HZ * 25);
    dprintk("rr3: set ir parm max lens fuzz %d rc 0x%02X",value8,rc);


    //TMP code to dump params in device...
    value32=0;
    rc = usb_control_msg(udev,
                         usb_rcvctrlpipe(udev, 0),
                         RR3_GET_IR_PARAM,
                         USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN,
                         RR3_IR_IO_SIG_TIMEOUT,
                         0,
                         &value32,
                         sizeof(value32),
                         HZ * 5);
    dprintk("rr3: io sig timeout returned 0x%02X value %d",rc,value32);
    value8=0;
    rc = usb_control_msg(udev,
                         usb_rcvctrlpipe(udev, 0),
                         RR3_GET_IR_PARAM,
                         USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN,
                         RR3_IR_IO_LENGTH_FUZZ,
                         0,
                         &value8,
                         sizeof(value8),
                         HZ * 5);
    dprintk("rr3: io len fuzz returned 0x%02X value %d",rc,value8);
    value8=0;
    rc = usb_control_msg(udev,
                         usb_rcvctrlpipe(udev, 0),
                         RR3_GET_IR_PARAM,
                         USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN,
                         RR3_IR_IO_MAX_LENGTHS,
                         0,
                         &value8,
                         sizeof(value8),
                         HZ * 5);
    dprintk("rr3: io max len returned 0x%02X value %d",rc,value8);

    value16=0;
    rc = usb_control_msg(udev,
                         usb_rcvctrlpipe(udev, 0),
                         RR3_GET_IR_PARAM,
                         USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN,
                         RR3_IR_IO_SIG_MEM_SIZE,
                         0,
                         &value16,
                         sizeof(value16),
                         HZ * 5);
    dprintk("rr3: io sigmem size returned 0x%02X value %d",rc,value16);
}

static void rr3usb_setup( struct usb_device *udev )
{
  int rc = 0;
  char *buffer;
  int size=256;
  buffer = kmalloc (sizeof(char)*(size+1), GFP_KERNEL);
  memset(buffer,0,size+1);

  //dprintk("%s", __func__);

  rr3usb_reset(udev);

  rc = usb_control_msg(udev,
                       usb_rcvctrlpipe(udev, 0),
                       RR3_FW_VERSION,
                       USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN,
                       0,
                       0,
                       buffer,
                       size,
                       HZ * 5);

  //dprintk(" rr3 version res %d (0x%02X)",rc, rc);

  if(rc>=0)
  {
      dprintk("rr3: Firmware : %s",buffer);
  }
  else
  {
      dprintk("rr3: Problem with firmware ID");
  }

  kfree(buffer);

  return;
}

static void rrir_cleanup( struct usb_skel* dev )
{
	memset( dev->bulk_in_buffer, 0, dev->bulk_in_size );

    //dev->rr3_det_enabled = 0;
	//memset( dev->lircdata, 0, sizeof(dev->lircdata) );
}

static int set_use_inc(void* data)
{
    struct usb_skel* dev=(struct usb_skel*)data;

    //dprintk("set use inc");

    dprintk("rr3: set use inc, setting driver state to reading in progress");
    atomic_set(&dev->rr3_recv_in_progress,1);
    dprintk("rr3: enabling detector");
    rr3_enable_detector(dev);

	MOD_INC_USE_COUNT;
	return 0;
}

static void set_use_dec(void* data)
{
    struct usb_skel* dev=(struct usb_skel*)data;

    atomic_set(&dev->rr3_recv_in_progress,0);

	MOD_DEC_USE_COUNT;
}


static int rr3ir_alloc_urb( struct usb_skel *dev)
{
#ifdef KERNEL_2_5
			dev->read_urb = usb_alloc_urb(0, GFP_KERNEL);
            		dprintk("Allocated dev->read_urb=usb_alloc_urb(0, GFP_KERNEL)");

#else
			dev->read_urb = usb_alloc_urb(0);
#endif
			if (!dev->read_urb) {
				err("No free urbs for read available");
			    return -1;
			}

#ifdef KERNEL_2_5
			usb_fill_bulk_urb(dev->read_urb, dev->udev,
				      usb_rcvbulkpipe(dev->udev, 0x82),
				      dev->bulk_in_buffer, dev->bulk_in_size,
				      rr3ir_handle_async, dev);
#else

			FILL_BULK_URB(dev->read_urb, dev->udev,
				      usb_rcvbulkpipe(dev->udev, 0x82),
				      dev->bulk_in_buffer, dev->bulk_in_size,
				      rr3ir_handle_async, dev);
#endif

            return 0;
}
/*
 * rr3ir_issue_async
 *
 *  Issues an async read to the ir data in port.. sets the callback to be rr3ir_handle_async
 */
static int rr3ir_issue_async( struct usb_skel* dev )
{
    int res=0;

    if(atomic_read(&dev->rr3_det_enabled))
    {

        dev->read_urb->dev=dev->udev; //reset the urb->dev ptr

        memset(dev->bulk_in_buffer,0,dev->bulk_in_size);

        if ((res=usb_submit_urb(dev->read_urb, GFP_ATOMIC))) {
            dprintk("issue of async receive request FAILED! (res=%d)\n", res);
	    dprintk("urb->transfer_buffer_length (%d)\n", dev->read_urb->transfer_buffer_length);
//            printUSBErrs();
            return -1;
        }

        dprintk("rr3: after issue of async read (res=%d)\n", res);
    }
    else
    {
        dprintk("rr3: not issuing async read, as det not enabled");
    }

    return 0;
}

#ifdef KERNEL_2_5
static void rr3ir_handle_async (struct urb *urb, struct pt_regs *regs)
#else
static void rr3ir_handle_async (struct urb *urb)
#endif
{
	struct usb_skel *dev;
	int len=0;
    uint16_t txError=0;

    //dprintk("inside async callback");

   	if (!urb)
    {
        dprintk("urb was null - cant clear the read flag! eek!");
		return;
    }


	if ((dev = urb->context))
    {

        if(! atomic_read(&dev->rr3_det_enabled))
        {
            dprintk("received a read callback when detector disabled.. ignoring");
            return;
        }

        //down( &dev->sem );

		len = urb->actual_length;

		//dprintk("async callback called (status=%d len=%d)\n",urb->status,len);

        if(urb->status==0 ) //USB_ST_NOERROR==0
        {
            //dprintk("processing valid usb return");
            if( dev->bytesRead==0 && len >= (sizeof(dev->txType) + sizeof(dev->txLength)))
            {
                dprintk("processing as start of rr3 packet");

                /* grab the Length and type of transfer */
                memcpy(&(dev->txLength), (unsigned char*)dev->bulk_in_buffer, sizeof(dev->txLength));
                memcpy(&(dev->txType), ((unsigned char*)dev->bulk_in_buffer + sizeof(dev->txLength)), sizeof(dev->txType));

                //data needs conversion to know what its real values are
                dev->txLength = ntohs(dev->txLength);
                dev->txType = ntohs(dev->txType);

                //dprintk("urb-status %d txLength of %d, partial of %d, type of 0x%02X",urb->status,dev->txLength, len, dev->txType);

                if(dev->txType != RR3_MOD_SIGNAL_IN)
                {
                    dprintk("ignoring this packet with type 0x%02x, len of %d, 0x%02x",dev->txType,len,dev->txLength);
                    if(dev->txType == 0x01)
                    {
                      memcpy(&txError, ((unsigned char*)dev->bulk_in_buffer + (sizeof(dev->txLength)+sizeof(dev->txType))), sizeof(txError));
                      txError = ntohs(txError);
                      dprintk("txError data was 0x%02x",txError);
                      _rr3TranslateFirmwareError(txError);
                    }
                }
                else
                {
                    dev->bytesRead = len;
                    dev->bytesRead -= ( sizeof(dev->txLength) + sizeof(dev->txType) );
                    dev->dataPtr = &(dev->rr3buf[0]);

                    //cant process the packet if we cant read it all,
                    if(dev->txLength>RR3_MAX_BUF_SIZE)
                        dev->buftoosmall=1;

                    if(!dev->buftoosmall)
                    {
                        memcpy(dev->dataPtr, ((unsigned char*)dev->bulk_in_buffer + sizeof dev->txLength + sizeof dev->txType),dev->bytesRead);
                        dev->dataPtr += dev->bytesRead;
                    }

                    //dprintk("bytesRead %d, txLength %d",dev->bytesRead, dev->txLength);

                    if(dev->bytesRead < dev->txLength)
                    {
                        dprintk("not got complete packet yet.. resumbitting async read urb");
                        int rc =
                            rr3ir_issue_async(dev);
                        dprintk("Resubmit urb returned %d",rc);
                        return;
                    }
                }
            }
            //reading continuation bytes
            else if( dev->bytesRead!=0 )
            {
                len = urb->actual_length;

                //only attempt to clone the data to our local buffer if it was large
                //enough, else we just try to skip the remainder of this packet
                if(!dev->buftoosmall)
                {
                    memcpy(dev->dataPtr, (unsigned char*)dev->bulk_in_buffer, len);
                    dev->dataPtr+=len;
                }

                dev->bytesRead+=len;
                dprintk("buffer now at %d/%d",dev->bytesRead,dev->txLength);

                //if(dev->bytesRead < dev->txLength)
                //{
                //   rr3ir_issue_async(dev);
                //   return;
                //}
            }
            else if(dev->bytesRead==0) //implication that len is less than sizeof txlen txtype
            {
                //hmm.. got less bytes than minimum
                dprintk("eh? didnt get min length packet");
            }


            if(dev->bytesRead > dev->txLength)
            {
              dprintk("eh? ended up reading %d more bytes than expected",(dev->bytesRead-dev->txLength));
              dev->bytesRead=0;
              dev->txLength=0;
              dev->txType=0;
            }
            else if(dev->bytesRead == dev->txLength)
            {
              //if we get here, our read size matches the expected packet size
              //dprintk("got complete packet from rr3");

              if(dev->txType == RR3_MOD_SIGNAL_IN)
              {
                  //this method will decode rr3 to mode2 data
                  //and append it to the dev->lircdata buf till full
                  if(atomic_read(&dev->rr3_recv_in_progress))
                  {
                      dprintk("type was mod_signal_in, passing off to rr3 decode method");
                      rr3DecodeSignalData(dev, dev->rr3buf, dev->txLength);
                  }
                  else
                  {
                      dprintk("discarding rr3 packet as not recording atm...");
                  }
              }
              else
              {
                  dprintk("txType was not mod_signal_in, discarding packet with type 0x%02X",dev->txType);
              }
              dev->bytesRead=0;
              dev->txLength=0;
              dev->txType=0;
            }
        }
        else
        {
            dprintk("either len (%d) was too small, or status (%d) was bad",len,urb->status);
            //len wasnt big enough to contain the rr3 info.. so we ignore this packet
            //setting recv to 0 means the add_to_buf call can reschedule reads..
            //clear of recv moved to fn exit
            dev->bytesRead=0;
            dev->txLength=0;
            dev->txType=0;
        }

        //up( &dev->sem );

	}

    if(!atomic_read(&dev->writing_data))
        rr3ir_issue_async(dev);
    else
    {
        dprintk("Not reissuing read, as write in progress");
    }
}

/**
 *	rr3usb_write_bulk_callback
 */
#ifdef KERNEL_2_5
static void rr3usb_write_bulk_callback (struct urb *urb, struct pt_regs *regs)
#else
static void rr3usb_write_bulk_callback (struct urb *urb)
#endif
{
	struct usb_skel *dev = (struct usb_skel *)urb->context;

	dprintk("%s - minor %d", __func__, dev->minor);

	if ((urb->status != -ENOENT) &&
	    (urb->status != -ECONNRESET)) {
		dprintk("%s - nonzero write buld status received: %d",
			__func__, urb->status);
		return;
	}

	return;
}

//There has to be a better way!
void dumpIoctl(unsigned int cmd)
{
switch (cmd) {
case LIRC_GET_FEATURES: dprintk("LIRC_GET_FEATURES - %u",LIRC_GET_FEATURES); break;
case LIRC_GET_SEND_MODE: dprintk("LIRC_GET_SEND_MODE - %u",LIRC_GET_SEND_MODE); break;
case LIRC_GET_REC_MODE: dprintk("LIRC_GET_REC_MODE - %u",LIRC_GET_REC_MODE); break;
case LIRC_GET_SEND_CARRIER: dprintk("LIRC_GET_SEND_CARRIER - %u",LIRC_GET_SEND_CARRIER); break;
case LIRC_GET_REC_CARRIER: dprintk("LIRC_GET_REC_CARRIER - %u",LIRC_GET_REC_CARRIER); break;
case LIRC_GET_SEND_DUTY_CYCLE: dprintk("LIRC_GET_SEND_DUTY_CYCLE - %u",LIRC_GET_SEND_DUTY_CYCLE); break;
case LIRC_GET_REC_DUTY_CYCLE: dprintk("LIRC_GET_REC_DUTY_CYCLE - %u",LIRC_GET_REC_DUTY_CYCLE); break;
case LIRC_GET_REC_RESOLUTION: dprintk("LIRC_GET_REC_RESOLUTION - %u",LIRC_GET_REC_RESOLUTION); break;
case LIRC_GET_LENGTH: dprintk("LIRC_GET_LENGTH - %u",LIRC_GET_LENGTH); break;
case LIRC_SET_SEND_MODE: dprintk("LIRC_SET_SEND_MODE - %u",LIRC_SET_SEND_MODE); break;
case LIRC_SET_REC_MODE: dprintk("LIRC_SET_REC_MODE - %u",LIRC_SET_REC_MODE); break;
case LIRC_SET_SEND_CARRIER: dprintk("LIRC_SET_SEND_CARRIER - %u",LIRC_SET_SEND_CARRIER); break;
case LIRC_SET_REC_CARRIER: dprintk("LIRC_SET_REC_CARRIER - %u",LIRC_SET_REC_CARRIER); break;
case LIRC_SET_SEND_DUTY_CYCLE: dprintk("LIRC_SET_SEND_DUTY_CYCLE - %u",LIRC_SET_SEND_DUTY_CYCLE); break;
case LIRC_SET_REC_DUTY_CYCLE: dprintk("LIRC_SET_REC_DUTY_CYCLE - %u",LIRC_SET_REC_DUTY_CYCLE); break;
case LIRC_SET_TRANSMITTER_MASK: dprintk("LIRC_SET_TRANSMITTER_MASK - %u",LIRC_SET_TRANSMITTER_MASK); break;
case LIRC_SET_REC_DUTY_CYCLE_RANGE: dprintk("LIRC_SET_REC_DUTY_CYCLE_RANGE - %u",LIRC_SET_REC_DUTY_CYCLE_RANGE); break;
case LIRC_SET_REC_CARRIER_RANGE: dprintk("LIRC_SET_REC_CARRIER_RANGE - %u",LIRC_SET_REC_CARRIER_RANGE); break;
default: dprintk("bad lirc ioctl.. norty norty - %u",cmd);
}
}

unsigned int freq = 38000; // default.. will get overridden by any sends with a freq defined
static int lirc_ioctl(struct inode *node,struct file *filep,unsigned int cmd,
		      unsigned long arg)
{
    int result;
    unsigned long value;
	unsigned int ivalue;
    dprintk("servicing ioctl request");

    switch(cmd)
    {
    case LIRC_SET_SEND_CARRIER:
        result=get_user(ivalue,(unsigned int *) arg);
        if(result) return(result);

        dprintk(" SET_SEND_CARRIER old: %u new :%u \n",freq,ivalue);

        if(freq!=ivalue) freq=ivalue;

        return 0;
        break;

	case LIRC_GET_SEND_MODE:

		result=put_user(LIRC_MODE2SEND(LIRC_MODE_PULSE),
				(unsigned long *) arg);

        dprintk(" GET SEND MODE %d",result);

		if(result) return(result);
		break;

    case LIRC_SET_SEND_MODE:

		result=get_user(value,(unsigned long *) arg);

        dprintk(" SET SEND MODE %d",result);

		if(result) return(result);
		break;

	case LIRC_GET_LENGTH:
		return(-ENOSYS);
		break;

    default:
        dumpIoctl(cmd);
        return(-ENOIOCTLCMD);
    }
    return(0);

}

void dumpEncodedSigData(char* origbuffer)
{
    int i=0;
    uint32_t tmpi;
    uint16_t tmps;
    uint8_t tmp8;
    uint8_t lens;
    uint16_t * lengthVals = NULL;
    uint8_t * dataVals = NULL;
    char *buffer=origbuffer+4;

    dprintk("Signal Dump");

    memcpy(&tmps,origbuffer,sizeof(tmps));
    tmps=ntohs(tmps);
    dprintk(" Size %d",tmps);

    memcpy(&tmps,origbuffer+2,sizeof(tmps));
    tmps=ntohs(tmps);
    dprintk(" Type 0x%02x",tmps);

    memcpy(&tmpi,buffer,sizeof(tmpi));
    tmpi=ntohl(tmpi);
    dprintk(" Intrasig %d",tmpi);

    memcpy(&tmps,buffer+4,sizeof(tmps));
    tmps=ntohs(tmps);
    dprintk(" Modfreq %d",tmps );

    memcpy(&tmp8,buffer+9,1);
    //tmp8=ntohl(tmp8);
    dprintk(" No of lens %d",tmp8);
    lens=tmp8;

    tmps=0;
    memcpy(&tmps,buffer+12,2);
    tmps=ntohs(tmps);
    dprintk(" Sigsize %d",tmps);

    memcpy(&tmp8,buffer+14,1);
    //tmp8=ntohs(tmp8);
    dprintk(" Repeats %d",tmp8);

    lengthVals = (uint16_t *) (buffer + 15);

    i=lens;
    while(i>0)
    {
        dprintk("len %d",ntohs(lengthVals[lens-i]));
        i--;
    }

    dprintk("Indexes");

    dataVals = ( buffer + 15 +
                 ( 128 * sizeof(uint16_t) ) );  //read 128 from ctrl msg

    if(tmps>256){ dprintk("SIGsize is fubar >256"); tmps=256;}
    i=tmps;
    while(i>0)
    {
        if(i>10)
        {
          dprintk("(%d-%d) %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                  tmps-i,tmps-i+9,
                  dataVals[tmps-i],dataVals[tmps-i+1],dataVals[tmps-i+2],dataVals[tmps-i+3],dataVals[tmps-i+4],
                  dataVals[tmps-i+5],dataVals[tmps-i+6],dataVals[tmps-i+7],dataVals[tmps-i+8],dataVals[tmps-i+9]
                  );
          i-=10;
        }
        else if(i>5)
        {
            dprintk("(%d-%d) %02X %02X %02X %02X %02X",
                    tmps-i,tmps-i+5,
                    dataVals[tmps-i],dataVals[tmps-i+1],dataVals[tmps-i+2],dataVals[tmps-i+3],dataVals[tmps-i+4]
                    );
            i-=5;
        }
        else
        {
            dprintk("(%d) %02X",tmps-i,dataVals[tmps-i]);
            i-=1;
        }
    }

    dprintk("\n");

}

/*
static uint16_t
modFreqToVal(double modFreq)
{
  int mult = RR3_CLK / 4;
  return (uint16_t) (65536 - (mult / modFreq));
}
*/

#define WBUF_LEN 256

static uint16_t
modFreqToVal(unsigned int modFreq)
{
  dprintk("converting modFreq %du",modFreq);

  //#define RR3_CLK                24000000
  //int mult = RR3_CLK / 4; /* Clk used in mod. freq. generation is CLK24/4. */
  int mult=6000000;

  dprintk("using mult of %d",mult);
  dprintk("using subval of %d",(mult/modFreq));

  return (uint16_t) (65536 - (mult / modFreq));
}

static struct usb_skel *usbdevs[MAX_IRCTL_DEVICES];

static ssize_t lirc_write(struct file *file, const char *buf,
			 size_t n, loff_t * ppos)
{
    struct usb_skel *dev = usbdevs[0];

	int retval,i,j,count;

    lirc_t wbuf[WBUF_LEN];
    lirc_t lircLens[126]; //rr3 max lengths..
    uint8_t *sigdata = NULL;

    int lencheck=0;
    int curlencheck=0;


    if(dev == NULL)
        return -EINVAL;

    if(atomic_read(&dev->writing_data)) // write in progress? make this atomic if it helps.
    {
        dprintk("write called when already writing? eh??");
        return -EINVAL;
    }

	if(n%sizeof(lirc_t)) return(-EINVAL);
	retval=!access_ok(VERIFY_READ,buf,n);
	if(retval)
    {
        return(retval);
    }

	count=n/sizeof(lirc_t);
	if(count>WBUF_LEN || count%2==0) return(-EINVAL);
	copy_from_user(wbuf,buf,n);

    //down( &dev->sem );
    dprintk("setting writing data to 1");
    atomic_set(&dev->writing_data, 1);

    dprintk("requesting detector shutdown");
    rr3_disable_detector(dev);

    if(atomic_read(&dev->rr3_det_enabled))
    {
        dprintk("hmm.. det still enabled after requesting disable, clearing write flag");
        atomic_set(&dev->writing_data,0);
        dprintk("could be in a right pickle now with no read waiting anymore...");
        return -EINVAL;
    }

    memset(lircLens,0,(sizeof(lirc_t) * 126));

    for(i=0;i<count;i++)
    {
        //dprintk("lirc send byte %lu",wbuf[i]);

        for(lencheck=0; lencheck<curlencheck; lencheck++)
        {
            if(lircLens[lencheck] == microsecToLen(wbuf[i] & PULSE_MASK))
            {
                //dprintk("Found match at position %d for index %d value %d",lencheck,i,wbuf[i]);
                break;
            }
        }
        //dprintk("after lencheck %d, curlencheck %d, val %d",lencheck,curlencheck,wbuf[i]);
        if( lencheck==curlencheck )
        {
            dprintk("adding length from index %d (%u) to 2list@ %d, enc %u",i,wbuf[i],curlencheck,microsecToLen(wbuf[i] & PULSE_MASK));
            if(curlencheck<255)
            {
                //now convert the value to a proper rr3 value..
                lircLens[curlencheck]=microsecToLen(wbuf[i] & PULSE_MASK);
                curlencheck++;
            }
            else
                dprintk("Max rr3 lengths exceeded for send command.. ");
                //TODO: add a rescan with increasing 'fudge factors' to make similar amounts the same.
        }
    }

  sigdata = (uint8_t*)kmalloc((sizeof(uint8_t)*(count + 1 + 1)), GFP_KERNEL);
  sigdata[count] = 127;
  sigdata[count + 1] = 127;
  for (i = 0; i < count; i++)
  {
    for (j = 0; j < curlencheck; j++)
    {
      if ( lircLens[j] == microsecToLen(wbuf[i] & PULSE_MASK) )
      {
        sigdata[i] = j;
      }
    }
  }


  for(i=0;i<curlencheck;i++)
  {
      dprintk("Length val %d - encoded %u de-encoded %u",i,lircLens[i],lengthToMicrosec(lircLens[i]));
  }
  //for(i=0;i<count;i++)
  //{
  //    dprintk("Index %d is length %d",i,lircLens[sigdata[i]]);
  //}
  int sendret=0;

  uint16_t modfreq = modFreqToVal(freq);
  uint16_t intrasig = lengthToMicrosec(100);

  uint32_t tmpi = 0;
  uint16_t tmps = 0;
  uint8_t tmp8 =0;
  int resultLength = 0;
  uint16_t * lengthsPtr = NULL;
  uint8_t * dataPtr = NULL;
  uint8_t maxLengths=RR3_DRIVER_MAXLENS; // read this from ctrl msg...
  /*
    int rc = usb_control_msg(dev->udev,
                         usb_rcvctrlpipe(dev->udev, 0),
                         RR3_GET_IR_PARAM,
                         USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN,
                         RR3_IR_IO_MAX_LENGTHS,
                         0,
                         &maxLengths,
                         sizeof(maxLengths),
                         HZ * 5);
    dprintk("rr3: io max len returned 0x%02X value %d",rc,maxLengths);
   */
  int sendBufLength = RR3_RAW_MSIG_HEADER_LENGTH + (sizeof(uint16_t) * maxLengths) + (sizeof(uint8_t)*(count+1+1)) + (sizeof(uint16_t) * 2);
  char *buffer = (char *)kmalloc( sendBufLength, GFP_KERNEL );

  dprintk("Allocated buffer of %d bytes for send",sendBufLength);

  memset(buffer, 0, sendBufLength);

  dprintk("Encoding intrasig of %d",intrasig);
  tmpi = htonl(intrasig);
  memcpy(buffer+4, &tmpi, sizeof(tmpi));

  dprintk("Encoding modfreq of %d based from freq %d",modfreq,freq);
  tmps = htons(modfreq);
  memcpy(buffer+4 + 4, &tmps, 2);

  dprintk("Encoding no of lens of %d",curlencheck);
  tmp8 = curlencheck;
  buffer[9+4] = tmp8;

  dprintk("Encoding sigsize of %d",count+1+1);
  tmps = htons(count+1+1);
  memcpy(buffer+4 + 12, &tmps, 2);

  //lirc handles repeats externally.. not rr3
  dprintk("Encoding zero repeats");
  buffer[14+4] = 0;


  lengthsPtr = (uint16_t *) (buffer+4 + 15);
  for (i = 0; i < curlencheck; ++i)
  {
    lengthsPtr[i] = htons(lircLens[i]);
  }

  dataPtr = (uint8_t *) (buffer+4 + 15 + (sizeof(uint16_t) * maxLengths));
  memcpy(dataPtr, sigdata, (count+1+1));

  //now put the first two parts of the header into the packet
  tmps = htons(sendBufLength-(sizeof(uint16_t) *2));
  memcpy(buffer, &tmps, 2);

  tmps = htons(0x21);  //rr3 mod sig out
  memcpy(buffer + 2, &tmps, 2);

  //dumpEncodedSigData(buffer);

    tmps = usb_bulk_msg(dev->udev, usb_sndbulkpipe (dev->udev, dev->bulk_out_endpointAddr), buffer, sendBufLength, &resultLength, 10*HZ);
    dprintk("Just sent %d bytes, rc from last send %d",resultLength,tmps);

    //now tell rr3 to transmit what we sent it...
    sendret=0; tmps=0;
    sendret = usb_control_msg(dev->udev,
                         usb_rcvctrlpipe(dev->udev, 0),
                         0xB3,  //rr3 mod sig out
                         USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_IN,
                         0,
                         0,
                         &tmps,
                         sizeof(tmps),
                         HZ * 10);

    dprintk("Told rr3 to send data, rc %d tmps %d",sendret,tmps);
    if(sendret<0)
    {
        dprintk("Error: sendret<0 (Told rr3 to send data, rc %d tmps %d)",sendret,tmps);
//        printUSBErrs();
    }


    //dev->write_urb->dev=dev->udev; //reset the urb->dev ptr
    //
    //int res=0;
    //if ((res=usb_submit_urb(dev->write_urb, GFP_ATOMIC))) {
    //    dprintk("issue of async write request FAILED! (res=%d)\n", res);
    //    printUSBErrs();
    //}
    //dprintk("async bulk send returned %d",res);

  dprintk("freeing alloc'd buffers");
  kfree(buffer);
  kfree(sigdata);

  dprintk("clearing writing data flag");
  atomic_set(&dev->writing_data, 0);

  dprintk("re-enabling detector");
  rr3_enable_detector(dev);

  //up( &dev->sem );

  return n;
}

static struct file_operations lirc_fops =
{
	write:   lirc_write,
};

/**
 *	rr3usb_probe
 *
 *	Called by the usb core when a new device is connected that it
 *	thinks this driver might be interested in.
 */
#ifdef KERNEL_2_5
static int rr3usb_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct usb_host_interface *iface_desc;
#else
static void * rr3usb_probe(struct usb_device *udev, unsigned int ifnum,
			   const struct usb_device_id *id)
{
	struct usb_interface *interface = &udev->actconfig->interface[ifnum];
	struct usb_interface_descriptor *iface_desc;
#endif
	struct usb_skel *dev = NULL;
	struct usb_endpoint_descriptor *endpoint;

	struct lirc_plugin* plugin;
	struct lirc_buffer* rbuf;

	int minor;
	size_t buffer_size;
	int i;
	int retval = -ENOMEM;

	/* See if the device offered us matches what we can accept */
	if ((udev->descriptor.idVendor != USB_RR3USB_VENDOR_ID) ||
	    (udev->descriptor.idProduct != USB_RR3USB_PRODUCT_ID)) {
	    	dprintk("Wrong Vendor/Product IDs");
#ifdef KERNEL_2_5
		return -ENODEV;
#else
		return NULL;
#endif
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
		goto error;
	}

	/* allocate memory for our device state and initialize it */
	dev = kmalloc (sizeof(struct usb_skel), GFP_KERNEL);
	if (dev == NULL) {
		err ("Out of memory");
#ifdef KERNEL_2_5
		retval = -ENOMEM;
#endif
		goto error;
	}
	minor_table[minor] = dev;

	memset (dev, 0x00, sizeof (*dev));
	init_MUTEX (&dev->sem);
	dev->udev = udev;
	dev->interface = interface;
	dev->minor = minor;

    //for now.. we just store the rr3 into position 0.. we'll fix this later..
    //its only used for write..
    usbdevs[0]=dev;

	/* set up the endpoint information */
	/* check out the endpoints */
	/* use only the first bulk-in and bulk-out endpoints */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,4)
	iface_desc = interface->cur_altsetting;
#else
	iface_desc = &interface->altsetting[0];
#endif

#ifdef KERNEL_2_5
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;
#else
	for (i = 0; i < iface_desc->bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i];
#endif
		if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) &&
		    ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
		     USB_ENDPOINT_XFER_BULK)) {
			//dprintk("we found a bulk in endpoint with #0x%02X",endpoint->bEndpointAddress);

            /* data comes in on 0x82, 0x81 is for other data...*/
            if(endpoint->bEndpointAddress == 0x82)
            {

			buffer_size = endpoint->wMaxPacketSize;
			dev->bulk_in_size = buffer_size;
			dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
#ifdef KERNEL_2_5
			dev->bulk_in_buffer = usb_buffer_alloc
				(udev, buffer_size, GFP_ATOMIC, &dev->dma_in);
#else
			dev->bulk_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
#endif
			if (!dev->bulk_in_buffer) {
				err("Couldn't allocate bulk_in_buffer");
				goto error;
			}

            }
		}

		if (((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == 0x00) &&
		    ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
		     USB_ENDPOINT_XFER_BULK)) {
			//dprintk("we found a bulk out endpoint");
#ifdef KERNEL_2_5
			dev->write_urb = usb_alloc_urb(0, GFP_KERNEL);
#else
			dev->write_urb = usb_alloc_urb(0);
#endif
			if (!dev->write_urb) {
				err("No free urbs for write available");
				goto error;
			}
			buffer_size = endpoint->wMaxPacketSize;
			dev->bulk_out_size = buffer_size;
			dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
#ifdef KERNEL_2_5
			dev->bulk_out_buffer = usb_buffer_alloc(udev, buffer_size, GFP_ATOMIC, &dev->dma_out);
#else
			dev->bulk_out_buffer = kmalloc (buffer_size, GFP_KERNEL);
#endif
			if (!dev->bulk_out_buffer) {
				err("Couldn't allocate bulk_out_buffer");
				goto error;
			}
#ifdef KERNEL_2_5
			usb_fill_bulk_urb(dev->write_urb, udev,
				      usb_sndbulkpipe
				      (udev, endpoint->bEndpointAddress),
				      dev->bulk_out_buffer, buffer_size,
				      rr3usb_write_bulk_callback, dev);
#else
			FILL_BULK_URB(dev->write_urb, udev,
				      usb_sndbulkpipe
				      (udev, endpoint->bEndpointAddress),
				      dev->bulk_out_buffer, buffer_size,
				      rr3usb_write_bulk_callback, dev);
#endif
		}
	}

	if (!(dev->bulk_in_endpointAddr && dev->bulk_out_endpointAddr)) {
		err("Couldn't find both bulk-in and bulk-out endpoints");
		goto error;
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

	/* rbuf is memset inside this method... */
	if( lirc_buffer_init( rbuf, sizeof(lirc_t), 4096)) {
		err("out of memory");
		kfree( plugin );
		kfree( rbuf );
		goto error;
	}

	strcpy(plugin->name, DRIVER_NAME " ");
	plugin->minor       = minor;
	plugin->code_length = sizeof(lirc_t);
	plugin->features    = LIRC_CAN_REC_MODE2 | LIRC_CAN_SEND_PULSE | LIRC_CAN_SET_SEND_CARRIER;
	plugin->data        = dev;
	plugin->rbuf        = rbuf;
	plugin->ioctl       = &lirc_ioctl;
	plugin->fops        = &lirc_fops,
	plugin->set_use_inc = &set_use_inc;
	plugin->set_use_dec = &set_use_dec;
	plugin->sample_rate = 0; //we no longer need a polling callback
	plugin->owner       = THIS_MODULE;

	if( lirc_register_plugin(plugin) < 0 )
	{
		kfree( plugin );
		lirc_buffer_free( rbuf );
		kfree( rbuf );
		goto error;
	}
	dev->plugin = plugin;

	rrir_cleanup( dev );
	rr3usb_setup( udev );

    dev->bytesRead=0;
    dev->dataPtr=NULL;
    dev->txType=0;
    dev->txLength=0;
    dev->buftoosmall=0;
    do_gettimeofday(&dev->last_time);

    rr3ir_alloc_urb( dev );

    atomic_set(&dev->rr3_recv_in_progress,0);
    atomic_set(&dev->writing_data,0);
    atomic_set(&dev->rr3_det_enabled,0);


#ifdef KERNEL_2_5
	/* we can register the device now, as it is ready */
	usb_set_intfdata (interface, dev);
#endif
	/* let the user know what node this device is now attached to */
	//info ("USB RedRat3 IR Transceiver device now attached to msir%d", dev->minor);
	up (&minor_table_mutex);
#ifdef KERNEL_2_5
	return 0;
#else
	return dev;
#endif
 error:
	rr3usb_delete (dev);
	dev = NULL;
	dprintk("%s: retval = %x", __func__, retval);
	up (&minor_table_mutex);
#ifdef KERNEL_2_5
	return retval;
#else
	return NULL;
#endif
}

/**
 *	rr3usb_disconnect
 *
 *	Called by the usb core when the device is removed from the system.
 *
 *	This routine guarantees that the driver will not submit any more urbs
 *	by clearing dev->udev.  It is also supposed to terminate any currently
 *	active urbs.
 */
#ifdef KERNEL_2_5
static void rr3usb_disconnect(struct usb_interface *interface)
#else
static void rr3usb_disconnect(struct usb_device *udev, void *ptr)
#endif
{
	struct usb_skel *dev;
	int minor;
#ifdef KERNEL_2_5
	dev = usb_get_intfdata (interface);
	usb_set_intfdata (interface, NULL);
#else
	dev = (struct usb_skel *)ptr;
#endif

	down (&minor_table_mutex);
	down (&dev->sem);
	minor = dev->minor;

    usbdevs[0]=NULL;

	rr3usb_delete (dev);

	/* unhook lirc things */
	lirc_unregister_plugin( minor );
	lirc_buffer_free( dev->plugin->rbuf );
	kfree( dev->plugin->rbuf );
	kfree( dev->plugin );
#ifdef KERNEL_2_5
	/* terminate an ongoing write */
	if (atomic_read (&dev->write_busy)) {
		usb_unlink_urb (dev->write_urb);
		wait_for_completion (&dev->write_finished);
	}

	/* prevent device read, write and ioctl */
	dev->present = 0;
#endif

	info("RedRat3 IR Transceiver #%d now disconnected", minor);
	up (&dev->sem);
	up (&minor_table_mutex);
}



/**
 *	usb_rr3usb_init
 */
static int __init usb_rr3usb_init(void)
{
	int result;

	/* register this driver with the USB subsystem */
	result = usb_register(&rr3usb_driver);
#ifdef KERNEL_2_5
	if ( result ) {
#else
	if ( result < 0 ) {
#endif
		err("usb_register failed for the " DRIVER_NAME " driver. error number %d",result);
#ifdef KERNEL_2_5
		return result;
#else
		return -1;
#endif
	}

	//info(DRIVER_DESC " " DRIVER_VERSION);
    info("RedRat3 Usb Tranceiver : 0.46");
	return 0;
}


/**
 *	usb_rr3usb_exit
 */
static void __exit usb_rr3usb_exit(void)
{
	/* deregister this driver with the USB subsystem */
	usb_deregister(&rr3usb_driver);
}

module_init (usb_rr3usb_init);
module_exit (usb_rr3usb_exit);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE (usb, rr3usb_table);

module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug enabled or not");

EXPORT_NO_SYMBOLS;

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/malloc.h>

#if CONFIG_MODVERSIONS==1
#define MODVERSIONS
#include <linux/modversions.h>
#endif        

/* For character devices */
#include <linux/fs.h>	    /* The character device definitions are here */
#include <asm/uaccess.h>
#include <linux/wrapper.h>  /* A wrapper which does next to nothing at
			     * at present, but may help for compatibility
			     * with future versions of Linux */

#include "remote.h"

#define SUCCESS 0
#define BUFLEN 4096

#define dprintk     if (debug) printk

static char * keyname(unsigned char v);
static int debug =  1; /* insmod parameter */

/* Buffer for key codes */
static unsigned char buf[BUFLEN];

/* Consumer/Producer pointers */ 
static int head=0, tail=0;

/* Is the device open right now? Used to prevent concurent access into
 * the same device */
static int Device_Open = 0;

#if LINUX_VERSION_CODE > 0x020100
MODULE_PARM(debug,"i");
#endif

static char * keyname(unsigned char v){
  switch (v) {
    case FV98_0:	      return "0";
    case FV98_1:	      return "1";
    case FV98_2:	      return "2";
    case FV98_3:	      return "3";
    case FV98_4:	      return "4";
    case FV98_5:	      return "5";
    case FV98_6:	      return "6";
    case FV98_7:	      return "7";
    case FV98_8:	      return "8";
    case FV98_9:	      return "9";
    case FV98_VOL_PLUS:       return "Vol +";
    case FV98_VOL_MINUS:      return "Vol -";
    case FV98_CHAN_PLUS:      return "Chan +";
    case FV98_CHAN_MINUS:     return "Chan -";
    case FV98_MUTE:	      return "Mute";
    case FV98_FULL_SCREEN:    return "Full Screen";
    default:	dprintk("Error, invalid key %d",v);
		return "";
  }
}

void remote_queue_key(int nr, unsigned long prev, unsigned long curr)
{
       unsigned char code=(curr & BT848_RMTCTL_MASK)>>BT848_RMTCTL_SHIFT;
       
#if 0  
       dprintk("IRCtl%d: %x => %x\n", 
	       nr, (int)(0xff & prev), (int)(0xff & curr ));
#endif

       if (!(prev & BT848_RMTCTL_MASK)) {
	       dprintk("IRCtl%d: %x %s\n", nr, code, keyname(code));
       
	       /* If device closed - do nothing */
	       if (! Device_Open) return; 
	       /* Check if buffer is full, if so drop chars */
	       if (((tail+1)%BUFLEN) == head) {
		       dprintk("IRCtl%d: Buffer overflow\n", nr);
	       } else {
		       /* We still have some space in the buffer */
		       buf[tail++]=code;
		       tail%=BUFLEN;
	       }
       }
	       
}


/* This function is called whenever a process attempts to open the device
 * file */
static int irctl_open(struct inode *inode, struct file *file)
{
  dprintk ("IRCtl_open(%p,%p)\n", inode, file);

  /* This is how you get the minor device number in case you have more
   * than one physical device using the driver. */
  dprintk("IRCtl: %d.%d\n", inode->i_rdev >> 8, inode->i_rdev & 0xFF);

  /* We don't want to talk to two processes at the same time */
  if (Device_Open)
    return -EBUSY;

  /* If this was a process, we would have had to be more careful here.
   *
   * In the case of processes, the danger would be that one process 
   * might have check Device_Open and then be replaced by the schedualer
   * by another process which runs this function. Then, when the first process
   * was back on the CPU, it would assume the device is still not open. 
   * However, Linux guarantees that a process won't be replaced while it is 
   * running in kernel context. 
   *
   * In the case of SMP, one CPU might increment Device_Open while another
   * CPU is here, right after the check. However, in version 2.0 of the 
   * kernel this is not a problem because there's a lock to guarantee
   * only one CPU will be kernel module at the same time. This is bad in 
   * terms of performance, so it will probably be changed in the future,
   * but in a safe way.
   */

  Device_Open++;

  /* Make sure that the module isn't removed while the file is open by 
   * incrementing the usage count (the number of opened references to the
   * module, if it's not zero rmmod will fail)
   */
  MOD_INC_USE_COUNT;

  return SUCCESS;
}


/* This function is called when a process closes the device file. It
 * doesn't have a return value because it can't fail (you must ALWAYS
 * be able to close a device). */
static int irctl_release(struct inode *inode, struct file *file)
{

  dprintk ("IRCtl_release(%p,%p)\n", inode, file);
 
  /* We're now ready for our next caller */
  Device_Open --;

  /* Decrement the usage count, otherwise once you opened the file you'll
   * never get rid of the module.
   */
  MOD_DEC_USE_COUNT;

  return SUCCESS;
}

/* This function is called whenever a process which already opened the
 * device file attempts to read from it. */
static ssize_t irctl_read(/* struct inode *inode, */
			 struct file *file,
			 char *buffer,   /* The buffer to fill with
					     the data */
			 size_t length, /* The length of the buffer
					 *  (mustn't write beyond
					 *  that!) */
			 loff_t *ppos)     
{
       /* Number of bytes actually written to the buffer */
       int bytes_read = 0;
       
#if 0
       dprintk("irctl_read(%p,%p,%d)\n",
	       file, buffer, length);
#endif 
       /* If we're at the end of the message, return 0 (which
	* signifies end of file) */
       if (head == tail)
	       return 0;
       
       /* Actually put the data into the buffer */
       while (head!=tail && length)  {
	       /* Because the buffer is in the user data segment, not
		* the kernel data segment, assignment wouldn't
		* work. Instead, we have to use put_user which copies
		* data from the kernel data segment to the user data
		* segment. */
	       put_user(buf[head++], buffer++);
	       bytes_read ++;
	       length --;
	       head%=BUFLEN;
       }
       
       dprintk ("Read %d bytes, %d left\n",
		bytes_read, length);

       /* Read functions are supposed to return the number of bytes
	* actually inserted into the buffer */
       return bytes_read;
}


/* Module Declarations ********************************************** */

/* The major device number for the device. This is static because it 
 * has to be accessible both for registration and for release. */
static int Major;

/* This structure will hold the functions to be called when 
 * a process does something to the device we created. Since a pointer to
 * this structure is kept in the devices table, it can't be local to
 * init_module. NULL is for unimplemented functions. */

struct file_operations Fops = {
  NULL,   /* seek */
  irctl_read, 
  NULL,   /* write */
  NULL,   /* readdir */
  NULL,   /* poll */
  NULL,   /* ioctl */
  NULL,   /* mmap */
  irctl_open,
  NULL,   /* flush */
  irctl_release  /* a.k.a. close */
};


/* ---------------------------------------------------------------------- */

/* For now it must be a module, i'll figure out static version later
   Dont try to use it as a static version !  */

//EXPORT_NO_SYMBOLS; 

#ifdef MODULE
int init_module(void)
#else
int i2c_remote_init(void)
#endif
{      
       /* Register the character device (atleast try) */
       Major = module_register_chrdev(IRCTL_DEV_MAJOR, 
				      IRCTL_DEV_NAME,
				      &Fops);
       
       /* Negative values signify an error */
       if (Major < 0) {
	       printk ("IRCtl device registration failed with %d\n",
		       Major);
	       return Major;
       }

       printk("IR Remote Control driver registered, at major %d \n", 
	       IRCTL_DEV_MAJOR);

       return 0;
}

#ifdef MODULE
void cleanup_module(void)
{
       int ret;
       
       /* Unregister the device */
       ret = module_unregister_chrdev(IRCTL_DEV_MAJOR, IRCTL_DEV_NAME);
 
       /* If there's an error, report it */ 
       if (ret < 0)
	       printk("Error in module_unregister_chrdev: %d\n", ret);
       else
	       dprintk("Module remote successfully unloaded\n");
}
#endif

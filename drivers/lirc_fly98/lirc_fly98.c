/*
 * Remote control driver for the FlyVideo 98 TV-card
 * 
 * (L) by Pawel T. Jochym <jochym@ifj.edu.pl>
 *        This code is licensed under GNU GPL
 *        For newer versions look at:
 *        http://wolf.ifj.edu.pl/~jochym/FlyVideo98/
 *
 * $Id: lirc_fly98.c,v 1.9 2000/06/18 09:28:01 columbus Exp $
 *
 */

/*
 * History:
 * 0.0.1  First public release
 * 
 * 0.0.2  Lots of changes. Following suggecstions and 
 *        code written by Unai Uribarri <unai@dobra.aic.uniovi.es>
 *        Added blocking reads with wait queue, support for multiple
 *        TV cards, changes in bttv part to further simplify it.
 *        Also modified to support Lirc library/daemon (not fully tested)
 *
 * 0.0.3  Ported to kernel 2.2.10 bttv driver. Poll function added
 *	  some clean-up.
 * From now on RCS log:
 * $Log: lirc_fly98.c,v $
 * Revision 1.9  2000/06/18 09:28:01  columbus
 * fixes for latest kernel,
 * thanks to wollny for the patch
 *
 * Revision 1.8  2000/02/05 12:57:28  columbus
 * corrected EINTR semantics
 *
 * Revision 1.7  1999/08/03 11:53:37  jochym
 * Changed Makefile.am to conform to the rest of the tree. Small modyfication
 * of kernel patch (tristate changed to bool in Config.in). Clean-ups.
 * No real bugs found so far. README modyfied to adapt to this changes.
 * Example configurations moved to live-view directory.
 *
 * Revision 1.6  1999/08/02 07:38:30  jochym
 * Finally a working setup. Further clean-up of the kernel patch (got it
 * even smaller). Configuration files for lircd, lircmd and ~/.lircrc.
 * Lock-up in the bttv caused by wrong initialization order removed.
 * External bttv patch not fully tested yet. Internal version 0.1. BETA.
 *
 */

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
#include <linux/modversions.h>
#endif        

/* For character devices */
#include <linux/fs.h>       /* The character device definitions are here */
#include <asm/uaccess.h>
#include <linux/wrapper.h>  /* A wrapper which does next to nothing at
                             * at present, but may help for compatibility
                             * with future versions of Linux */

#include "../drivers/char/bttv.h"
#include "../drivers/char/bt848.h"
#include "lirc_fly98.h"

#define SUCCESS 0

#define dprintk     if (debug) printk

static int debug = 0; /* insmod parameter */

/* Array of IRCtls structures */
struct irctl irctls[MAX_IRCTL_DEVICES];

#if LINUX_VERSION_CODE > 0x020100
MODULE_PARM(debug,"i");
#endif

void remote_queue_key(int nr, unsigned long code)
{
	struct irctl *ir=&irctls[nr];
	
	code=(code & BT848_RMTCTL_MASK)>>BT848_RMTCTL_SHIFT;	
#if 0	
	dprintk("IRCtl%d: %x => %x\n", 
		nr, (int)(0xff & prev), (int)(0xff & curr ));
#endif

	spin_lock(&ir->lock);
	dprintk("IRCtl%d: %lx\n", nr, code);
	
	/* Check if buffer is full, if so drop chars */
	if (((ir->tail+1)%BUFLEN) == ir->head) {
		dprintk("IRCtl%d: Buffer overflow\n", nr);
	} else {
		/* We still have some space in the buffer */
		ir->buffer[ir->tail++]=(unsigned char)code;
		ir->tail%=BUFLEN;
		wake_up_interruptible(&ir->wait);
	}
	spin_unlock(&ir->lock);
}


/* This function is called whenever a process attempts to open the device
 * file */
static int irctl_open(struct inode *inode, struct file *file)
{
	/* This is how you get the minor device number in case you have more
	 * than one physical device using the driver. 
	 */
	int nr=MINOR(inode->i_rdev);
	
	dprintk("IRCtl: %d.%d open called\n", MAJOR(inode->i_rdev), nr);
	if (nr>=MAX_IRCTL_DEVICES)
		return -ENODEV;

	/* We don't want to talk to two processes at the same time */
	spin_lock(&irctls[nr].lock);
	if (irctls[nr].open) {
		spin_unlock(&irctls[nr].lock);
		return -EBUSY;
	}
	++irctls[nr].open;
	spin_unlock(&irctls[nr].lock);

	/* Flush the buffer */
	irctls[nr].head=irctls[nr].tail;

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
	int nr=MINOR(inode->i_rdev);
	
	spin_lock(&irctls[nr].lock);
	dprintk ("IRCtl%d: release called\n", nr);
 
	/* We're now ready for our next caller */
	--irctls[nr].open;

	/* Flush the buffer */
	irctls[nr].head=irctls[nr].tail;
	spin_unlock(&irctls[nr].lock);
	
	/* Decrement the usage count, 
	 * otherwise once you opened the file you'll
	 * never get rid of the module.
	 */
	MOD_DEC_USE_COUNT;
	
	return SUCCESS;
}

/*
 *      Poll to see if we're readable
 */
static unsigned int irctl_poll(struct file *file, poll_table * wait)
{
	struct irctl *ir=&irctls[MINOR(file->f_dentry->d_inode->i_rdev)];
	
	dprintk("irctl%d: poll head=%d tail=%d\n",
		MINOR(file->f_dentry->d_inode->i_rdev),
		    ir->head, ir->tail);
		    
	poll_wait(file, &ir->wait, wait);
	if (ir->head!=ir->tail)
		return POLLIN | POLLRDNORM;
	return 0;
}

static int irctl_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	int result;
	unsigned long features = LIRC_CAN_REC_CODE, mode;

	switch(cmd)
	{
		case LIRC_GET_FEATURES:
			result = put_user(features,(unsigned long*)arg);
			if(result)
				return(result); 
			break;
		case LIRC_GET_REC_MODE:
			result = put_user(LIRC_MODE_CODE,(unsigned long*)arg);
			if(result)
				return(result); 
			break;
		case LIRC_SET_REC_MODE:
			result = get_user(mode,(unsigned long*)arg);
			if(result)
				return(result);
			if(mode != LIRC_MODE_CODE)
			{
				return -ENOSYS;
			}
			break;
		default:
			return -ENOIOCTLCMD;
	}
	return 0;
}


/* This function is called whenever a process which already opened the
 * device file attempts to read from it. */
static ssize_t irctl_read(struct file *file,
			  char *buffer,   
			  size_t length, 
			  loff_t *ppos)     
{
	/* Minor device number */
	struct irctl *ir=&irctls[MINOR(file->f_dentry->d_inode->i_rdev)];
	int r;
#if 1
	dprintk("irctl_read(%p,%p,%d)\n",
		file, buffer, length);
#endif	
	
	spin_lock(&ir->lock);
	
	if (length != 1) {	/* LIRC_MODE_CODE */
		spin_unlock(&ir->lock);
		return -EIO;
	}
	if (ir->head==ir->tail) {
		spin_unlock(&ir->lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		interruptible_sleep_on(&ir->wait);
		if (signal_pending(current))
		    return -ERESTARTSYS;
		spin_lock(&ir->lock);
	} 
	r=put_user(ir->buffer[ir->head++],(unsigned char *)buffer);
	ir->head%=BUFLEN;
	spin_unlock(&ir->lock);
	dprintk("irctl_read: ret %d data %d\n", r, *buffer);
	return (r==0) ? 1 : -EFAULT;

#if 0
	    /*
	      This version supports long reads
	      switched off for now.
	    */
	/* Number of bytes actually written to the buffer */
	int codes_read = 0;
	while (codes_read < length)  {
	    if (ir->head==ir->tail) {
		spin_unlock(&ir->lock);
		if (file->f_flags & O_NONBLOCK)
		    return codes_read*sizeof(long);
		if (signal_pending(current))
		    return -ERESTARTSYS;
		interruptible_sleep_on(&ir->wait);
		current->state = TASK_RUNNING;
		spin_lock(&ir->lock);
	    } else if (ir->head<ir->tail) {
		int count=MIN(ir->tail-ir->head, length);
		copy_to_user(buffer+codes_read*sizeof(long),
			ir->buffer+ir->head,
			count*sizeof(long));
		codes_read+=count;
		ir->head+=count;
	    } else {
		int count=MIN(BUFLEN-ir->head, length);
		copy_to_user(buffer+codes_read*sizeof(long),
			ir->buffer+ir->head,
			count*sizeof(long));
		codes_read+=count;
		ir->head=(ir->head+count)%BUFLEN;
	    }
	}
	
	dprintk ("Read %d bytes, %d left\n",
		    codes_read*sizeof(long), (length-codes_read)*sizeof(long));
		    
	spin_unlock(&ir->lock);
	/* Read functions are supposed to return the number of bytes
	 * actually inserted into the buffer */
	return codes_read*sizeof(long);
#endif
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
	read:    irctl_read,
	poll:    irctl_poll,
	ioctl:   irctl_ioctl,
	open:    irctl_open,
	release: irctl_release
};


/* ---------------------------------------------------------------------- */

/* For now it must be a module, i'll figure out static version later
   Dont try to use it as a static version !  */

EXPORT_NO_SYMBOLS; 

#ifdef MODULE
int init_module(void)
#else
int i2c_remote_init(void)
#endif
{  	
	int i;
	
	for (i=0; i<MAX_IRCTL_DEVICES; ++i) {
		irctls[i].head=0;
		irctls[i].tail=0;
#if LINUX_VERSION_CODE >= 0x020100
		irctls[i].wait=NULL;
		irctls[i].lock=SPIN_LOCK_UNLOCKED;
#endif
	}
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
	
	bttv_set_gpio_monitor(remote_queue_key);

	printk("IR Remote Control driver registered, at major %d \n", 
		IRCTL_DEV_MAJOR);

	return 0;
}

#ifdef MODULE
void cleanup_module(void)
{
	int ret;
	
	bttv_set_gpio_monitor(NULL);
	/* Unregister the device */
	ret = module_unregister_chrdev(IRCTL_DEV_MAJOR, IRCTL_DEV_NAME);
 
	/* If there's an error, report it */ 
	if (ret < 0){
		printk("Error in module_unregister_chrdev: %d\n", ret);
	} else {
		dprintk("Module remote successfully unloaded\n");
	}
}
#endif

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */

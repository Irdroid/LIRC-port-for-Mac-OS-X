#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/tqueue.h>
#include <linux/poll.h>
#include <linux/kmod.h>

#include <linux/i2c.h>

#include "drivers/lirc.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "lirc_haup.h"

#define dprintk if (debug) printk
#define DEVICE_NAME "hauppauge ir rc"

/* lock for SMP support */
spinlock_t remote_lock;

/* we use a timer_list instead of a tq_struct. */
static struct timer_list lirc_haup_timer;

static struct file_operations lirc_haup_fops = {
	NULL,            /* seek    */
	lirc_haup_read,  /* read    */
	lirc_haup_write, /* write   */
	NULL,            /* readdir */
	lirc_haup_poll,  /* poll    */
	lirc_haup_ioctl, /* ioctl   */
	NULL,            /* mmap    */
	lirc_haup_open,  /* open    */
	NULL,            /* flush   */
	lirc_haup_close  /* release */
};

/* ----------------------------------------------------------------------- */

struct i2c_driver lirc_haup_i2c_driver = 
{
	"remote",                     /* name       */
	I2C_DRIVERID_REMOTE,          /* ID         */
	IR_READ_LOW, IR_READ_HIGH,    /* addr range */

	lirc_haup_attach,
	lirc_haup_detach,
	lirc_haup_command
};


static struct lirc_haup_status remote;

/* some other tunable parameters */
static int polling     = POLLING;

/* Default address, will be overwritten by autodetection */
static int ir_read     = IR_READ_LOW;

/* debug module parameter */
static int debug = 0;

static void lirc_haup_do_timer(unsigned long data)
{
	struct lirc_haup_status *remote = (struct lirc_haup_status *)data;
	unsigned short code;

	spin_lock(&remote_lock);
	
	if (remote->wait_cleanup != NULL) {
		wake_up(&remote->wait_cleanup);
	        spin_unlock(&remote_lock);
		/* Now cleanup_module can return */
		return;
	}

	if (remote->attached) {
	        spin_unlock(&remote_lock);
		code = read_raw_keypress(remote); /* get key from the remote */
		spin_lock(&remote_lock);
		if( (remote->status_changed) ) {
			/* if buffer is full, drop input */
			if (((remote->tail+1)%BUFLEN)==remote->head) {
				dprintk("Input buffer overflow\n");
			} else {
				/* put new code to the buffer */
				remote->buffer[remote->tail++]=code;
				remote->tail%=BUFLEN;
				wake_up_interruptible(&remote->wait_poll);
			}
		}
	}

 	lirc_haup_timer.expires = jiffies + polling;
	add_timer(&lirc_haup_timer);
	
	spin_unlock(&remote_lock);
}

static __u16 read_raw_keypress(struct lirc_haup_status *remote)
{
	unsigned char b1, b2, b3;
	__u16 repeat_bit;
	struct lirc_haup_i2c_info *t = remote->i2c_remote;
	
        LOCK_FLAGS;
        
        LOCK_I2C_BUS(t->bus);
	/* Starting bus */
	i2c_start(t->bus);
	/* Resetting bus */
	spin_lock(&remote_lock);
	i2c_sendbyte(t->bus, ir_read, 0);
	spin_unlock(&remote_lock);
	/* Read first byte: Toggle byte (192 or 224) */
	b1 = i2c_readbyte(t->bus, 0);
	/* Read 2. byte: Key pressed by user */
	b2 = i2c_readbyte(t->bus, 0);
	/* Read 3. byte: Firmware version */
        b3 = i2c_readbyte(t->bus, 0);
	/* Stopping bus */
	i2c_stop(t->bus);
        UNLOCK_I2C_BUS(t->bus);
       
	spin_lock(&remote_lock);

	/* Turn off the red light: Read 5 bytes from the bus */
        LOCK_I2C_BUS(t->bus);
        i2c_read(t->bus, ir_read);
        UNLOCK_I2C_BUS(t->bus);
        LOCK_I2C_BUS(t->bus);
        i2c_read(t->bus, ir_read);
        UNLOCK_I2C_BUS(t->bus);
        LOCK_I2C_BUS(t->bus);
        i2c_read(t->bus, ir_read);
        UNLOCK_I2C_BUS(t->bus);
        LOCK_I2C_BUS(t->bus);
        i2c_read(t->bus, ir_read);
        UNLOCK_I2C_BUS(t->bus);

	if (b1 == REPEAT_TOGGLE_0 || b1 == REPEAT_TOGGLE_1) {
		remote->status_changed = 1;
		dprintk("key %s (0x%02x/0x%02x)\n", keyname(b2), b1, b2);
		if (b1 == remote->last_b1) { /* repeat */
			dprintk("Repeat\n");
		} else {
			remote->last_b1 = b1;
		}
	} else {
		remote->status_changed = 0;
		b1 = b2 = 0xff;
	}

	spin_unlock(&remote_lock);

	repeat_bit=(b1&0x20) ? 0x800:0;
	return (__u16) (0x1000 | repeat_bit | (b2>>2));
}

static char * keyname(unsigned char v){
  switch (v) {
    case REMOTE_0:              return "0";
    case REMOTE_1:              return "1";
    case REMOTE_2:              return "2";
    case REMOTE_3:              return "3";
    case REMOTE_4:              return "4";
    case REMOTE_5:              return "5";
    case REMOTE_6:              return "6";
    case REMOTE_7:              return "7";
    case REMOTE_8:              return "8";
    case REMOTE_9:              return "9";
    case REMOTE_RADIO:          return "Radio";
    case REMOTE_MUTE:           return "Mute";
    case REMOTE_TV:             return "TV";
    case REMOTE_VOL_PLUS:       return "Vol +";
    case REMOTE_VOL_MINUS:      return "Vol -";
    case REMOTE_RESERVED:       return "Reserved";
    case REMOTE_CHAN_PLUS:      return "Chan +";
    case REMOTE_CHAN_MINUS:     return "Chan -";
    case REMOTE_SOURCE:         return "Source";
    case REMOTE_MINIMIZE:       return "Minimize";
    case REMOTE_FULL_SCREEN:    return "Full Screen";
    default:    dprintk("Error, invalid key %d",v);
                return "";
  }
}    

/* ---------------------------------------------------------------------- */

static int lirc_haup_attach(struct i2c_device *device)
{
	struct lirc_haup_i2c_info *t;

	dprintk("Attaching remote device\n");

	if(device->bus->id!=I2C_BUSID_BT848)
		return -EINVAL;
		
	device->data = t = kmalloc(sizeof(struct lirc_haup_i2c_info),
				   GFP_KERNEL);
	if (NULL == t)
		return -ENOMEM;
	memset(t, 0, sizeof(struct lirc_haup_i2c_info));
	strcpy(device->name, "remote");
	t->bus  = device->bus;
	t->addr = device->addr;

	spin_lock(&remote_lock);
	
	ir_read = t->addr;

	remote.i2c_remote = t;
	remote.attached   = 1;
	remote.last_b1    = 0xff;

	spin_unlock(&remote_lock);

/*	MOD_INC_USE_COUNT; */

	return SUCCESS;
}

static int lirc_haup_detach(struct i2c_device *device)
{
	struct lirc_haup_i2c_info *t =
		(struct lirc_haup_i2c_info *)device->data;

	kfree(t);

	spin_lock(&remote_lock);
	
	remote.i2c_remote = NULL;
	remote.attached   = 0;

	spin_unlock(&remote_lock);

/*	MOD_DEC_USE_COUNT; */

	return SUCCESS;
}

static int lirc_haup_command(struct i2c_device *device,
			     unsigned int cmd, void *arg)
{
	dprintk("remote: cmd = %i\n", cmd);
	return SUCCESS;
}

static unsigned int lirc_haup_poll(struct file *file,
				   struct poll_table_struct * wait)
{
        poll_wait(file, &remote.wait_poll, wait);

	spin_lock(&remote_lock);
	if(remote.status_changed) {
		remote.status_changed = 0;
		spin_unlock(&remote_lock);
		return POLLIN | POLLRDNORM;
	} else {
	        spin_unlock(&remote_lock);
		return SUCCESS;
	}
}

static int lirc_haup_ioctl(struct inode *ino, struct file *fp,
			   unsigned int cmd, unsigned long arg)
{
        int result;
	unsigned long features    = LIRC_CAN_REC_LIRCCODE;
	unsigned long mode;
	
	switch(cmd) {
	case LIRC_GET_FEATURES:
		result = put_user(features,(unsigned long *) arg);
		break;
	case LIRC_SET_REC_MODE:
		result = get_user(mode, (unsigned long *) arg);
		if (result) {
			break;
		}
		if ((LIRC_MODE2REC(mode) & features) == 0) {
			result = -EINVAL;
		}
		break;
	case LIRC_GET_LENGTH:
		result = put_user(CODE_LENGTH, (unsigned long *)arg);
		break;
	default:
		result = -ENOIOCTLCMD;
	}
	return result;
}

/* This function is called whenever a process attempts to open the device
 * file */
static int lirc_haup_open(struct inode *inode, struct file *file)
{
        spin_lock(&remote_lock);
  
	/* We don't want to talk to two processes at the same time */
	if (remote.open) {
	        spin_unlock(&remote_lock);
		return -EBUSY;
	}
	
	remote.open           = 1;
#if LINUX_VERSION_CODE < 0x020300
	remote.wait_poll      = NULL;
	remote.wait_cleanup   = NULL;
#else
	init_waitqueue_head(&remote.wait_poll);
	init_waitqueue_head(&remote.wait_cleanup);
#endif
	remote.status_changed = 0;

	init_timer(&lirc_haup_timer);
	lirc_haup_timer.function = lirc_haup_do_timer;
	lirc_haup_timer.data     = (unsigned long)&remote;
	lirc_haup_timer.expires  = jiffies + polling;
	add_timer(&lirc_haup_timer);

	spin_unlock(&remote_lock);

	/* initialize input buffer pointers */
	remote.head=remote.tail=0;
	
	/* Make sure that the module isn't removed while the file is open by 
	 * incrementing the usage count (the number of opened references to the
	 * module, if it's not zero rmmod will fail)
	 */
	MOD_INC_USE_COUNT;

	return SUCCESS;
}

/* This function is called when a process closes the device file. */
static int lirc_haup_close(struct inode *inode, struct file *file)
{
	sleep_on(&remote.wait_cleanup);

        spin_lock(&remote_lock);
  
	remote.open = 0;

	/* flush the buffer */
	remote.head=remote.tail=0;
	
	del_timer(&lirc_haup_timer);

	spin_unlock(&remote_lock);
	
	/* Decrement the usage count, 
	 * otherwise once you opened the file you'll
	 * never get rid of the module.
	 */
	MOD_DEC_USE_COUNT;
	
	return SUCCESS;
}

static ssize_t lirc_haup_read(struct file * file, char * buffer,
			      size_t length, loff_t *ppos)
{
        unsigned char data[2];

	spin_lock(&remote_lock);
	
	if(length != 2) {
	        spin_unlock(&remote_lock);
		return -EIO; /* we are only prepared to send 2 bytes */
	}

	/* if input buffer is empty, wait for input */
	if (remote.head==remote.tail) {
		spin_unlock(&remote_lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (signal_pending(current))
			return -ERESTARTSYS;
		interruptible_sleep_on(&remote.wait_poll);
		current->state = TASK_RUNNING;
		spin_lock(&remote_lock);
	}
	
	data[0]=(unsigned char) ((remote.buffer[remote.head]>>8)&0x1f);
	data[1]=(unsigned char) (remote.buffer[remote.head++]&0xff);
	remote.head%=BUFLEN;
	
	if(copy_to_user(buffer,data,2)) {
	        spin_unlock(&remote_lock);
	        return(-EFAULT);
	}

	spin_unlock(&remote_lock);
	
	return length;
}


static ssize_t lirc_haup_write(struct file * file, const char * buffer,
			       size_t length, loff_t *ppos)
{
	return -EINVAL;
}

MODULE_PARM(polling,  "i");
MODULE_PARM_DESC(polling, "Polling rate in jiffies");
MODULE_PARM(ir_write, "i");
MODULE_PARM_DESC(ir_write, "write register of hauppage ir interface");
MODULE_PARM(ir_read,  "i");
MODULE_PARM_DESC(ir_read, "read register of hauppage ir interface");
MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "turn on debug output");

int init_module(void)
{
	int ret;

	remote_lock = SPIN_LOCK_UNLOCKED;

	/* BTTV module is needed to access the on-board I2C bus */

	/* ret = */ request_module("bttv");

	/* can anybody tell me why request_module() has a return value
	   while you can't use it!?!
	*/
	/* 
	if (ret <0) {
		printk ("lirc_haup: "
			"request for module bttv failed, "
			"code %d\n", ret);
		return ret;
	}
	*/
	     
	ret = register_chrdev(LIRC_MAJOR, DEVICE_NAME, &lirc_haup_fops);

	if (ret < 0) {
		printk ("lirc_haup: "
			"registration of device with major %d failed, "
			"code %d\n", LIRC_MAJOR, ret);
		return ret;
	}

	ret = i2c_register_driver(&lirc_haup_i2c_driver);

	if (ret < 0) {
		printk ("lirc_haup: "
			"registration of i2c driver failed, "
			"code %d\n", ret);
		return ret;
	}

	spin_lock(&remote_lock);
	
	remote.open = 0;

	lirc_haup_i2c_driver.addr_l = ir_read;
	lirc_haup_i2c_driver.addr_h = ir_read;

	spin_unlock(&remote_lock);
	
	return SUCCESS;
}

void cleanup_module(void)
{
	int ret;

	/* Unregister the device */
	ret = unregister_chrdev(LIRC_MAJOR, DEVICE_NAME);

	/* If there's an error, report it */ 
	if (ret < 0)
		printk("Error in unregister_chrdev: %d\n", ret);

	i2c_unregister_driver(&lirc_haup_i2c_driver);

	/* If there's an error, report it */ 
	if (ret < 0)
		printk("Error in i2c_unregister_driver: %d\n", ret);

}

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */

/*
 * Hauppauge remote control driver
 * Re-implementation for new I2C stuff in 2.3
 * this file is include from the old driver !
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/malloc.h>
#include <linux/poll.h>
#include <linux/kmod.h>
#include <linux/i2c.h>
#include "drivers/lirc.h"
#include "lirc_haup_new.h"

#define dprintk(x, y...)
#define DEVICE_NAME "hauppauge ir rc"

static unsigned short normal_i2c[] = {I2C_CLIENT_END};
static unsigned short normal_i2c_range[] = {0x31>>1,0x35>>1,I2C_CLIENT_END};
I2C_CLIENT_INSMOD;

static struct lirc_haup_status *remote_dev[256];
spinlock_t remote_dev_lock;

static int polling = POLLING;

struct lirc_haup_status
{
	spinlock_t lock;
	unsigned short buffer[BUFLEN];
	int head, tail;
	wait_queue_head_t wait_poll, wait_cleanup;
	struct i2c_client *c;
	unsigned char last_b1;
	unsigned int open : 1;
};
static struct timer_list lirc_haup_timer;

static void lirc_haup_do_timer (unsigned long data);
static int lirc_haup_open (struct inode *inode, struct file *file);
static ssize_t lirc_haup_close (struct inode *inode, struct file *file);
static ssize_t lirc_haup_read (struct file *file,
	char *buffer, size_t count, loff_t *ppos);
static int lirc_haup_write (struct file *file,
	const char *buffer, size_t count, loff_t *ppos);
static unsigned int lirc_haup_poll (struct file *file,
	struct poll_table_struct *wait);
static int lirc_haup_ioctl (struct inode *inode, struct file *file,
	unsigned int cmd, unsigned long arg);

static __u16 read_raw_keypress (struct lirc_haup_status *remote);

static struct file_operations lirc_haup_fops =
{
	NULL,
	lirc_haup_read,
	lirc_haup_write,
	NULL,
	lirc_haup_poll,
	lirc_haup_ioctl,
	NULL,
	lirc_haup_open,
	NULL,
	lirc_haup_close
};

/*** I2C functions ***/

static struct i2c_driver driver =
{
	"i2c IR remote control driver",
	I2C_DRIVERID_REMOTE,
	I2C_DF_NOTIFY,
	remote_probe,
	remote_detach,
	remote_command,
};

static struct i2c_client client_template =
{
	"(unset)",		/* name */
	-1,
	0,
	0,
	NULL,
	&driver
};

static int remote_attach (struct i2c_adapter *adap, int addr,
	unsigned short flags, int kind)
{
	struct i2c_client *client;
	struct lirc_haup_status *remote;
	int i;
	
	client_template.adapter = adap;
	client_template.addr = addr;
	
	printk ("remote: chip found @ 0x%x\n", addr);
	
	if (NULL == (client = kmalloc (sizeof (struct i2c_client), GFP_KERNEL)))
		return -ENOMEM;
	memcpy (client, &client_template, sizeof (struct i2c_client));
	client->data = remote = kmalloc (sizeof (struct lirc_haup_status), GFP_KERNEL);
	if (remote == NULL)
	{
		kfree (client);
		return -ENOMEM;
	}
	remote->lock = SPIN_LOCK_UNLOCKED;
	remote->c = client;
	remote->last_b1 = 0xff;
	remote->open = 0;
	remote->head = remote->tail = 0;
	
	spin_lock (&remote_dev_lock);
	for (i = 0; i < 256; i++)
		if (!remote_dev[i])
			break;
	if (i < 256)
		remote_dev[i] = remote;
	else
	{
		kfree (remote);
		kfree (client);
		return -ENOMEM;
	}
	spin_unlock (&remote_dev_lock);
	
	i2c_attach_client (client);
	
	return 0;
}

static int remote_probe (struct i2c_adapter *adap)
{
	if (adap->id == (I2C_ALGO_BIT | I2C_HW_B_BT848))
		return i2c_probe (adap, &addr_data, remote_attach);
	return 0;
}

static int remote_detach (struct i2c_client *client)
{
	struct lirc_haup_status *remote = (struct lirc_haup_status *)(client->data);
	int i;
	
	spin_lock (&remote->lock);
	remote->c = NULL;
	
	spin_lock (&remote_dev_lock);
	for (i = 0; i < 256; i++)
		if (remote_dev[i] == remote)
			remote_dev[i] = NULL;
	spin_unlock (&remote_dev_lock);
	
	i2c_detach_client (client);
	kfree (client);
	spin_unlock (&remote->lock);
	kfree (remote);
	return 0;
}

static int remote_command (struct i2c_client *client,
	unsigned int cmd, void *arg)
{
	return 0;
}

/*** Timer function ***/

static void lirc_haup_do_timer(unsigned long data)
{
	struct lirc_haup_status *remote = (struct lirc_haup_status *)(data);
	unsigned short code;

	if (remote == NULL)
		return;
	
	spin_lock(&remote->lock);
	
#if LINUX_VERSION_CODE < 0x020300
        if (remote->wait_cleanup != NULL) {
#else
	if (remote->wait_cleanup.task_list.next != 
	    &(remote->wait_cleanup.task_list)) {
#endif
		wake_up(&remote->wait_cleanup);
	        spin_unlock(&remote->lock);
		/* Now cleanup_module can return */
		return;
	}

	/* keep the remote->lock during the execution
	   of read_raw_keypress */
	code = read_raw_keypress(remote); /* get key from the remote */
	if( (code!=0xffff) ) {
		/* if buffer is full, drop input */
		if (((remote->tail+1)%BUFLEN)==remote->head) {
			dprintk("lirc_haup: input buffer overflow\n");
		} else {
			/* put new code to the buffer */
			remote->buffer[remote->tail++]=code;
			remote->tail%=BUFLEN;
			wake_up_interruptible(&remote->wait_poll);
		}
	}

 	lirc_haup_timer.expires = jiffies + polling;
	add_timer(&lirc_haup_timer);
	
	spin_unlock(&remote->lock);
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

static __u16 read_raw_keypress (struct lirc_haup_status *remote)
{
	struct i2c_client *c = remote->c;
	unsigned char b[3];
	__u16 repeat_bit;
	int i;
	
	i = i2c_master_recv (c, b, 3);
	if (b[0] == REPEAT_TOGGLE_0 || b[0] == REPEAT_TOGGLE_1)
	{
		//printk ("key %s (0x%02x/0x%02x)\n", keyname (b[1]), b[0], b[1]);
		if (b[0] == remote->last_b1)
			;// repeat
		else
			remote->last_b1 = b[0];
		repeat_bit = (b[0] & 0x20) ? 0x800 : 0;
		return (__u16) (0x1000 | repeat_bit | (b[1] >> 2));
	}
	else
		return (__u16) 0xffff;
}

/*** File functions ***/

static unsigned int lirc_haup_poll (struct file *file,
	struct poll_table_struct *wait)
{
	struct lirc_haup_status *remote =
		(struct lirc_haup_status *)file->private_data;
	
	if (remote == NULL)
		return SUCCESS;
	
	poll_wait (file, &remote->wait_poll, wait);
	
	spin_lock (&remote->lock);
	if (remote->head != remote->tail)
	{
		spin_unlock (&remote->lock);
		return POLLIN | POLLRDNORM;
	}
	else
	{
		spin_unlock (&remote->lock);
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
        struct lirc_haup_status *remote;
        
        remote = remote_dev[MINOR(inode->i_rdev)];
        if (remote == NULL)
        {
        	printk ("no device %d\n", MINOR(inode->i_dev));
        	return -ENODEV;
        }
        
        spin_lock(&remote->lock);
  
	/* We don't want to talk to two processes at the same time */
	if (remote->open) {
	        spin_unlock(&remote->lock);
		return -EBUSY;
	}
	
	file->private_data = remote;
	
	remote->open           = 1;
#if LINUX_VERSION_CODE < 0x020300
	remote->wait_poll      = NULL;
	remote->wait_cleanup   = NULL;
#else
	init_waitqueue_head(&remote->wait_poll);
	init_waitqueue_head(&remote->wait_cleanup);
#endif

	init_timer(&lirc_haup_timer);
	lirc_haup_timer.function = lirc_haup_do_timer;
	lirc_haup_timer.data     = (unsigned long)remote;
	lirc_haup_timer.expires  = jiffies + polling;
	add_timer(&lirc_haup_timer);

	/* initialize input buffer pointers */
	remote->head=remote->tail=0;
	
	spin_unlock(&remote->lock);

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
	struct lirc_haup_status *remote =
		(struct lirc_haup_status *)file->private_data;
	
	sleep_on(&remote->wait_cleanup);

        spin_lock(&remote->lock);
  
	remote->open = 0;

	/* flush the buffer */
	remote->head=remote->tail=0;
	
	del_timer(&lirc_haup_timer);

	spin_unlock(&remote->lock);
	
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
        struct lirc_haup_status *remote =
        	(struct lirc_haup_status *)file->private_data;

	if (remote == NULL)
	{
		printk ("read: remote NULL\n");
		return -ENODEV;
	}
	
	spin_lock(&remote->lock);
	
	if(length != 2) {
	        spin_unlock(&remote->lock);
		return -EIO; /* we are only prepared to send 2 bytes */
	}

	/* if input buffer is empty, wait for input */
	if (remote->head==remote->tail) {
		spin_unlock(&remote->lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		interruptible_sleep_on(&remote->wait_poll);
		if (signal_pending(current))
			return -ERESTARTSYS;
		spin_lock(&remote->lock);
	}
	
	data[0]=(unsigned char) ((remote->buffer[remote->head]>>8)&0x1f);
	data[1]=(unsigned char) (remote->buffer[remote->head++]&0xff);
	remote->head%=BUFLEN;
	
	if(copy_to_user(buffer,data,2)) {
	        spin_unlock(&remote->lock);
	        return(-EFAULT);
	}

	spin_unlock(&remote->lock);
	
	return length;
}


static ssize_t lirc_haup_write(struct file * file, const char * buffer,
			       size_t length, loff_t *ppos)
{
	return -EINVAL;
}

MODULE_PARM(polling,  "i");
MODULE_PARM_DESC(polling, "Polling rate in jiffies");

EXPORT_NO_SYMBOLS;

int init_module (void)
{
	request_module ("bttv");
	memset (remote_dev, 0, sizeof (remote_dev));
	remote_dev_lock = SPIN_LOCK_UNLOCKED;
	register_chrdev (LIRC_MAJOR, DEVICE_NAME, &lirc_haup_fops);
	i2c_add_driver (&driver);
	return SUCCESS;
}

void cleanup_module (void)
{
	i2c_del_driver (&driver);
	unregister_chrdev (LIRC_MAJOR, DEVICE_NAME);
}

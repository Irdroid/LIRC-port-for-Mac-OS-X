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

#include <linux/i2c.h>
#include "drivers/lirc.h"

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#define dprintk printk
#define DEVICE_NAME "remote"

/*
 * The polling rate (per second) of the i2c status.
 */
#define POLLING    20
#define IR_READ    0xC9
#define I2C_DRIVERID_REMOTE 4

struct i2c_remote_info
{
	struct i2c_bus   *bus;     /* where is our chip */
	int ckey;
	int addr;
};

struct remote_status {
	unsigned char last2, last1;
	unsigned char code;
	struct wait_queue *wait_poll, *wait_cleanup;
	int open;
	int status_changed;
	struct i2c_remote_info *i2c_remote;
};


static void remote_timer(void *unused);
static unsigned int remote_poll(struct file *file,
				struct poll_table_struct * wait);
static int remote_open(struct inode *inode, struct file *file);
static int remote_close(struct inode *inode, struct file *file);
static ssize_t remote_read(struct file * file, char * buffer,
			   size_t count, loff_t *ppos);
static ssize_t remote_write(struct file * file, const char * buffer,
			    size_t count, loff_t *ppos);
static int remote_attach(struct i2c_device *device);
static int remote_detach(struct i2c_device *device);
static int remote_command(struct i2c_device *device,
			  unsigned int cmd, void *arg);
static unsigned char readKey(void);


static struct tq_struct polling_task = {
	NULL,		/* Next item in list - queue_task will do this for us */
	0,		/* A flag meaning we haven't been inserted into a task queue yet */
	remote_timer,	/* The function to run */
	NULL		/* The void* parameter for that function */
};

static int lirc_ioctl(struct inode *node,struct file *filep,unsigned int cmd,
		      unsigned long arg)
{
        int result;
	unsigned long value;
	unsigned long features=LIRC_CAN_REC_CODE;
	
	switch(cmd)
	{
	case LIRC_GET_FEATURES:
		result=put_user(features,(unsigned long *) arg);
		if(result) return(result); 
		break;
        case LIRC_SET_REC_MODE:
                result=get_user(value,(unsigned long *) arg);
                if(result) return(result);
		if(value!=LIRC_MODE_CODE) return(-EINVAL);
		break;
	default:
		return(-ENOIOCTLCMD);
	}
	return(0);
}


struct file_operations fops = {
	NULL,   	/* seek    */
	remote_read, 	/* read    */
	remote_write,	/* write   */
	NULL,		/* readdir */
	remote_poll,	/* poll    */
	lirc_ioctl, 	/* ioctl   */
	NULL,		/* mmap    */
	remote_open,	/* open    */
	NULL,		/* flush   */
	remote_close    /* release */
};

/* ----------------------------------------------------------------------- */

struct i2c_driver i2c_driver_remote = 
{
	"remote",                      /* name       */
	I2C_DRIVERID_REMOTE,           /* ID         */
	IR_READ, IR_READ,             /* addr range */

	remote_attach,
	remote_detach,
	remote_command
};


static struct remote_status remote;
static int ticks = 0;


#if LINUX_VERSION_CODE < 0x02017f
void schedule_timeout(int j)
{
	current->state   = TASK_INTERRUPTIBLE;
	current->timeout = jiffies + j;
	schedule();
}
#endif

static void remote_timer(void *unused)
{
	unsigned char code, last1, last2;
	
	if (remote.wait_cleanup != NULL) 
	{
		wake_up(&remote.wait_cleanup);   /* Now cleanup_module can return */
		return;
	}
	else
		queue_task(&polling_task, &tq_timer);  /* Put ourselves back in the task queue */

	if(ticks < 200)
	{
		ticks = 0;
	}
	else
	{
		ticks++;
		return;	
	}

	code = readKey();
        last1 = remote.last1;
        last2 = remote.last2;

        remote.last2 = remote.last1;
        remote.last1 = code;

	if ( code != 0xff && last1 == 0xff )                   /* falling edge :-) */
	{
		remote.code = code;
                if (last2 != 0xff) remote.code |= 0x100;       /* repeat event */
		remote.status_changed = 1;
		wake_up_interruptible(&remote.wait_poll);
	}
}

static unsigned char readKey(void)
{
	unsigned char b1, b2, b3;
	struct i2c_remote_info *t = remote.i2c_remote;

        LOCK_FLAGS;
        
        LOCK_I2C_BUS(t->bus);
	//       	dprintk("Starting i2c Bus\n");
	i2c_start(t->bus);
	//	dprintk("Sending byte\n");
	i2c_sendbyte(t->bus, IR_READ, 0);
	//	dprintk("Return from sendbyte: %i\n", i);
	//	dprintk("Reading bytes...\n");
	b1 = i2c_readbyte(t->bus, 0);
		//dprintk("Return from ack 1: %i\n", b1);
	b2 = i2c_readbyte(t->bus, 0);
		//dprintk("Return from ack 2: %i\n", b2);
        b3 = i2c_readbyte(t->bus, 0);
		//dprintk("Return from ack 5: %i\n", b3);
	i2c_stop(t->bus);
        UNLOCK_I2C_BUS(t->bus);
       
        LOCK_I2C_BUS(t->bus);
        i2c_read(t->bus, IR_READ);
        UNLOCK_I2C_BUS(t->bus);
        LOCK_I2C_BUS(t->bus);
        i2c_read(t->bus, IR_READ);
        UNLOCK_I2C_BUS(t->bus);
        LOCK_I2C_BUS(t->bus);
        i2c_read(t->bus, IR_READ);
        UNLOCK_I2C_BUS(t->bus);
        LOCK_I2C_BUS(t->bus);
        i2c_read(t->bus, IR_READ);
        UNLOCK_I2C_BUS(t->bus);

	return (unsigned char)(b1);
}

/* ---------------------------------------------------------------------- */

static int remote_attach(struct i2c_device *device)
{
	struct i2c_remote_info *t;

	dprintk("Attaching remote device\n");

	if(device->bus->id!=I2C_BUSID_BT848)
		return -EINVAL;
		
	device->data = t = kmalloc(sizeof(struct i2c_remote_info),GFP_KERNEL);
	if (NULL == t)
		return -ENOMEM;
	memset(t,0,sizeof(struct i2c_remote_info));
	strcpy(device->name,"remote");
	t->bus  = device->bus;
	t->addr = device->addr;

	remote.i2c_remote = t;
/*	ready = 1;*/
	return 0;
}

static int remote_detach(struct i2c_device *device)
{
	struct remote *t = (struct remote*)device->data;
/*	ready = 0;*/
	kfree(t);
	return 0;
}

static int remote_command(struct i2c_device *device,
	      unsigned int cmd, void *arg)
{
	dprintk("remote: cmd = %i\n", cmd);
	return 0;
}



static unsigned int remote_poll(struct file *file, struct poll_table_struct * wait)
{
        poll_wait(file, &remote.wait_poll, wait);

	if(remote.status_changed)
	{
		remote.status_changed = 0;
		return POLLIN | POLLRDNORM;
	}
	else
		return 0;
}

static int remote_open(struct inode *inode, struct file *file)
{
	/* We don't want to talk to two processes at the same time */
	if (remote.open)
		return -EBUSY;
	
	remote.open = 1;
	remote.wait_poll = NULL;
	remote.wait_cleanup = NULL;
	remote.status_changed = 0;
	remote.last2 = remote.last1 = 0xFF;

	queue_task(&polling_task, &tq_timer);

	MOD_INC_USE_COUNT;

	return 0;
}

static int remote_close(struct inode *inode, struct file *file)
{
	sleep_on(&remote.wait_cleanup);

	remote.open = 0;

	MOD_DEC_USE_COUNT;
	
	return 0;
}

static ssize_t remote_read(struct file * file, char * buffer, size_t count, loff_t *ppos)
{
	int result;

	if(count != 1)
		return -EINVAL;
	
	result=put_user(remote.code, (unsigned char *) buffer);
	if(result) return(result);

	return count;
}


static ssize_t remote_write(struct file * file, const char * buffer, size_t count, loff_t *ppos)
{
        return -EINVAL;
}


int init_module(void)
{
	int ret;
	ret = register_chrdev(LIRC_MAJOR, DEVICE_NAME, &fops);

	if (ret < 0)
	{
		printk ("remote: registration of device with major %d failed, code %d\n", LIRC_MAJOR, ret);
		return ret;
	}
	i2c_register_driver(&i2c_driver_remote);

	remote.open = 0;
	
	return 0;
}

void cleanup_module(void)
{
	int ret;

	/* Unregister the device */
	ret = unregister_chrdev(LIRC_MAJOR, DEVICE_NAME);

	i2c_unregister_driver(&i2c_driver_remote);

	/* If there's an error, report it */ 
	if (ret < 0)
		printk("Error in module_unregister_chrdev: %d\n", ret);
}

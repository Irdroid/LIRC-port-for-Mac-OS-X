/*
 * LIRC base driver
 * 
 * (L) by Artur Lipowski <alipowski@interia.pl>
 *
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
 *
 * $Id: lirc_dev.c,v 1.25 2003/05/04 09:31:28 ranty Exp $
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
 
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,0)
#define LIRC_HAVE_DEVFS
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 2, 18)
#error "**********************************************************"
#error " Sorry, this driver needs kernel version 2.2.18 or higher "
#error "**********************************************************"
#endif

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/poll.h>
#ifdef LIRC_HAVE_DEVFS
#include <linux/devfs_fs_kernel.h>
#endif
#include <linux/smp_lock.h>
#include <asm/uaccess.h>
#include <asm/semaphore.h>
#include <asm/errno.h>
#include <linux/wrapper.h>
#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>

#include "drivers/lirc.h"

#include "lirc_dev.h"

static int debug = 0;

MODULE_PARM(debug,"i");

#define IRCTL_DEV_NAME    "BaseRemoteCtl"
#define SUCCESS           0
#define NOPLUG            -1
#define dprintk           if (debug) printk

#define LOGHEAD           "lirc_dev (%s[%d]): "

struct irctl
{
	struct lirc_plugin p;
	int open;

	struct lirc_buffer *buf;

	int tpid;
	struct semaphore *t_notify;
	struct semaphore *t_notify2;
	int shutdown;
	long jiffies_to_wait;

#ifdef LIRC_HAVE_DEVFS
	devfs_handle_t devfs_handle;
#endif
};

DECLARE_MUTEX(plugin_lock);

static struct irctl irctls[MAX_IRCTL_DEVICES];
static struct file_operations fops;


/*  helper function
 *  initializes the irctl structure
 */
static inline void init_irctl(struct irctl *ir)
{
	memset(&ir->p, 0, sizeof(struct lirc_plugin));
	ir->p.minor = NOPLUG;

	ir->tpid = -1;
	ir->t_notify = NULL;
	ir->t_notify2 = NULL;
	ir->shutdown = 0;
	ir->jiffies_to_wait = 0;

	ir->open = 0;
}


/*  helper function
 *  reads key codes from plugin and puts them into buffer
 *  buffer free space is checked and locking performed
 *  returns 0 on success
 */

inline static int add_to_buf(struct irctl *ir)
{
	unsigned char buf[BUFLEN];
	unsigned int i;

	if (lirc_buffer_full(ir->buf)) {
		dprintk(LOGHEAD "buffer overflow\n",
			ir->p.name, ir->p.minor);
		return -EOVERFLOW;
	}

	for (i=0; i < ir->buf->chunk_size; i++) {
		if (ir->p.get_key(ir->p.data, &buf[i], i)) {
			return -ENODATA;
		}
		dprintk(LOGHEAD "remote code (0x%x) now in buffer\n",
			ir->p.name, ir->p.minor, buf[i]);
	}

	/* here is the only point at which we add key codes to the buffer */
	lirc_buffer_write_1(ir->buf, buf);

	return SUCCESS;
}

/* main function of the polling thread
 */
static int lirc_thread(void *irctl)
{
	struct irctl *ir = irctl;
	
	lock_kernel();
	
	/* This thread doesn't need any user-level access,
	 * so get rid of all our resources
	 */
	exit_mm(current);
	exit_files(current);
	exit_fs(current);
	current->session = 1;
	current->pgrp = 1;
	current->euid = 0;
	current->tty = NULL;
	sigfillset(&current->blocked);
	
	strcpy(current->comm, "lirc_dev");
	
	unlock_kernel();
	
	if (ir->t_notify != NULL) {
		up(ir->t_notify);
	}
	
	dprintk(LOGHEAD "poll thread started\n", ir->p.name, ir->p.minor);
	
	do {
		if (ir->open) {
			if (ir->jiffies_to_wait) {
				current->state = TASK_INTERRUPTIBLE;
				schedule_timeout(ir->jiffies_to_wait);
			} else {
				interruptible_sleep_on(ir->p.get_queue(ir->p.data));
			}
			if (ir->shutdown) {
				break;
			}
			if (!add_to_buf(ir)) {
				wake_up_interruptible(&ir->buf->wait_poll);
			}
		} else {
			/* if device not opened so we can sleep half a second */
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(HZ/2);
		}
	} while (!ir->shutdown);
	
	if (ir->t_notify2 != NULL) {
		down(ir->t_notify2);
	}

	ir->tpid = -1;
	if (ir->t_notify != NULL) {
		up(ir->t_notify);
	}
	
	dprintk(LOGHEAD "poll thread ended\n", ir->p.name, ir->p.minor);
	
	return 0;
}

/*
 *
 */
int lirc_register_plugin(struct lirc_plugin *p)
{
	struct irctl *ir;
	int minor;
	int bytes_in_key;
#ifdef LIRC_HAVE_DEVFS
	char name[16];
#endif
	DECLARE_MUTEX_LOCKED(tn);

	if (!p) {
		printk("lirc_dev: lirc_register_plugin:"
		       "plugin pointer must be not NULL!\n");
		return -EBADRQC;
	}

	if (MAX_IRCTL_DEVICES <= p->minor) {
		printk("lirc_dev: lirc_register_plugin:"
		       "\" minor\" must be beetween 0 and %d (%d)!\n",
		       MAX_IRCTL_DEVICES-1, p->minor);
		return -EBADRQC;
	}

	if (1 > p->code_length || (BUFLEN*8) < p->code_length) {
		printk("lirc_dev: lirc_register_plugin:"
		       "code length in bits for minor (%d) "
		       "must be less than %d!\n",
		       p->minor, BUFLEN*8);
		return -EBADRQC;
	}

	printk("lirc_dev: lirc_register_plugin:"
	       "sample_rate: %d\n",p->sample_rate);
	if (p->sample_rate) {
		if (2 > p->sample_rate || 50 < p->sample_rate) {
			printk("lirc_dev: lirc_register_plugin:"
			       "sample_rate must be beetween 2 and 50!\n");
			return -EBADRQC;
		}
	} else if (!(p->fops && p->fops->read)
			&& !p->get_queue && !p->rbuf) {
		printk("lirc_dev: lirc_register_plugin:"
		       "fops->read, get_queue and rbuf cannot all be NULL!\n");
		return -EBADRQC;
	} else if (!p->rbuf) {
		if (!(p->fops && p->fops->read && p->fops->poll) 
				|| (!p->fops->ioctl && !p->ioctl)) {
			printk("lirc_dev: lirc_register_plugin:"
			       "neither read, poll nor ioctl can be NULL!\n");
			return -EBADRQC;
		}
	}

	down_interruptible(&plugin_lock);

	minor = p->minor;

	if (0 > minor) {
		/* find first free slot for plugin */
		for (minor=0; minor<MAX_IRCTL_DEVICES; minor++)
			if (irctls[minor].p.minor == NOPLUG)
				break;
		if (MAX_IRCTL_DEVICES == minor) {
			printk("lirc_dev: lirc_register_plugin: "
			       "no free slots for plugins!\n");
			up(&plugin_lock);
			return -ENOMEM;
		}
	} else if (irctls[minor].p.minor != NOPLUG) {
		printk("lirc_dev: lirc_register_plugin:"
		       "minor (%d) just registerd!\n", minor);
		up(&plugin_lock);
		return -EBUSY;
	}

	ir = &irctls[minor];

	if (p->sample_rate) {
		ir->jiffies_to_wait = HZ / p->sample_rate;
	} else {
                /* it means - wait for externeal event in task queue */
		ir->jiffies_to_wait = 0;
	} 

	/* some safety check 8-) */
	p->name[sizeof(p->name)-1] = '\0';

	bytes_in_key = p->code_length/8 + (p->code_length%8 ? 1 : 0);
	
	if (p->rbuf) {
		ir->buf = p->rbuf;
	} else {
		ir->buf = kmalloc(sizeof(struct lirc_buffer), GFP_KERNEL);
		lirc_buffer_init(ir->buf, bytes_in_key, BUFLEN/bytes_in_key);
	}

	if (p->features==0)
		p->features = (p->code_length > 8) ?
			LIRC_CAN_REC_LIRCCODE : LIRC_CAN_REC_CODE;

	ir->p = *p;
	ir->p.minor = minor;

#ifdef LIRC_HAVE_DEVFS
	sprintf (name, DEV_LIRC "/%d", ir->p.minor);
	ir->devfs_handle = devfs_register(NULL, name, DEVFS_FL_DEFAULT,
					  IRCTL_DEV_MAJOR, ir->p.minor,
					  S_IFCHR | S_IRUSR | S_IWUSR,
					  &fops, NULL);
#endif

	if(p->sample_rate || p->get_queue) {
		/* try to fire up polling thread */
		ir->t_notify = &tn;
		ir->tpid = kernel_thread(lirc_thread, (void*)ir, 0);
		if (ir->tpid < 0) {
			up(&plugin_lock);
			printk("lirc_dev: lirc_register_plugin:"
			       "cannot run poll thread for minor = %d\n",
			       p->minor);
			return -ECHILD;
		}
		down(&tn);
		ir->t_notify = NULL;
	}
	up(&plugin_lock);

	MOD_INC_USE_COUNT;

	dprintk("lirc_dev: plugin %s registered at minor number = %d\n",
		ir->p.name, ir->p.minor);

	return minor;
}

/*
 *
 */
int lirc_unregister_plugin(int minor)
{
	struct irctl *ir;
	DECLARE_MUTEX_LOCKED(tn);
	DECLARE_MUTEX_LOCKED(tn2);

	if (minor < 0 || minor >= MAX_IRCTL_DEVICES) {
		printk("lirc_dev: lirc_unregister_plugin:"
		       "\" minor\" must be beetween 0 and %d!\n",
		       MAX_IRCTL_DEVICES-1);
		return -EBADRQC;
	}

	ir = &irctls[minor];

	down_interruptible(&plugin_lock);

	if (ir->p.minor != minor) {
		printk("lirc_dev: lirc_unregister_plugin:"
		       "minor (%d) device not registered!", minor);
		up(&plugin_lock);
		return -ENOENT;
	}

	if (ir->open) {
		printk("lirc_dev: lirc_unregister_plugin:"
		       "plugin %s[%d] in use!", ir->p.name, ir->p.minor);
		up(&plugin_lock);
		return -EBUSY;
	}

	/* end up polling thread */
	if (ir->tpid >= 0) {
		ir->t_notify = &tn;
		ir->t_notify2 = &tn2;
		ir->shutdown = 1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,0)
		{
			struct task_struct *p;
			
			p = find_task_by_pid(ir->tpid);
			wake_up_process(p);
		}
#else
		/* 2.2.x does not export wake_up_process() */
		wake_up_interruptible(ir->p.get_queue(ir->p.data));
#endif
		up(&tn2);
		down(&tn);
		ir->t_notify = NULL;
		ir->t_notify2 = NULL;
	}

	dprintk("lirc_dev: plugin %s unregistered from minor number = %d\n",
		ir->p.name, ir->p.minor);

#ifdef LIRC_HAVE_DEVFS
	devfs_unregister(ir->devfs_handle);
#endif
	if (ir->buf != ir->p.rbuf){
		lirc_buffer_free(ir->buf);
		kfree(ir->buf);
	}
	ir->buf = NULL;
	init_irctl(ir);
	up(&plugin_lock);

	MOD_DEC_USE_COUNT;

	return SUCCESS;
}

/*
 *
 */
static int irctl_open(struct inode *inode, struct file *file)
{
	struct irctl *ir;
	int retval;
	
	if (MINOR(inode->i_rdev) >= MAX_IRCTL_DEVICES) {
		dprintk("lirc_dev [%d]: open result = -ENODEV\n",
			MINOR(inode->i_rdev));
		return -ENODEV;
	}

	ir = &irctls[MINOR(inode->i_rdev)];

	dprintk(LOGHEAD "open called\n", ir->p.name, ir->p.minor);

	/* if the plugin has an open function use it instead */
	if(ir->p.fops && ir->p.fops->open)
		return ir->p.fops->open(inode, file);

	down_interruptible(&plugin_lock);

	if (ir->p.minor == NOPLUG) {
		up(&plugin_lock);
		dprintk(LOGHEAD "open result = -ENODEV\n",
			ir->p.name, ir->p.minor);
		return -ENODEV;
	}

	if (ir->open) {
		up(&plugin_lock);
		dprintk(LOGHEAD "open result = -EBUSY\n",
			ir->p.name, ir->p.minor);
		return -EBUSY;
	}

	/* there is no need for locking here because ir->open is 0 
         * and lirc_thread isn't using buffer
	 * plugins which use irq's should allocate them on set_use_inc,
	 * so there should be no problem with those either.
         */
	ir->buf->head = ir->buf->tail;
	ir->buf->fill = 0;

	++ir->open;
	retval = ir->p.set_use_inc(ir->p.data);

	up(&plugin_lock);

	if (retval != SUCCESS) {
		--ir->open;
		return retval;
	}

	dprintk(LOGHEAD "open result = %d\n", ir->p.name, ir->p.minor, SUCCESS);

	return SUCCESS;
}

/*
 *
 */
static int irctl_close(struct inode *inode, struct file *file)
{
	struct irctl *ir = &irctls[MINOR(inode->i_rdev)];

	dprintk(LOGHEAD "close called\n", ir->p.name, ir->p.minor);
 
	/* if the plugin has a close function use it instead */
	if(ir->p.fops && ir->p.fops->release)
		return ir->p.fops->release(inode, file);

	down_interruptible(&plugin_lock);

	--ir->open;
	ir->p.set_use_dec(ir->p.data);

	up(&plugin_lock);

	return SUCCESS;
}

/*
 *
 */
static unsigned int irctl_poll(struct file *file, poll_table *wait)
{
	struct irctl *ir = &irctls[MINOR(file->f_dentry->d_inode->i_rdev)];

	dprintk(LOGHEAD "poll called\n", ir->p.name, ir->p.minor);

	/* if the plugin has a poll function use it instead */
	if(ir->p.fops && ir->p.fops->poll)
		return ir->p.fops->poll(file, wait);

	poll_wait(file, &ir->buf->wait_poll, wait);

	dprintk(LOGHEAD "poll result = %s\n",
		ir->p.name, ir->p.minor, 
		lirc_buffer_empty(ir->buf) ? "0" : "POLLIN|POLLRDNORM");

	return lirc_buffer_empty(ir->buf) ? 0 : (POLLIN|POLLRDNORM);
}

/*
 *
 */
static int irctl_ioctl(struct inode *inode, struct file *file,
                       unsigned int cmd, unsigned long arg)
{
	unsigned long mode;
	int result;
	struct irctl *ir = &irctls[MINOR(inode->i_rdev)];

	dprintk(LOGHEAD "poll called (%u)\n",
		ir->p.name, ir->p.minor, cmd);

	/* if the plugin has a ioctl function use it instead */
	if(ir->p.fops && ir->p.fops->ioctl)
		return ir->p.fops->ioctl(inode, file, cmd, arg);

	if (ir->p.minor == NOPLUG) {
		dprintk(LOGHEAD "ioctl result = -ENODEV\n",
			ir->p.name, ir->p.minor);
		return -ENODEV;
	}

	/* Give the plugin a chance to handle the ioctl */
	if(ir->p.ioctl){
		result = ir->p.ioctl(inode, file, cmd, arg);
		if (result != -ENOIOCTLCMD)
			return result;
	}
	/* The plugin can't handle cmd */
	result = SUCCESS;

	switch(cmd)
	{
	case LIRC_GET_FEATURES:
		result = put_user(ir->p.features, (unsigned long*)arg);
		break;
	case LIRC_GET_REC_MODE:
		if(!(ir->p.features&LIRC_CAN_REC_MASK))
			return(-ENOSYS);
		
		result = put_user(LIRC_REC2MODE
				  (ir->p.features&LIRC_CAN_REC_MASK),
				  (unsigned long*)arg);
		break;
	case LIRC_SET_REC_MODE:
		if(!(ir->p.features&LIRC_CAN_REC_MASK))
			return(-ENOSYS);

		result = get_user(mode, (unsigned long*)arg);
		if(!result && !(LIRC_MODE2REC(mode) & ir->p.features)) {
			result = -EINVAL;
		}
		/* FIXME: We should actually set the mode somehow 
		 * but for now, lirc_serial doesn't support mode changin
		 * eighter */
		break;
	case LIRC_GET_LENGTH:
		result = put_user((unsigned long)ir->p.code_length, 
				  (unsigned long *)arg);
		break;
	default:
		result = -ENOIOCTLCMD;
	}

	dprintk(LOGHEAD "ioctl result = %d\n",
		ir->p.name, ir->p.minor, result);

	return result;
}

/*
 *
 */
static ssize_t irctl_read(struct file *file,
			  char *buffer,   
			  size_t length, 
			  loff_t *ppos)     
{
	struct irctl *ir = &irctls[MINOR(file->f_dentry->d_inode->i_rdev)];
	unsigned char buf[ir->buf->chunk_size];
	int ret=0, written=0;
	DECLARE_WAITQUEUE(wait, current);

	dprintk(LOGHEAD "read called\n", ir->p.name, ir->p.minor);

	/* if the plugin has a specific read function use it instead */
	if(ir->p.fops && ir->p.fops->read)
		return ir->p.fops->read(file, buffer, length, ppos);

	if (length % ir->buf->chunk_size) {
		dprintk(LOGHEAD "read result = -EINVAL\n",
			ir->p.name, ir->p.minor);
		return -EINVAL;
	}

	/* we add ourselves to the task queue before buffer check 
         * to avoid losing scan code (in case when queue is awaken somewhere 
	 * beetwen while condition checking and scheduling)
	 */
	add_wait_queue(&ir->buf->wait_poll, &wait);
	current->state = TASK_INTERRUPTIBLE;

	/* while we did't provide 'length' bytes, device is opened in blocking
	 * mode and 'copy_to_user' is happy, wait for data.
	 */
	while (written < length && ret == 0) { 
		if (lirc_buffer_empty(ir->buf)) {
			/* According to the read(2) man page, 'written' can be
			 * returned as less than 'length', instead of blocking
			 * again, returning -EWOULDBLOCK, or returning
			 * -ERESTARTSYS */
			if (written) break;
			if (file->f_flags & O_NONBLOCK) {
				dprintk(LOGHEAD "read result = -EWOULDBLOCK\n", 
						ir->p.name, ir->p.minor);
				remove_wait_queue(&ir->buf->wait_poll, &wait);
				current->state = TASK_RUNNING;
				return -EWOULDBLOCK;
			}
			if (signal_pending(current)) {
				dprintk(LOGHEAD "read result = -ERESTARTSYS\n", 
						ir->p.name, ir->p.minor);
				remove_wait_queue(&ir->buf->wait_poll, &wait);
				current->state = TASK_RUNNING;
				return -ERESTARTSYS;
			}
			schedule();
			current->state = TASK_INTERRUPTIBLE;
		} else {
			lirc_buffer_read_1(ir->buf, buf);
			ret = copy_to_user((void *)buffer+written, buf,
					   ir->buf->chunk_size);
			written += ir->buf->chunk_size;
		}
	}

	remove_wait_queue(&ir->buf->wait_poll, &wait);
	current->state = TASK_RUNNING;

	dprintk(LOGHEAD "read result = %s (%d)\n",
		ir->p.name, ir->p.minor, ret ? "-EFAULT" : "OK", ret);

	return ret ? -EFAULT : written;
}

static ssize_t irctl_write(struct file *file, const char *buffer,
			   size_t length, loff_t * ppos)
{
	struct irctl *ir = &irctls[MINOR(file->f_dentry->d_inode->i_rdev)];

	dprintk(LOGHEAD "read called\n", ir->p.name, ir->p.minor);

	/* if the plugin has a specific read function use it instead */
	if(ir->p.fops && ir->p.fops->write)
		return ir->p.fops->write(file, buffer, length, ppos);

	return -EINVAL;
}


static struct file_operations fops = {
	read:    irctl_read, 
	write:   irctl_write,
	poll:    irctl_poll,
	ioctl:   irctl_ioctl,
	open:    irctl_open,
	release: irctl_close
};



EXPORT_SYMBOL(lirc_register_plugin);
EXPORT_SYMBOL(lirc_unregister_plugin);

/*
 *
 */
int lirc_dev_init(void)
{  	
	int i;

	for (i=0; i < MAX_IRCTL_DEVICES; ++i) {
		init_irctl(&irctls[i]);	
	}

#ifndef LIRC_HAVE_DEVFS
 	i = register_chrdev(IRCTL_DEV_MAJOR,
#else
	i = devfs_register_chrdev(IRCTL_DEV_MAJOR,
#endif
				   IRCTL_DEV_NAME,
				   &fops);
	
	if (i < 0) {
		printk ("lirc_dev: device registration failed with %d\n", i);
		return i;
	}
	
	printk("lirc_dev: IR Remote Control driver registered, at major %d \n", 
	       IRCTL_DEV_MAJOR);

	return SUCCESS;
}

/* ---------------------------------------------------------------------- */

/* For now dont try to use it as a static version !  */

#ifdef MODULE

MODULE_DESCRIPTION("LIRC base driver module");
MODULE_AUTHOR("Artur Lipowski");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

/*
 *
 */
int init_module(void)
{
	return lirc_dev_init();
}

/*
 *
 */
void cleanup_module(void)
{
	int ret;
	
#ifndef LIRC_HAVE_DEVFS
 	ret = unregister_chrdev(IRCTL_DEV_MAJOR, IRCTL_DEV_NAME);
#else
	ret = devfs_unregister_chrdev(IRCTL_DEV_MAJOR, IRCTL_DEV_NAME);
#endif
 
	if (0 > ret){
		printk("lirc_dev: error in module_unregister_chrdev: %d\n",
		       ret);
	} else {
		dprintk("lirc_dev: module successfully unloaded\n");
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

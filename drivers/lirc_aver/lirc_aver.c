/* $Id: lirc_aver.c,v 1.2 1999/06/08 12:01:42 denis Exp $ */
/* $Log: lirc_aver.c,v $
/* Revision 1.2  1999/06/08 12:01:42  denis
/* Compilation with new lirc fixed.
/* New lirc ioctl interface added.
/* Main lirc config.h file added.
/* "major" module option added.
/* Repeat code with "repeat_bit" implemented.
/* */

/* 
    remote_aver.o - Remote control driver for avermedia remotes and lirc

    Copyright (C) 1999  Ryan Gammon (rggammon@engmail.uwaterloo.ca)
    Changes by Denis V. Dmitrienko <denis@null.net>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
This from avermedia. In actual fact, it seems to all be a lie. 
Or maybe I'm just confused...

The following is the define of our remote function for the GPIO pins:

Remote code(8 bits):
	GPIO (MSB)23 22 21 20 19 17 16 15(LSB)
Handshaking:(3 bits):
	GPIO 14:	0	Get the code of remote
			1	Read the code of remote
	GPIO 23:	0	The code of remote not ready
			1	The code of remote is ready
	GPIO 22:	0
			1	Repeat

Algorithm:
	If (bit 23 is 1)
		Set (bit 14 to 1)
		Get remote code(8 bits)
		Set (bit 14 to 0)
	Endif
*/

#include <linux/version.h>

#if LINUX_VERSION_CODE < 0x020200
/* We need the latest bttv.c in the kernel */
#error "--- A 2.2.x kernel is required to use this module ---"
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <linux/module.h>

#ifndef MODULE
#error "This driver can be used only as kernel module"
#endif

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/fs.h>

#include "../lirc.h"

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE !FALSE
#endif

/* In linux/drivers/char/bttv.c */
extern int read_bt848_gpio(int card, int latch, unsigned long mask, unsigned long *val);
extern int write_bt848_gpio(int card, int latch, unsigned long mask, unsigned long val);

static void remote_timer(void *unused);
static unsigned int remote_poll(struct file *file, struct poll_table_struct * wait);
static int remote_open(struct inode *inode, struct file *file);
static int remote_close(struct inode *inode, struct file *file);
static int remote_ioctl(struct inode *, struct file *, unsigned int cmd, unsigned long arg);
static ssize_t remote_read(struct file * file, char * buffer, size_t count, loff_t *ppos);
static ssize_t remote_write(struct file * file, const char * buffer, size_t count, loff_t *ppos);

MODULE_PARM(card, "1-4i"); /* Which avermedia card to use, if you have more than 1 */
MODULE_PARM(major, "i");
EXPORT_NO_SYMBOLS;

static int card = 1;
static int major = LIRC_MAJOR;

struct remote_status {
  int is_open;
  int mode_decoded;
	unsigned char code;
	struct wait_queue *wait_poll, *wait_cleanup;
	int status_changed;
  char repeat;
};

static struct tq_struct polling_task = {
	NULL,		/* Next item in list - queue_task will do this for us */
	0,		/* A flag meaning we haven't been inserted into a task queue yet */
	remote_timer,	/* The function to run */
	NULL		/* The void* parameter for that function */
};

struct file_operations fops = {
	NULL,   	/* seek    */
	remote_read, 	/* read    */
	remote_write,	/* write   */
	NULL,		/* readdir */
	remote_poll,	/* poll    */
	remote_ioctl,	/* ioctl   */
	NULL,		/* mmap    */
	remote_open,	/* open    */
	NULL,		/* flush   */
	remote_close    /* release */
};

static struct remote_status remote;
static int ticks = 0;

static void remote_timer(void *unused)
{
	unsigned char code;	
	unsigned long gpio, ready;
	int result;
	
	if(remote.mode_decoded)
	{
		/* Put ourselves back in the task queue */
		queue_task(&polling_task, &tq_timer); 
	}
	else
	{
		if (remote.wait_cleanup != NULL)
		{
			/* Now cleanup_module can return */
			wake_up(&remote.wait_cleanup);   
		}
		return;
	}

	if(ticks < 10)
	{
		ticks++;
		return;
	}
	
	ticks = 0;

	result = read_bt848_gpio(card-1, FALSE, 0x00010000, &ready);

	if((result != 0) || (!ready))
	{
    if(remote.code != 0xFF)
      remote.repeat = !remote.repeat;
		remote.code = 0xFF;
		return;
	}
	
	if(write_bt848_gpio(card-1, FALSE, 0x00020000, 0x00020000) != 0) return;
	if(read_bt848_gpio (card-1, FALSE, 0x00FC0000, &gpio)      != 0) return;
	if(write_bt848_gpio(card-1, FALSE, 0x00020000, 0x00000000) != 0) return;

	gpio >>= 16;
	code = (gpio & 0xF0) >> 4;
	if(gpio & 0x08)	code += 0x10;

  remote.code = code;
  remote.status_changed = 1;
  wake_up_interruptible(&remote.wait_poll);
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

static int remote_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
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
        if(remote.mode_decoded)
        {
          remote.mode_decoded = FALSE;
          sleep_on(&remote.wait_cleanup);
        }
        return -ENOSYS;
      }
      else
      {
        if(!remote.mode_decoded)
        {
          remote.mode_decoded = TRUE;
          queue_task(&polling_task, &tq_timer);
        }
      }
      break;
    default:
      return -ENOIOCTLCMD;
	}
  return 0;
}

static int remote_open(struct inode *inode, struct file *file)
{
	/* We don't want to talk to two processes at the same time */
	if (remote.is_open)
		return -EBUSY;
	
	remote.is_open = TRUE;
	remote.wait_poll = NULL;
	remote.wait_cleanup = NULL;
	remote.status_changed = FALSE;
	remote.code = 0xFF;
  remote.repeat = 0;

	MOD_INC_USE_COUNT;

	return 0;
}

static int remote_close(struct inode *inode, struct file *file)
{
	if(remote.mode_decoded)
	{
		remote.mode_decoded = FALSE;
		sleep_on(&remote.wait_cleanup);
	}

	remote.is_open = FALSE;

	MOD_DEC_USE_COUNT;
	
	return 0;
}

static ssize_t remote_read(struct file * file, char * buffer, size_t count, loff_t *ppos)
{
	unsigned char lirc_data;

	if(count != 1)		/* LIRC_MODE_CODE */
		return -EIO;
	
	lirc_data = (unsigned char)(remote.code | (remote.repeat << 7));
	put_user(lirc_data, (unsigned char*)(buffer));

	return count;
}

static ssize_t remote_write(struct file * file, const char * buffer, size_t count, loff_t *ppos)
{
  return -EINVAL;
}

int init_module(void)
{
	int ret;
	
	ret = register_chrdev(major, LIRC_DRIVER, &fops);

	if (ret < 0)
	{
		printk("remote: registration of device with major %d failed, code %d\n", major, ret);
		return ret;
	}
  remote.mode_decoded = FALSE;
  remote.is_open = FALSE;

	return 0;
}

void cleanup_module(void)
{
	int ret;

	/* Unregister the device */
	ret = unregister_chrdev(major, LIRC_DRIVER);

	/* If there's an error, report it */ 
	if (ret < 0)
		printk("Error in module_unregister_chrdev: %d\n", ret);
}

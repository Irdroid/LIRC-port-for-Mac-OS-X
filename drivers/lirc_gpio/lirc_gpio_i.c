/*
 * Remote control driver for the TV-card
 * key codes are obtained from GPIO port after interrupt 
 * notification from bttv module
 * 
 * (L) by Artur Lipowski <lipowski@comarch.pl>
 *        This code is licensed under GNU GPL
 *
 * $Id: lirc_gpio_i.c,v 1.1 2000/04/02 13:07:21 columbus Exp $
 *
 */

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 2, 4)
#error "!!! Sorry, this driver needs kernel version 2.2.4 or higher !!!"
#endif

#include <linux/module.h>
#include <linux/wrapper.h>
#include <linux/kmod.h>

#include "../lirc_dev/lirc_dev.h"
/* i2c.h (2.2.14) does not include spinlock.h */
#include <asm/spinlock.h>
#include "../drivers/char/bttv.h"
#include "../drivers/char/bt848.h"

/* default parameters values are suitable for the PixelView Play TVPro card */

static int debug = 0;
static int card = 0;
static int minor = 0;
static int code_length = 5;
static unsigned int gpio_mask = 0x1f;
static int gpio_shift = 8;
static unsigned int gpio_lock_mask = 0x8000;
static int soft_gap = 300; /* ms  0 - disable */

MODULE_PARM(debug,"i");
MODULE_PARM(card,"i");
MODULE_PARM(minor,"i");
MODULE_PARM(code_length,"i");
MODULE_PARM(gpio_mask,"i");
MODULE_PARM(gpio_shift,"i");
MODULE_PARM(gpio_lock_mask,"i");
MODULE_PARM(soft_gap,"i");

#define dprintk  if (debug) printk


static int get_key(void* data, unsigned char* key, int key_no)
{
	unsigned long code;
	static unsigned long next_time = 0;

	if (soft_gap && jiffies < next_time) {
		return -1;
	} else {
		next_time = jiffies + soft_gap;
	}

	if (bttv_read_gpio(card, &code)) {
		dprintk("lirc_gpio_p %d: cannot read GPIO\n", card);
		return -1;
	}

	if (gpio_lock_mask && (code & gpio_lock_mask)) {
		return -1;
	}

	*key = (code >> gpio_shift) & gpio_mask;
	dprintk("lirc_gpio_p %d: new key code:  0x%x\n", 
		card, (unsigned int)*key);

	return 0;
}

static WAITQ* get_queue(void* data)
{
	return bttv_get_gpio_queue(card);
}

static void set_use_inc(void* data)
{
	MOD_INC_USE_COUNT;
}

static void set_use_dec(void* data)
{
	MOD_DEC_USE_COUNT;
}

static struct lirc_plugin plugin = {
	"lirc_gpio_i",
	0,
	0,
	0,
	NULL,
	get_key,
	get_queue,
	set_use_inc,
	set_use_dec
};

/*
 *
 */
int gpio_remote_init(void)
{  	
	int ret;
	
	plugin.minor = minor;
	plugin.code_length = code_length;

	ret = lirc_register_plugin(&plugin);
	
	if (0 > ret) {
		printk ("lirc_gpio_i %d: device registration failed with %d\n",
			minor, ret);
		return ret;
	}
	
	printk("lirc_gpio_i %d: driver registered\n", minor);

	return 0;
}

EXPORT_NO_SYMBOLS; 

#ifdef MODULE
MODULE_DESCRIPTION("Driver module for remote control (data from bt848 GPIO port)");
MODULE_AUTHOR("Artur Lipowski");

/*
 *
 */
int init_module(void)
{
	unsigned int max_mask;

	if (0 > minor || MAX_IRCTL_DEVICES < minor) {
		printk("lirc_gpio_i: parameter minor (%d) must be beetween 0 and %d!\n",
		       minor, MAX_IRCTL_DEVICES);
		return -1;
	}
	
	if (0 > bttv_get_id(card)) {
		printk("lirc_gpio_i %d: parameter card (%d) has bad value!\n",
		       minor, card);
		return -1;
	}

	if (1 > code_length || 16 < code_length) {
		printk("lirc_gpio_i %d: parameter code_length (%d) has bad value!\n",
		       minor, code_length);
		return -1;
	}

	max_mask = code_length > 8 ? 0xffff : 0xff;
	if (gpio_mask) {
		if (gpio_mask > max_mask) {
		printk("lirc_gpio_i %d: parameter gpio_mask (%d) must be non zero "
		       "and not greather than %u!\n", minor, gpio_mask, max_mask);
		return -1;
		}
	} else {
		gpio_mask = max_mask;
	}

	if (19 < (unsigned)gpio_shift ) {
		printk("lirc_gpio_i %d: parameter gpio_shift must be less than 20!\n",
		       minor);
		return -1;
	}

	if (soft_gap && (100 > soft_gap || 1000 < soft_gap)) {
		printk("lirc_gpio_p %d: parameter soft_gap must be beetween 100 and 1000!\n",
                       minor);
		return -1;
	}
	/* translate ms to jiffies */
	soft_gap = (soft_gap*HZ) / 1000;

	request_module("bttv");
	request_module("lirc_dev");

	return gpio_remote_init();
}

/*
 *
 */
void cleanup_module(void)
{
	int ret;


	ret = lirc_unregister_plugin(minor);
 
	if (0 > ret) {
		printk("lirc_gpio_i %d: error in lirc_unregister_minor: %d\n"
		       "Trying again...\n",
		       minor, ret);

		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ);

		ret = lirc_unregister_plugin(minor);
 
		if (0 > ret) {
			printk("lirc_gpio_i %d: error in lirc_unregister_minor: %d!!!\n",
			       minor, ret);
			return;
		}
	}

	dprintk("lirc_gpio_i %d: module successfully unloaded\n", minor);
}
#endif

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */

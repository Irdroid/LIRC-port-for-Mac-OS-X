/*
 * Remote control driver for the TV-card
 * key codes are obtained from GPIO port
 * 
 * (L) by Artur Lipowski <lipowski@comarch.pl>
 *     patch for the AverMedia by Santiago Garcia Mantinan <manty@i.am>
 * This code is licensed under GNU GPL
 *
 * $Id: lirc_gpio_p.c,v 1.3 2000/05/05 11:08:56 columbus Exp $
 *
 */

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 2, 4)
#error "!!! Sorry, this driver needs kernel version 2.2.4 or higher !!!"
#endif

#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/sched.h>
#include <linux/wrapper.h>

#include "../lirc_dev/lirc_dev.h"
#include "../drivers/char/bttv.h"

/* default parameters value are suitable for the PixelView Play TVPro card
 * for the AverMedias you should use:
 * gpio_lock_mask=0x010000
 * gpio_xor_mask=0x010000
 * gpio_mask=0x0f88000
 */                                                                                                    

static int debug = 0;
static int card = 0;
static int minor = -1;
static unsigned int gpio_mask = 0x1f00;
static unsigned int gpio_lock_mask = 0x8000;
static unsigned int gpio_xor_mask = 0;
static unsigned int soft_gap = 500; /* ms  0 - disable */
static unsigned int sample_rate = 12;

MODULE_PARM(debug,"i");
MODULE_PARM(card,"i");
MODULE_PARM(minor,"i");
MODULE_PARM(gpio_mask,"i");
MODULE_PARM(gpio_lock_mask,"i");
MODULE_PARM(gpio_xor_mask,"i");
MODULE_PARM(soft_gap,"i");
MODULE_PARM(sample_rate,"i");

#define dprintk  if (debug) printk


/* how many bits GPIO value can be shifted right before processing
 * it is computed from the value of gpio_mask_parameter
 */
static unsigned char gpio_pre_shift = 0;

static int get_key(void* data, unsigned char *key, int key_no)
{
	static unsigned long next_time = 0;
	static unsigned char last_key = 0xff;

	unsigned long code;
	unsigned int mask = gpio_mask;
	unsigned char shift = 0;
	unsigned char curr_key = 0;

	if (bttv_read_gpio(card, &code)) {
		dprintk("lirc_gpio_p %d: cannot read GPIO\n", card);
		return -1;
	}

	code ^= gpio_xor_mask;

 	if (gpio_lock_mask && (code & gpio_lock_mask)) {
 		last_key = 0xff;
 		return -1;
 	}

	/* extract bits from "raw" GPIO value using gpio_mask */
	code >>= gpio_pre_shift;
	while (mask) {
		if (mask & 1u) {
			curr_key |= (code & 1u) << shift++;
		}
		mask >>= 1;
		code >>= 1;
	}
	   
	if (curr_key == last_key && jiffies < next_time) {
		return -1;
	}

	next_time = jiffies + soft_gap;
	last_key = *key = curr_key;

	return 0;
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
	"lirc_gpio_p",
	0,
	0,
	0,
	NULL,
	get_key,
	NULL,
	set_use_inc,
	set_use_dec
};

/*
 *
 */
int gpio_remote_init(void)
{  	
	int ret;
	unsigned int mask;

	/* "normalize" gpio_mask
	 * this means shift it right until first bit is set
	 */
	while (!(gpio_mask & 1u)) {
		gpio_pre_shift++;
		gpio_mask >>= 1;
	}

	/* calculte scan code length in bits */
	plugin.code_length = 1;
	mask = gpio_mask >> 1;
	while(mask) {
		if (mask & 1u) {
			plugin.code_length++;
		}
		mask >>= 1;
	}

	/* translate ms to jiffies */
	soft_gap = (soft_gap*HZ) / 1000;

	plugin.minor = minor;
	plugin.sample_rate = sample_rate;

	ret = lirc_register_plugin(&plugin);
	
	if (0 > ret) {
		printk ("lirc_gpio_p %d: device registration failed with %d\n",
			minor, ret);
		return ret;
	}
	
	minor = ret;
	printk("lirc_gpio_p %d: driver registered\n", minor);

	return 0;
}

EXPORT_NO_SYMBOLS; 

/* Dont try to use it as a static version !  */

#ifdef MODULE
MODULE_DESCRIPTION("Driver module for remote control (data from bt848 GPIO port)");
MODULE_AUTHOR("Artur Lipowski");

/*
 *
 */
int init_module(void)
{
	if (MAX_IRCTL_DEVICES < minor) {
		printk("lirc_gpio_p: parameter minor (%d) must be lesst han %d!\n",
		       minor, MAX_IRCTL_DEVICES-1);
		return -1;
	}
	
	if (0 > bttv_get_id(card)) {
		printk("lirc_gpio_p %d: parameter card (%d) has bad value!\n",
		       minor, card);
		return -1;
	}

	if (!gpio_mask) {
		printk("lirc_gpio_p %d: parameter gpio_mask cannot be zero!\n", minor);
		return -1;
	}

	if (2 > sample_rate || 50 < sample_rate) {
		printk("lirc_gpio_p %d: parameter sample_rate must be beetween 2 and 50!\n",
                       minor);
		return -1;
	}

	if (soft_gap && ((2000/sample_rate) > soft_gap || 1000 < soft_gap)) {
		printk("lirc_gpio_p %d: parameter soft_gap must be beetween %d and 1000!\n",
                       minor, 2000/sample_rate);
		return -1;
	}

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
		printk("lirc_gpio_p %d: error in lirc_unregister_minor: %d\n"
		       "Trying again...\n",
		       minor, ret);

		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ);

		ret = lirc_unregister_plugin(minor);
 
		if (0 > ret) {
			printk("lirc_gpio_p %d: error in lirc_unregister_minor: %d!!!\n",
			       minor, ret);
			return;
		}
	}

	dprintk("lirc_gpio_p %d: module successfully unloaded\n", minor);
}
#endif

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */

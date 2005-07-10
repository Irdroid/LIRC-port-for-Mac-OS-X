/*      $Id: lirc_cmdir.c,v 1.1 2005/07/10 08:34:13 lirc Exp $      */

/*
 * lirc_cmdir.c - Driver for InnovationOne's COMMANDIR USB Transceiver
 *
 *  This driver requires the COMMANDIR hardware driver, available at
 *  http://www.commandir.com/.
 *
 *  Copyright (C) 2005  InnovationOne - Evelyn Yeung
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
 */


#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
 
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 2, 18)
#error "**********************************************************"
#error " Sorry, this driver needs kernel version 2.2.18 or higher "
#error "**********************************************************"
#endif

#include <linux/config.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include "drivers/lirc.h"
#include "drivers/lirc_dev/lirc_dev.h"
#include "drivers/kcompat.h"
#include "lirc_cmdir.h"

struct lirc_cmdir
{
	int features;
};

struct lirc_cmdir hardware=
{
	(
	/* LIRC_CAN_SET_SEND_DUTY_CYCLE|   */
	LIRC_CAN_SET_SEND_CARRIER|
	LIRC_CAN_SEND_PULSE|
	LIRC_CAN_SET_TRANSMITTER_MASK|
	LIRC_CAN_REC_MODE2)
	,
};

#define LIRC_DRIVER_NAME "lirc_cmdir"
#define RBUF_LEN   256
#define WBUF_LEN   256
#define MAX_PACKET 64

static struct lirc_buffer rbuf;
static lirc_t wbuf[WBUF_LEN];
static unsigned char cmdir_char[4*WBUF_LEN];
static unsigned char write_control[MCU_CTRL_SIZE];
static unsigned int last_mc_time = 0;
static int usb_status=ON;
static unsigned char signal_num=0;

unsigned int freq = 38000;
/* unsigned int duty_cycle = 50; */


#ifndef MAX_UDELAY_MS
#define MAX_UDELAY_US 5000
#else
#define MAX_UDELAY_US (MAX_UDELAY_MS*1000)
#endif

static inline void safe_udelay(unsigned long usecs)
{
	while(usecs>MAX_UDELAY_US)
	{
		udelay(MAX_UDELAY_US);
		usecs-=MAX_UDELAY_US;
	}
	udelay(usecs);
}

static unsigned int get_time_value(unsigned int firstint, unsigned int secondint, unsigned char overflow) 
{	/* get difference between two timestamps from MCU */
	unsigned int t_answer = 0;
	
	if (secondint > firstint) 
	{
		t_answer = secondint - firstint + overflow*65535;
	} 
	else 
	{
		if (overflow > 0) 
		{
			t_answer = (65535 - firstint) + secondint + (overflow - 1)*65535;
		} 
		else 
		{
			t_answer = (65535 - firstint) + secondint;
		}
	}

	/* clamp to long signal  */
	if (t_answer > 16000000) t_answer = PULSE_MASK;
	
	return t_answer;
}


static int set_use_inc(void* data)
{
	/* Init read buffer. */
	if (lirc_buffer_init(&rbuf, sizeof(lirc_t), RBUF_LEN) < 0)
	{
		return -ENOMEM;
	}
	
	MOD_INC_USE_COUNT;
	return 0;
}

static void set_use_dec(void* data)
{
	lirc_buffer_free(&rbuf);
	MOD_DEC_USE_COUNT;
}


static void usb_error_handle(int retval)
{
	switch (retval)
	{
		case -ENODEV:
			/* device has been unplugged */
			if (usb_status == ON)
			{
				usb_status = OFF;
				printk(LIRC_DRIVER_NAME ": device is unplugged\n");
			}
			break;
		default:
			printk(LIRC_DRIVER_NAME ": usb error = %d\n", retval);
			break;
	}
}

static int write_to_usb(unsigned char *buffer, int count)
{
	int write_return;
	
	write_return = cmdir_write(buffer, count, NULL);
	if (write_return != count)
	{
		usb_error_handle(write_return);
	}
	else
	{
		if (usb_status == OFF) 
		{
			printk(LIRC_DRIVER_NAME ": device is now plugged in\n");
			usb_status = ON;
		}
	}
	return write_return;
}

static void set_freq(void)
{
	/* float tempfreq=0.0; */
	unsigned int timerval=0;
	int write_return;
	
	/* can't use floating point in 2.6 kernel! May be some loss of precision */
	/* tempfreq = freq*2;
	tempfreq = 12000000/tempfreq;
	timerval = (int)(tempfreq + 0.5);	//round to nearest int
	timerval = 256-timerval; */
	timerval = 256 - (12000000/(freq*2));
	write_control[0]=FREQ_HEADER;
	write_control[1]=timerval;
	write_control[2]=0;
	write_return = write_to_usb(write_control, MCU_CTRL_SIZE);
	if (write_return == MCU_CTRL_SIZE) printk(LIRC_DRIVER_NAME ": freq set to %dHz\n", freq);
	else printk(LIRC_DRIVER_NAME ": freq unchanged\n");
}

static int cmdir_convert_RX(unsigned char *orig_rxbuffer)
{
	unsigned char tmp_char_buffer[80];
	unsigned int tmp_int_buffer[20];
	unsigned int final_data_buffer[20];	
	unsigned int num_data_values = 0;
	unsigned char num_data_bytes = 0;
	unsigned int orig_index = 0;
	int i;
	
	for (i=0; i<80; i++) tmp_char_buffer[i]=0;
	for (i=0; i<20; i++) tmp_int_buffer[i]=0;

	/* get number of data bytes that follow the control bytes (NOT including them)	 */
	num_data_bytes = orig_rxbuffer[1];
	
	/* check if num_bytes is multiple of 3; if not, error  */
	if ((num_data_bytes%3 > 0) || (num_data_bytes > 60)) return -1;
	if (num_data_bytes < 3) return -1;
	
	/* get number of ints to be returned; num_data_bytes does NOT include control bytes */
	num_data_values = num_data_bytes/3;	
	
	for (i=0; i<num_data_values; i++) 
	{
		tmp_char_buffer[i*4] = orig_rxbuffer[i*3+2];
		tmp_char_buffer[i*4+1] = orig_rxbuffer[i*3+3];
		tmp_char_buffer[i*4+2] = 0;
		tmp_char_buffer[i*4+3] = 0;
	}
		
	/* convert to int array */
	memcpy((unsigned char*)tmp_int_buffer, tmp_char_buffer, (num_data_values*4));

	if (orig_rxbuffer[4] < 255) 
	{
		final_data_buffer[0] = get_time_value(last_mc_time, tmp_int_buffer[0],
			 orig_rxbuffer[4]);
	} 
	else 
	{
		/* is pulse */
		final_data_buffer[0] = get_time_value(last_mc_time, tmp_int_buffer[0], 0);
		final_data_buffer[0] |= PULSE_BIT;
	}
	for (i=1; i<num_data_values; i++) 
	{
		/* index of orig_rxbuffer that corresponds to overflow/pulse/space  */
		orig_index = (i+1)*3 + 1;
		if (orig_rxbuffer[orig_index] < 255) 
		{
			final_data_buffer[i] = get_time_value(tmp_int_buffer[i-1],
				 tmp_int_buffer[i], orig_rxbuffer[orig_index]);
		} 
		else 
		{
			final_data_buffer[i] = get_time_value(tmp_int_buffer[i-1],
				 tmp_int_buffer[i], 0);
			final_data_buffer[i] |= PULSE_BIT;
		}
	}
	last_mc_time = tmp_int_buffer[num_data_values-1];
		
	if(lirc_buffer_full(&rbuf))   
	{
		printk(KERN_ERR  LIRC_DRIVER_NAME ": lirc_buffer is full\n");
		return -EOVERFLOW;
	}	
	lirc_buffer_write_n(&rbuf, (char*)final_data_buffer, num_data_values);

	return 0;
}


static int usb_read_once(void)
{
	int read_retval = 0;
	int conv_retval = 0;
	unsigned char read_buffer[MAX_PACKET];
	int i=0;
	
	for (i=0; i<MAX_PACKET; i++) read_buffer[i] = 0;
	read_retval = cmdir_read(read_buffer, MAX_PACKET);
	if (!(read_retval == MAX_PACKET)) 
	{
		if (read_retval == -ENODEV) 
		{
			if (usb_status==ON) 
			{
				printk(KERN_ALERT LIRC_DRIVER_NAME ": device is unplugged\n");
				usb_status = OFF;
			}
		}
		else
		{
			/* supress errors */
			/*  printk(KERN_ALERT LIRC_DRIVER_NAME ": usb error on read = %d\n",
					 read_retval);  */
			return -ENODATA;
		}
		return -ENODATA;
	}
	else
	{
		if (usb_status==OFF) 
		{
			usb_status = ON;
			printk(LIRC_DRIVER_NAME ": device is now plugged in\n");
		}
	}

	if (read_buffer[0] & 0x08) 
	{
		conv_retval = cmdir_convert_RX(read_buffer);
		if (conv_retval == 0) 
		{
			return 0;
		}
		else
		{
			return -ENODATA;
		}
	} 
	else 
	{
		return -ENODATA;
	}		
}

int add_to_buf (void* data, struct lirc_buffer* buf)
{
	return usb_read_once();
}


static ssize_t lirc_write(struct file *file, const char *buf,
			 size_t n, loff_t * ppos)
{
	int retval,i,count;
	int num_bytes_to_send;
	unsigned int prev_length_waited=0;
	unsigned int mod_signal_length=0;
	/* double wbuf_mod=0.0;			//no floating point in 2.6 kernel  */
	unsigned int num_bytes_already_sent=0;
	unsigned int hibyte=0;
	unsigned int lobyte=0;
	int cmdir_cnt =0;
		
	if(n%sizeof(lirc_t)) return(-EINVAL);
	retval=verify_area(VERIFY_READ,buf,n);
	if(retval) return(retval);
	
	count=n/sizeof(lirc_t);
	if(count>WBUF_LEN || count%2==0) return(-EINVAL);	
	copy_from_user(wbuf,buf,n);
	
	cmdir_char[0] = TX_HEADER;
	signal_num++;
	cmdir_char[1] = signal_num;
	cmdir_cnt = 2;
	for(i=0;i<count;i++)
	{
		safe_udelay(wbuf[i]);
		prev_length_waited += wbuf[i];
	
		/*conversion to number of modulation frequency pulse edges*/
		/*  wbuf_mod = 2*freq*wbuf[i]/1000000.0;  //this was to properly round with FP	
		 mod_signal_length = (int)(wbuf_mod+0.5);	*/
		mod_signal_length = 2*freq*wbuf[i]/1000000;
		if (mod_signal_length%2 == 0) mod_signal_length++;  //need odd number
		if (i%2==0) mod_signal_length-=5;
		else mod_signal_length+=5;	
				
		hibyte = mod_signal_length/256;
		lobyte = mod_signal_length%256;
		cmdir_char[cmdir_cnt+1] = hibyte;
		cmdir_char[cmdir_cnt] = lobyte;
		cmdir_cnt += 2;
		
		/* write data to usb if full packet is collected */
		if (cmdir_cnt%MAX_PACKET == 0)
		{
			write_to_usb(cmdir_char+num_bytes_already_sent, MAX_PACKET);
			num_bytes_already_sent+= MAX_PACKET;
			prev_length_waited = 0;
			
			if ((i+1)<count) 
			{
				cmdir_char[cmdir_cnt] =	TX_HEADER;
				cmdir_char[cmdir_cnt+1] = signal_num;
				cmdir_cnt += 2;
			}
		}
	}
	
	/* add extra delay in case there are 2 usb writes too close to each other (2.4) */
	if (prev_length_waited < 2000) safe_udelay(2000-prev_length_waited);
	
	/* send last chunk of data */
	if (cmdir_cnt > num_bytes_already_sent)
	{
		num_bytes_to_send = cmdir_cnt - num_bytes_already_sent;
		write_to_usb(cmdir_char+num_bytes_already_sent, num_bytes_to_send);
	}
	return(n);
}


static int lirc_ioctl(struct inode *node,struct file *filep,unsigned int cmd,
		      unsigned long arg)
{
        int result;
	unsigned long value;
	unsigned int ivalue;
	unsigned int multiplier=1;
	unsigned int mask=0;
	int i;
	switch(cmd)
	{
	case LIRC_SET_TRANSMITTER_MASK:
		if (!(hardware.features&LIRC_CAN_SET_TRANSMITTER_MASK))
		{
			return(-ENOIOCTLCMD);
		}
		result=get_user(ivalue,(unsigned int *) arg);
		if(result) return(result);
		for(i=0;i<MAX_CHANNELS;i++) 
		{
			multiplier=multiplier*0x10;
			mask|=multiplier;
		}
		if(ivalue >= mask) return (MAX_CHANNELS);
		set_tx_channels(ivalue);
		return (0);
		break;
				
	case LIRC_GET_SEND_MODE:
		if(!(hardware.features&LIRC_CAN_SEND_MASK))
		{
			return(-ENOIOCTLCMD);
		}
		
		result=put_user(LIRC_SEND2MODE
				(hardware.features&LIRC_CAN_SEND_MASK),
				(unsigned long *) arg);
		if(result) return(result); 
		break;
	
	case LIRC_SET_SEND_MODE:
		if(!(hardware.features&LIRC_CAN_SEND_MASK))
		{
			return(-ENOIOCTLCMD);
		}
		
		result=get_user(value,(unsigned long *) arg);
		if(result) return(result);
		break;
		
	case LIRC_GET_LENGTH:
		return(-ENOSYS);
		break;
		
	case LIRC_SET_SEND_DUTY_CYCLE:
#               ifdef DEBUG
		printk(KERN_WARNING LIRC_DRIVER_NAME ": SET_SEND_DUTY_CYCLE\n");
#               endif

		if(!(hardware.features&LIRC_CAN_SET_SEND_DUTY_CYCLE))
		{
			return(-ENOIOCTLCMD);
		}
				
		result=get_user(ivalue,(unsigned int *) arg);
		if(result) return(result);
		if(ivalue<=0 || ivalue>100) return(-EINVAL);
		
		/* TODO: */
		/* printk(LIRC_DRIVER_NAME ": set_send_duty_cycle not yet supported\n"); */
	
		return 0;
		break;
		
	case LIRC_SET_SEND_CARRIER:
#               ifdef DEBUG
		printk(KERN_WARNING LIRC_DRIVER_NAME ": SET_SEND_CARRIER\n");
#               endif
		
		if(!(hardware.features&LIRC_CAN_SET_SEND_CARRIER))
		{
			return(-ENOIOCTLCMD);
		}
		
		result=get_user(ivalue,(unsigned int *) arg);
		if(result) return(result);
		if(ivalue>500000 || ivalue<24000) return(-EINVAL);
		if (ivalue != freq) 
		{
			freq=ivalue;
			set_freq();
		}
		return 0;
		break;
		
	default:
		return(-ENOIOCTLCMD);
	}
	return(0);
}

static struct file_operations lirc_fops =
{
	write:   lirc_write,
};

static struct lirc_plugin plugin = {
	name:		LIRC_DRIVER_NAME,
	minor:		-1,
	code_length:	1,
	sample_rate:	28,
	data:		NULL,
	add_to_buf:	add_to_buf,
	get_queue:	NULL,
	rbuf:		&rbuf,
	set_use_inc:	set_use_inc,
	set_use_dec:	set_use_dec,
	ioctl:		lirc_ioctl,
	fops:		&lirc_fops,
	owner:		THIS_MODULE,
};

#ifdef MODULE

MODULE_AUTHOR("Evelyn Yeung, Matt Bodkin");
MODULE_DESCRIPTION("InnovationOne driver for CommandIR USB infrared transceiver");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

#ifndef KERNEL_2_5
EXPORT_NO_SYMBOLS;
#endif

int init_module(void)
{
	plugin.features = hardware.features;
	if ((plugin.minor = lirc_register_plugin(&plugin)) < 0) 
	{
		printk(KERN_ERR  LIRC_DRIVER_NAME  
		       ": register_chrdev failed!\n");
		return -EIO;
	}
	return 0;
}

void cleanup_module(void)
{
	lirc_unregister_plugin(plugin.minor);
	printk(KERN_INFO  LIRC_DRIVER_NAME  ": module removed\n");
}

#endif



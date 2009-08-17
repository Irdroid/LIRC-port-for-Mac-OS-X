/*
 * driver for ENE KB3924 CIR (also known as ENE0100)
 *
 * Copyright (C) 2009 Maxim Levitsky <maximlevitsky@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#include "drivers/kcompat.h"
#include "drivers/lirc.h"
#include "drivers/lirc_dev/lirc_dev.h"


/* hardware address */
#define ENE_STATUS		0	/* hardware status - unused */
#define ENE_ADDR_HI 		1	/* hi byte of register address */
#define ENE_ADDR_LO 		2	/* low byte of register address */
#define ENE_IO 			3	/* read/write window */
#define ENE_MAX_IO		3


/* 8 bytes of samples, divided in 2 halfs*/
#define ENE_SAMPLE_BUFFER 		0xF8F0
#define ENE_SAMPLE_LOW_MASK 		(1 << 7)
#define ENE_SAMPLE_VALUE_MASK		0x7F
#define ENE_SAMPLE_OVERFLOW		0x7F
#define ENE_SAMPLES_SIZE		4


/* firmware settings */
#define ENE_FW_SETTINGS			0xF8F8
#define	ENE_FW_ENABLE			(1 << 0) /* enable fw processing */
#define ENE_FW_WAKE			(1 << 6) /* enable wake from S3 */
#define ENE_FW_IRQ			(1 << 7) /* enable interrupt */


/* buffer pointer, tells which half of ENE_SAMPLE_BUFFER to read */
#define ENE_FW_BUFFER_POINTER		0xF8F9
#define ENE_FW_BUFFER_POINTER_HIGH	(1 << 0)


/* IRQ registers block */
#define ENE_IRQ				0xFD09	 /* IRQ number */
#define ENE_UNK1			0xFD17   /* unknown setting = 1 */
#define ENE_IRQ_STATUS			0xFD80   /* irq status */
#define ENE_IRQ_STATUS_IR		(1 << 5) /* IR irq */


/* ADC settings */
#define ENE_ADC_UNK1			0xFEC0	 /* unknown setting = 7 */
#define ENE_ADC_UNK2			0xFEC1   /* unknown setting = 0 */
#define ENE_ADC_SAMPLE_PERIOD		0xFEC8   /* sample period in us */
#define ENE_ADC_SAMPLE_OVERFLOW		(1 << 7) /* interrupt on
						    overflows if set */

/* fimware version */
#define ENE_FW_VERSION			0xFF00
#define ENE_FW_VER_SUPP			0xC0


#define same_sign(a, b) ((((a) > 0) && (b) > 0) || ((a) < 0 && (b) < 0))

#define ENE_DRIVER_NAME 		"enecir"

#define ENE_MAXGAP 			250000	/* this is the amount
						   of time we wait
						   before turning off
						   the sampler, chosen
						   to be higher than
						   standard gap
						   values */

#define space(len) 			(-(len))   /* add a space */


struct ene_device {
	struct pnp_dev *pnp_dev;
	struct lirc_driver *lirc_driver;

	/* hw settings */
	unsigned long hw_io;
	int irq;

	/* device data */
	int idle;
	int sample;
	int in_use;
	
	struct timeval gap_start;
};


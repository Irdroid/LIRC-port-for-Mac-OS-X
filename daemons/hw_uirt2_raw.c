/*      $Id: hw_uirt2_raw.c,v 5.1 2003/10/12 14:08:29 lirc Exp $   */

/****************************************************************************
 ** hw_uirt2_raw.c **********************************************************
 ****************************************************************************
 *
 * Routines for UIRT2 receiver/transmitter.
 * Receiving using the raw mode and transmitting using struc or raw mode,
 * depending on code length.
 * 
 * Copyright (C) 2003 Mikael Magnusson <mikma@users.sourceforge.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
//#define DEBUG

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include "hardware.h"
#include "serial.h"
#include "ir_remote.h"
#include "lircd.h"
#include "receive.h"
#include "transmit.h"
#include "hw_uirt2_common.h"

#define NUMBYTES 6 

//static int debug = 2;

static uirt2_t *dev;
static lirc_t rec_buf[200];
static int rec_rptr;
static int rec_wptr;
static int rec_size;

/* exported functions  */
static int uirt2_raw_init(void);
static int uirt2_raw_deinit(void);
static int uirt2_send(struct ir_remote *remote,struct ir_ncode *code);
static char *uirt2_raw_rec(struct ir_remote *remotes);
static int uirt2_raw_decode(struct ir_remote *remote,
			    ir_code *prep,ir_code *codep,ir_code *postp,
			    int *repeat_flagp,lirc_t *remaining_gapp);
static lirc_t uirt2_raw_readdata(lirc_t timeout);

/* forwards */
static int uirt2_send_mode2_raw(uirt2_t *dev, struct ir_remote *remote,
				lirc_t *buf, int length);
static int uirt2_send_mode2_struct1(uirt2_t *dev,
				    struct ir_remote *remote,
				    struct ir_ncode *code);

struct hardware hw_uirt2_raw =
{
	LIRC_DRIVER_DEVICE,       /* default device */
	-1,                       /* fd */
	LIRC_CAN_REC_MODE2 | LIRC_CAN_SEND_PULSE, /* features */
	LIRC_MODE_PULSE,          /* send_mode */
	LIRC_MODE_MODE2,          /* rec_mode */
	0,                        /* code_length */
	uirt2_raw_init,           /* init_func */
	NULL,                     /* config_func */
	uirt2_raw_deinit,         /* deinit_func */
	uirt2_send,               /* send_func */
	uirt2_raw_rec,            /* rec_func */
	uirt2_raw_decode,         /* decode_func */
	uirt2_raw_readdata,       /* readdata */
	"uirt2_raw"
};

/*
 * queue
 */
static int queue_put(lirc_t data) {
	int next = (rec_wptr + 1) % rec_size;

	LOGPRINTF(3, "queue_put: %d", data);

	if (next != rec_rptr) {
		rec_buf[rec_wptr] = data;
		rec_wptr = next;
		return 0;
	} else {
		logprintf(LOG_ERR, "uirt2_raw: queue full");
		return -1;
	}
}


static int queue_get(lirc_t *pdata) {
	if (rec_wptr != rec_rptr) {
		*pdata = rec_buf[rec_rptr];
		rec_rptr = (rec_rptr + 1) % rec_size;
		LOGPRINTF(3, "queue_get: %d", *pdata);

		return 0;
	} else {
		logprintf(LOG_ERR, "uirt2_raw: queue empty");
		return -1;
	}
}


static int queue_is_empty() {
	return rec_wptr == rec_rptr;
}


static void queue_clear() {
	rec_rptr = 0;
	rec_wptr = 0;
}


static int uirt2_raw_decode(struct ir_remote *remote,
			    ir_code *prep,ir_code *codep,ir_code *postp,
			    int *repeat_flagp,lirc_t *remaining_gapp)
{
	int res;

	LOGPRINTF(1, "uirt2_raw_decode: enter");

	res = receive_decode(remote,prep,codep,postp,
			     repeat_flagp,remaining_gapp);

	LOGPRINTF(1, "uirt2_raw_decode: %d");

	return res;
}


static lirc_t uirt2_raw_readdata(lirc_t timeout)
{
	lirc_t data = 0;

	if (queue_is_empty()) {
		lirc_t data = uirt2_read_raw(dev, timeout);

		if (!data) {
			LOGPRINTF(1, "uirt2_raw_readdata failed");
			return 0;
		}

		queue_put(data);
	}

	queue_get(&data);

	LOGPRINTF(1, "uirt2_raw_readdata %d %d", 
		  !!(data & PULSE_BIT), data & PULSE_MASK);

	return(data);
}


static int uirt2_raw_init(void)
{
	if(!tty_create_lock(hw.device))
	{
		logprintf(LOG_ERR,"uirt2_raw: could not create lock files");
		return(0);
	}

	if((hw.fd=open(hw.device,O_RDWR|O_NONBLOCK|O_NOCTTY))<0)
	{
		logprintf(LOG_ERR,"uirt2_raw: could not open %s",hw.device);
		tty_delete_lock();
		return(0);
	}

	if(!tty_reset(hw.fd))
	{
		logprintf(LOG_ERR,"uirt2_raw: could not reset tty");
		close(hw.fd);
		tty_delete_lock();
		return(0);
	}

	// Wait for UIRT device to power up
	usleep(100 * 1000);

	if(!tty_setbaud(hw.fd,115200))
	{
		logprintf(LOG_ERR,"uirt2_raw: could not set baud rate");
		close(hw.fd);
		tty_delete_lock();
		return(0);
	}

	if((dev = uirt2_init(hw.fd)) == NULL)
	{
		logprintf(LOG_ERR, 
			  "uirt2_raw: No UIRT2 device found at %s", 
			  hw.device);
		close(hw.fd);
		tty_delete_lock();
		return(0);
	}

	if(uirt2_setmoderaw(dev) < 0)
	{
		logprintf(LOG_ERR,"uirt2_raw: could not set raw mode");
		uirt2_raw_deinit();
		return(0);
	}

	init_rec_buffer();
	init_send_buffer();

	rec_rptr = 0;
	rec_wptr = 0;
	rec_size = sizeof(rec_buf) / sizeof(rec_buf[0]);

	return(1);
}

static int uirt2_raw_deinit(void)
{
	uirt2_uninit(dev);
	dev = NULL;
	close(hw.fd);
	hw.fd = -1;
	tty_delete_lock();
	return 1;
}

static char *uirt2_raw_rec(struct ir_remote *remotes)
{
	LOGPRINTF(1, "uirt2_raw_rec");
	LOGPRINTF(1, "uirt2_raw_rec: %p", remotes);

	if(!clear_rec_buffer()) return(NULL);

	if (remotes) {
		char *res;
		res = decode_all(remotes);

		return res;
	} else {
		lirc_t data;
		
		queue_clear();
		data = uirt2_read_raw(dev, 1);
		if (data) {
			queue_put(data);
		}

		return NULL;
	}
}


static int uirt2_send(struct ir_remote *remote,struct ir_ncode *code)
{
	int length = code->length;
	lirc_t *signals = code->signals;
	int res;

	if(!init_send(remote,code)) {
		return 0;
	}

	if(send_buffer.wptr!=0) {
		length = send_buffer.wptr;
		signals = send_buffer.data;
	}

	if (length <= 0 || signals == NULL) {
		LOGPRINTF(1,"nothing to send");
		return 0;
	}

	if (length > 48) {
		LOGPRINTF(1, "Using REMSTRUC1 transmition");
		res = uirt2_send_mode2_struct1(dev, remote, code);
	} else {
		LOGPRINTF(1, "Using RAW transition");
		res = uirt2_send_mode2_raw(dev, remote, signals, length);
	}

	if (res) {
		LOGPRINTF(1, "uirt2_send: succeeded");
	}

	return res;
}


static int uirt2_send_mode2_raw(uirt2_t *dev, struct ir_remote *remote,
				lirc_t *buf, int length)
{
	byte_t tmp[1024];
	int i;
	int ir_length = 0;
	int res;
	int repeat = 7;

	if (length > 48) {
		logprintf(LOG_ERR, 
			  "uirt2_raw: to long RAW transmition %d > 48");
		return 0;
	}


	LOGPRINTF(1, "uirt2_send_mode2_raw %d %p",
		  length, buf);

	tmp[0] = 0;
	tmp[1] = 0;

	for (i = 0; i < length; i++) {
		tmp[2 + i] = buf[i] / UIRT2_UNIT;
		ir_length += buf[i];
	}

	tmp[2 + length] = uirt2_calc_freq(remote->freq) + (repeat & 0x1f);

	res = uirt2_send_raw(dev, tmp, length + 3);

	if (!res) {
		return 0;
	}

	LOGPRINTF(1, "uirt2_send_mode2_raw exit");
	return 1;
}


static void set_data_bit(byte_t *dest, int offset, int bit)
{
	int i = offset / 8;
	int j = offset % 8;
	int mask = 1 << j;

	byte_t src = dest[i];
	byte_t dst;

	if (bit) {
		dst = src | mask;
	} else {
		dst = src & ~mask;
	}

	dest[i] = dst;
}


static int set_data_bits(byte_t *dest,
			  int offset,
			  int bits, ir_code code)
{
	int i;
	int bit;

	LOGPRINTF(1, "set_data_bits %d %d %llx", offset, bits, code);

	for (i = 0; i < bits; i++) {
		bit = (code >> i) & 1;
		set_data_bit(dest, offset + 2 * i, bit);
		set_data_bit(dest, offset + 2 * i + 1, bit);
	}

	return bits * 2;
}


static int tolerance(struct ir_remote *remote, lirc_t len, lirc_t len2)
{
	return ((len < len2 * (100 + remote->eps) / 100 &&
		 len < len2 + remote->aeps) &&
		(len > len2 * (100 - remote->eps) / 100 &&
		 len > len2 - remote->aeps));
}

/*
  Returns:
  0 - no pulse
  1 - pulse
  -1 - error
 */
static int calc_data_bit(struct ir_remote *remote, lirc_t len, ir_code *data)
{
	int bits = 0;

	if (len > 0) {
		LOGPRINTF(2, "calc_data_bit: len %d", len);

		if (tolerance(remote, len, remote->pzero)) {
			*data = 0;
			bits = 1;
		}
		else if (tolerance(remote, len, remote->pone)) {
			*data = 1;
			bits = 1;
		}
		else {
			bits = 0;
			logprintf(LOG_WARNING,
				  "uirt2_raw: Can't encode pulse in struct1");
		}
	}

	LOGPRINTF(2, "calc_data_bit: pulse %d, data %d", *data, bits);
	
	return bits;
}


static int set_data_pulse(byte_t *dest, int offset,
			  struct ir_remote *remote,
			  lirc_t len)
{
	ir_code data = 0;
	int bits;

	bits = calc_data_bit(remote, len, &data);

	set_data_bits(dest, offset, bits, data);

	return bits;
}


static int set_bits_reverse(byte_t *dest,
			    int offset,
			    int bits, ir_code data)
{
	return set_data_bits(dest, offset, bits,
			     reverse(data, bits));
}


/* returns: number of bits */
static int set_all_bits(remstruct1_t *buf, struct ir_remote *remote,
			struct ir_ncode *code)
{
	int offset = 0;

	// plead
	offset += set_data_pulse(buf->bDatBits, offset,
				 remote, remote->plead);

	// pre_data
	offset += set_bits_reverse(buf->bDatBits, offset,
				   remote->pre_data_bits,
				   remote->pre_data);

	// code
	offset += set_bits_reverse(buf->bDatBits, offset,
				   remote->bits,
				   code->code);

	// post_data
	offset += set_bits_reverse(buf->bDatBits, offset,
				   remote->post_data_bits,
				   remote->post_data);

	// ptrail
	offset += set_data_pulse(buf->bDatBits, offset,
				 remote, remote->ptrail);

	return offset;
}


static int uirt2_send_mode2_struct1(uirt2_t *dev,
				    struct ir_remote *remote,
				    struct ir_ncode *code)
{
	remstruct1_t buf;
	int repeat_count = 3;
	int res;
	int bits;

	if (!(remote->flags & SPACE_ENC) ||
	      remote->pfoot || remote->sfoot ||
	      remote->pre_p || remote->pre_s ||
	      remote->post_p || remote->post_s) {
		logprintf(LOG_ERR, 
			  "uirt2_raw: remote not supported");
		return 0;
	}

	memset(&buf, 0, sizeof(buf));

	bits = set_all_bits(&buf, remote, code);
	
	buf.bCmd = uirt2_calc_freq(remote->freq) + repeat_count;
	buf.bISDlyHi = remote->gap / UIRT2_UNIT / 256;
	buf.bISDlyLo = (remote->gap / UIRT2_UNIT) & 255;
	buf.bBits = bits;
	buf.bHdr1 = remote->phead / UIRT2_UNIT;
	buf.bHdr0 = remote->shead / UIRT2_UNIT;
	buf.bOff0 = remote->szero / UIRT2_UNIT;
	buf.bOff1 = remote->sone / UIRT2_UNIT;
	buf.bOn0 = remote->pzero / UIRT2_UNIT;
	buf.bOn1 = remote->pone / UIRT2_UNIT;

	LOGPRINTF(2, "bits %d", bits);

	res = uirt2_send_struct1(dev, &buf);

	return res;
}


/*      $Id: hw_uirt2_raw.c,v 5.2 2003/10/26 14:52:45 lirc Exp $   */

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
                                    lirc_t *buf, int length);

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

	/* Wait for UIRT device to power up */
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
	int res = 0;

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

        LOGPRINTF(1, "Trying REMSTRUC1 transmission");
        res = uirt2_send_mode2_struct1(dev, remote, 
                                       signals, length);
        if (!res && (length < 48)) {
                LOGPRINTF(1, "Using RAW transission");
                res = uirt2_send_mode2_raw(dev, remote, 
                                           signals, length);
        }

        if (!res) {
                logprintf(LOG_ERR,
                          "uirt2_send: remote not supported");
        } else {
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
			  "uirt2_raw: to long RAW transmission %d > 48");
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

static int calc_data_bit(struct ir_remote *remote,
                         int table[], int table_len, int signal)
{
        int i;

        for (i = 0; i < table_len; i++)
        {
                if (table[i] == 0)
                {
                        table[i] = signal / UIRT2_UNIT;
                        
                        LOGPRINTF(2, "table[%d] = %d\n", i, table[i]);

                        return i;
                }

                if (expect(remote, signal, table[i] * UIRT2_UNIT))
                {
                        LOGPRINTF(2, "expect %d, table[%d] = %d\n",
                                  signal / UIRT2_UNIT, i, table[i]);
                        return i;
                }
        }

        LOGPRINTF(2, "Couldn't find %d\n", signal/UIRT2_UNIT);

        return -1;
}

static int uirt2_send_mode2_struct1(uirt2_t *dev,
                                    struct ir_remote *remote,
                                    lirc_t *buf, int length)
{
	const int REPEAT_COUNT = 3;
        const int TABLE_LEN = 2;
        remstruct1_t rem;
	int res;
        int table[2][TABLE_LEN];
	int bits = 0;
        int i;

        if (length - 2 > UIRT2_MAX_BITS)
                return 0;

	memset(&rem, 0, sizeof(rem));

        memset(table[0], 0, sizeof(table[0]));
        memset(table[1], 0, sizeof(table[1]));

        for (i = 0; i < length; i++) {
                int bit;
                int len = buf[i] / UIRT2_UNIT;

                if (i == 0)
                {
                        rem.bHdr1 = len;
                        continue;
                }
                else if (i == 1)
                {
                        rem.bHdr0 = len;
                        continue;
                }

                bit = calc_data_bit(remote, table[i % 2], TABLE_LEN, buf[i]);

                if (bit < 0)
                {
                        return 0;
                }

                set_data_bit(rem.bDatBits, i - 2, bit);
                bits++;
	}

	rem.bCmd = uirt2_calc_freq(remote->freq) + REPEAT_COUNT;
	rem.bISDlyHi = remote->gap / UIRT2_UNIT / 256;
	rem.bISDlyLo = (remote->gap / UIRT2_UNIT) & 255;
	rem.bBits = bits;
	rem.bOff0 = table[1][0];
	rem.bOff1 = table[1][1];
	rem.bOn0 = table[0][0];
	rem.bOn1 = table[0][1];

	LOGPRINTF(2, "bits %d", bits);

	res = uirt2_send_struct1(dev, &rem);

	return res;
}

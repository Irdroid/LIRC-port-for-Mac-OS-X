/*
 * hw_livedrive.c - lirc routines for a Creative Labs LiveDrive.
 *
 *     Copyright (C) 2003 Stephen Beahm <stephenbeahm@adelphia.net>
 *
 *     This program is free software; you can redistribute it and/or 
 *     modify it under the terms of the GNU General Public License as 
 *     published by the Free Software Foundation; either version 2 of 
 *     the License, or (at your option) any later version. 
 * 
 *     This program is distributed in the hope that it will be useful, 
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of 
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
 *     GNU General Public License for more details. 
 * 
 *     You should have received a copy of the GNU General Public 
 *     License along with this program; if not, write to the Free 
 *     Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, 
 *     USA. 
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

#include "hardware.h"
#include "ir_remote.h"
#include "lircd.h"

#define NUMBYTES 12

#define SYSEX     0xF0
#define SYSEX_END 0xF7

struct timeval start, end, last;
lirc_t gap;
ir_code pre, code;

struct remote_cmd
{
	unsigned char vendor_id[3];
	unsigned char dev;
	unsigned char filler[2];
	unsigned char keygroup;
	unsigned char remote[2];
	unsigned char key[2];
	unsigned char sysex_end;
};

int livedrive_decode(struct ir_remote *remote,
		     ir_code * prep, ir_code * codep, ir_code * postp,
		     int *repeat_flagp, lirc_t * remaining_gapp)
{
	if (!map_code(remote, prep, codep, postp, 16, pre, 16, code, 0, 0))
		return (0);

	gap = 0;
	if (start.tv_sec - last.tv_sec >= 2)
		*repeat_flagp = 0;
	else {
		gap = time_elapsed(&last, &start);

		if (gap < 500000)
			*repeat_flagp = 1;
		else
			*repeat_flagp = 0;
	}

	return (1);
}

int livedrive_init(void)
{
	if ((hw.fd = open(hw.device, O_RDONLY, 0)) < 0) {
		logprintf(LOG_ERR, "could not open %s", hw.device);
		return (0);
	}

	return (1);
}

int livedrive_deinit(void)
{
	close(hw.fd);
	return (1);
}

char *livedrive_rec(struct ir_remote *remotes)
{
	struct remote_cmd cmd;
	unsigned char buf;
	ir_code bit[4];

	last = end;

	gettimeofday(&start, NULL);
	/* poll for system exclusive status byte so we don't try to
	   record other midi events */
	do {
		read(hw.fd, &buf, 1);
		usleep(1);
	}
	while (buf != SYSEX);

	read(hw.fd, &cmd, NUMBYTES);
	gettimeofday(&end, NULL);

	/* test for correct system exclusive end byte so we don't try
	   to record other midi events */
	if (cmd.sysex_end != SYSEX_END)
		return (NULL);

	bit[0] = (cmd.keygroup >> 3) & 0x1;
	bit[1] = (cmd.keygroup >> 2) & 0x1;
	bit[2] = (cmd.keygroup >> 1) & 0x1;
	bit[3] = (cmd.keygroup >> 0) & 0x1;

	pre = reverse(cmd.remote[0] |
		      (cmd.remote[1] << 8), 16) | (bit[0] << 8) | bit[1];
	code = reverse(cmd.key[0] |
		       (cmd.key[1] << 8), 16) | (bit[2] << 8) | bit[3];
	
	return (decode_all(remotes));
}

struct hardware hw_livedrive = {
	"/dev/midi",		/* simple device */
	-1,			/* fd */
	LIRC_CAN_REC_LIRCCODE,	/* features */
	0,			/* send_mode */
	LIRC_MODE_LIRCCODE,	/* rec_mode */
	32,			/* code_length */
	livedrive_init,		/* init_func */
	NULL,			/* config_func */
	livedrive_deinit,	/* deinit_func */
	NULL,			/* send_func */
	livedrive_rec,		/* rec_func */
	livedrive_decode,	/* decode_func */
	NULL,
	"livedrive"
};

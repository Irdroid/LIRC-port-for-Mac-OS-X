/****************************************************************************
 ** hw_hiddev.c ***********************************************************
 ****************************************************************************
 *
 * receive keycodes input via /dev/usb/hiddev...
 * 
 * Copyright (C) 2002 Oliver Endriss <o.endriss@gmx.de>
 * Copyright (C) 2004 Chris Pascoe <c.pascoe@itee.uq.edu.au>
 * Copyright (C) 2005 William Uther <william.uther@nicta.com.au>
 *
 * Distribute under GPL version 2 or later.
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <sys/fcntl.h>

#include <linux/types.h>
#include <linux/hiddev.h>

#include "hardware.h"
#include "ir_remote.h"
#include "lircd.h"
#include "receive.h"


static int hiddev_init();
static int hiddev_deinit(void);
static int hiddev_decode(struct ir_remote *remote,
			   ir_code *prep, ir_code *codep, ir_code *postp,
			   int *repeat_flagp, lirc_t *remaining_gapp);
static char *hiddev_rec(struct ir_remote *remotes);

struct hardware hw_dvico=
{
	"/dev/usb/hiddev0", /* "device" */
	-1,			/* fd (device) */
	LIRC_CAN_REC_LIRCCODE,	/* features */
	0,			/* send_mode */
	LIRC_MODE_LIRCCODE, /* rec_mode */
	32,			/* code_length */
	hiddev_init,		/* init_func */
	NULL,			/* config_func */
	hiddev_deinit,	/* deinit_func */
	NULL,			/* send_func */
	hiddev_rec,		/* rec_func */
	hiddev_decode,	/* decode_func */
	NULL,			/* readdata */
	"dvico"
};

static int dvico_repeat_mask = 0x8000;

static int pre_code_length = 32;
static int main_code_length = 32;

static unsigned int pre_code;
static signed int main_code = 0;

static int repeat_flag = 0;

int hiddev_init()
{
	logprintf(LOG_INFO, "initializing '%s'", hw.device);
	
	if ((hw.fd = open(hw.device, O_RDONLY)) < 0) {
		logprintf(LOG_ERR, "unable to open '%s'", hw.device);
		return 0;
	}
	
	return 1;
}


int hiddev_deinit(void)
{
	logprintf(LOG_INFO, "closing '%s'", hw.device);
	close(hw.fd);
	hw.fd=-1;
	return 1;
}


int hiddev_decode(struct ir_remote *remote,
			ir_code *prep, ir_code *codep, ir_code *postp,
			int *repeat_flagp, lirc_t *remaining_gapp)
{
	LOGPRINTF(1, "hiddev_decode");

	if(!map_code(remote,prep,codep,postp,
			 pre_code_length,pre_code,
			 main_code_length,main_code,
			 0,0))
	{
		return(0);
	}
	
	LOGPRINTF(1, "lirc code: 0x%X", *codep);

	*repeat_flagp = repeat_flag;
	*remaining_gapp = 0;
	
	return 1;
}


char *hiddev_rec(struct ir_remote *remotes)
{
	struct hiddev_event event;
	int rd;

	LOGPRINTF(1, "hiddev_rec");
	
	rd = read(hw.fd, &event, sizeof event);
	if (rd != sizeof event) {
		logprintf(LOG_ERR, "error reading '%s'", hw.device);
		return 0;
	}

	LOGPRINTF(1, "hid 0x%X  value 0x%X", event.hid, event.value);

	pre_code = event.hid;
	main_code = event.value;

	/*
	 * This stuff is probably dvico specific.
	 * I don't have any other hid devices to test...
	 */
	if (event.hid == 0x10046) {
		repeat_flag = (main_code & dvico_repeat_mask);
		main_code = (main_code & ~dvico_repeat_mask);
		return decode_all(remotes);
	}

	/* insert decoding logic for other hiddev remotes here */

	return 0;
}

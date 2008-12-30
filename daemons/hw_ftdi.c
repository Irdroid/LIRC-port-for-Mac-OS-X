/****************************************************************************
 ** hw_ftdi.c ***************************************************************
 ****************************************************************************
 *
 * Mode2 receiver using the bitbang mode of an FTDI USB-to-serial chip such as
 * the FT232R, with a demodulating IR receiver connected to one of the FTDI
 * chip's data pins -- by default, D1 (RXD).
 *
 * Copyright (C) 2008 Albert Huitsing <albert@huitsing.nl>
 * Copyright (C) 2008 Adam Sampson <ats@offog.org>
 *
 * Inspired by the UDP driver, which is:
 * Copyright (C) 2002 Jim Paris <jim@jtan.com>
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
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include "hardware.h"
#include "ir_remote.h"
#include "lircd.h"
#include "receive.h"
#include "transmit.h"
#include "hw_default.h"

#include <ftdi.h>

/* PID of the child process */
static pid_t child_pid = -1;

#define RXBUFSZ		2048

static char *device_config = NULL;
static int baud_rate = 4800;
static int input_pin = 1; /* RXD (RTS is output) */
static int usb_vendor = 0x0403; /* default for FT232 */
static int usb_product = 0x6001;
static const char *usb_desc = NULL;
static const char *usb_serial = NULL;

static int laststate = -1;
static unsigned long rxctr = 0;

static void parsesamples(unsigned char *buf,int n,int pipe_w)
{
	int i;

	for (i=0; i<n; i++)
	{
		int curstate = (buf[i] & (1 << input_pin)) != 0;
		rxctr++;

		if (curstate != laststate)
		{
			/* Convert number of samples to us.
			 * The sample rate in bitbang mode is 16 times the baud
			 * rate. */
			lirc_t usecs = (rxctr * 1000000LL) / (baud_rate * 16);

			/* Clamp */
			if (usecs > PULSE_MASK)
			{
				usecs = PULSE_MASK;
			}

			/* Indicate pulse or bit */
			if (curstate)
			{
				usecs |= PULSE_BIT;
			}

			/* Send the sample */
			write(pipe_w, &usecs, sizeof usecs);

			/* Remember last state */
			laststate = curstate;
			rxctr = 0;
		}
	}
}

static void child_process(int pipe_w)
{
	struct ftdi_context ftdic;

	alarm(0);
	signal(SIGTERM, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGHUP, SIG_IGN);
	signal(SIGALRM, SIG_IGN);

	ftdi_init(&ftdic);

	while (1)
	{
		int ret;

		/* Open the USB device */
		if (ftdi_usb_open_desc(&ftdic, usb_vendor, usb_product,
				       usb_desc, usb_serial)<0)
		{
			logprintf(LOG_ERR, "unable to open FTDI device (%s)",
				  ftdi_get_error_string(&ftdic));
			goto retry;
		}

		/* Enable bit-bang mode, setting all pins as inputs */
		if (ftdi_enable_bitbang(&ftdic,0)<0)
		{
			logprintf(LOG_ERR,
				  "unable to enable bitbang mode (%s)",
				  ftdi_get_error_string(&ftdic));
			goto retry;
		}

		/* Set baud rate */
		if (ftdi_set_baudrate(&ftdic, baud_rate)<0)
		{
			logprintf(LOG_ERR,
				  "unable to set required baud rate (%s)",
				  ftdi_get_error_string(&ftdic));
			goto retry;
		}

		logprintf(LOG_INFO,"opened FTDI device '%s' OK",hw.device);

		do
		{
			unsigned char buf[RXBUFSZ];

			/* Read until an error occurs */
			ret = ftdi_read_data(&ftdic, buf, sizeof buf);
			if (ret>0)
			{
				parsesamples(buf, ret, pipe_w);
			}
		} while (ret>0);

	retry:
		/* Wait a while and try again */
		usleep(500000);
	}
}

static int hwftdi_init()
{
	int pipe_fd[2] = { -1, -1 };
	char *p;

	logprintf(LOG_INFO, "Initializing FTDI: %s", hw.device);

	/* Parse the device string, which has the form key=value,key=value,...
	 * This isn't very nice, but it's not a lot more complicated than what
	 * some of the other drivers do. */
	p = device_config = strdup(hw.device);
	while (p)
	{
		char *comma, *value;

		comma = strchr(p,',');
		if (comma != NULL)
		{
			*comma = '\0';
		}

		/* Skip empty options. */
		if (*p == '\0')
		{
			goto next;
		}

		value = strchr(p, '=');
		if (value == NULL)
		{
			logprintf(LOG_ERR, "device configuration option "
				  "must contain an '=': '%s'", p);
			goto fail;
		}
		*value++ = '\0';

		if (strcmp(p, "vendor") == 0)
		{
			usb_vendor = strtol(value, NULL, 0);
		}
		else if (strcmp(p, "product") == 0)
		{
			usb_product = strtol(value, NULL, 0);
		}
		else if (strcmp(p, "desc") == 0)
		{
			usb_desc = value;
		}
		else if (strcmp(p, "serial") == 0)
		{
			usb_serial = value;
		}
		else if (strcmp(p, "input") == 0)
		{
			input_pin = strtol(value, NULL, 0);
		}
		else if (strcmp(p, "baud") == 0)
		{
			baud_rate = strtol(value, NULL, 0);
		}
		else
		{
			logprintf(LOG_ERR, "unrecognised device configuration "
				  "option: '%s'",p);
			goto fail;
		}

	next:
		if (comma == NULL)
		{
			break;
		}
		p = comma + 1;
	}

	init_rec_buffer();

	/* Allocate a pipe for lircd to read from */
	if (pipe(pipe_fd) == -1)
	{
		logprintf(LOG_ERR, "unable to create pipe");
		goto fail;
	}
	hw.fd = pipe_fd[0];

	/* Spawn the child process */
	child_pid = fork();
	if (child_pid == -1)
	{
		logprintf(LOG_ERR, "unable to fork child process");
		goto fail;
	}
	else if (child_pid == 0)
	{
		close(pipe_fd[0]);
		child_process(pipe_fd[1]);
	}
	close(pipe_fd[1]);

	return(1);

fail:
	if (hw.fd != -1)
	{
		close(pipe_fd[0]);
		close(pipe_fd[1]);
		hw.fd = -1;
	}

	if (device_config != NULL)
	{
		free(device_config);
		device_config = NULL;
	}

	return(0);
}

static int hwftdi_deinit(void)
{
	if (child_pid != -1)
	{
		/* Kill the child process, and wait for it to exit */
		if (kill(child_pid, SIGTERM) == -1)
		{
			return(0);
		}
		if (waitpid(child_pid, NULL, 0) == 0)
		{
			return(0);
		}
		child_pid = -1;
	}

	close(hw.fd);
	hw.fd = -1;

	free(device_config);
	device_config = NULL;

	return(1);
}

static char *hwftdi_rec(struct ir_remote *remotes)
{
	if (!clear_rec_buffer()) return(NULL);
	return(decode_all(remotes));
}

static lirc_t hwftdi_readdata(lirc_t timeout)
{
	int n;
	lirc_t res = 0;

	if (!waitfordata((long)timeout))
	{
		return 0;
	}

	n = read(hw.fd, &res, sizeof res);
	if (n != sizeof res)
	{
		res = 0;
	}

	return(res);
}

struct hardware hw_ftdi=
{
	"",                 /* "device" -> used as configuration */
	-1,                 /* fd */
	LIRC_CAN_REC_MODE2, /* features */
	0,                  /* send_mode */
	LIRC_MODE_MODE2,    /* rec_mode */
	0,                  /* code_length */
	hwftdi_init,	    /* init_func */
	NULL,		    /* config_func */
	hwftdi_deinit,      /* deinit_func */
	NULL,		    /* send_func */
	hwftdi_rec,         /* rec_func */
	receive_decode,     /* decode_func */
	NULL,               /* ioctl_func */
	hwftdi_readdata,    /* readdata */
	"ftdi"
};

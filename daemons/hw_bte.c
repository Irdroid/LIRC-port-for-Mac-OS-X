/****************************************************************************
 ** hw_bte.c ****************************************************************
 ****************************************************************************
 *
 *  routines for Ericsson mobile phone receiver (BTE)
 * 
 *  Copyright (C) 2003 Vadim Shliakhov <vadp@fromru.com>
 *
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
 *
 *
 *  14-07-03 log flood fixed (vss)
 *
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
#include <errno.h>

#include "hardware.h"
#include "receive.h"
#include "serial.h"
#include "ir_remote.h"
#include "lircd.h"
#include "hw_bte.h"


extern int errno;

struct timeval start,end,last;
lirc_t gap,signal_length;
ir_code pre,code;

#define BTE_CAN_SEND 0
struct hardware hw_bte=
{
	LIRC_DRIVER_DEVICE,       /* default device */
	-1,                       /* fd */
#if BTE_CAN_SEND
	LIRC_CAN_REC_STRING|LIRC_CAN_SEND_STRING,    /* features */
	LIRC_MODE_STRING,         /* send_mode */
#else
	LIRC_CAN_REC_STRING,      /* features */
	0,                        /* send_mode */
#endif

	LIRC_MODE_STRING,         /* rec_mode */
	16,                        /* code_length */
	bte_init,				 /* init_func */
	NULL,                     /* config_func */
	bte_deinit,	           /* deinit_func */
#if BTE_CAN_SEND
	bte_send,                 /* send_func */
#else
	NULL,                     /* send_func */
#endif
	bte_rec,             	 /* rec_func */
	bte_decode,          	 /* decode_func */
	NULL,                     /* readdata */
	"bte"
};

enum bte_state {
	BTE_NONE=0, BTE_INIT, BTE_SET_ECHO, BTE_CHARSET, BTE_SET_ACCESSORY, 
	BTE_START_EVENTS, BTE_STOP_EVENTS
};

static int pending;
static char prev_cmd[PACKET_SIZE+1];

int bte_sendcmd(char* str, int next_state)
{
	
	pending = next_state;
	sprintf(prev_cmd,"AT%s\r", str);

	LOGPRINTF(1, "bte_sendcmd: <%s>", str);
	if( write(hw.fd, prev_cmd, strlen(prev_cmd)) <= 0 )
	{
		logprintf(LOG_ERR,"bte_sendcmd: write failed");
		return -1;
	}
	LOGPRINTF(1, "bte_sendcmd: done");
	return 0;
}

char *bte_readline()
{
	static char msg[PACKET_SIZE+1];
	static int read_failed = 0;
	char c;
	int n=0;
	int ok=1;

	LOGPRINTF(1, "bte_readline: start");
	do
	{
		if((ok=read(hw.fd,&c,1))!=1)
		{
			if (!read_failed) // don't bloat to log 
			    logprintf(LOG_ERR,
				  "bte_readline: read failed - %d : %s",
				  errno, strerror(errno));
			else
			    sleep(1);
			read_failed = 1;
			return(NULL);
		}
		else if (read_failed) // restored 
		{
			logprintf(LOG_ERR,
				  "bte_readline: read restored!");
			read_failed = 0;
		}
		if(n>=PACKET_SIZE-1)
		{
			ok=0;
			c='!';
		}
		if(c!='\r' && c!='\n')
			msg[n++]=c;
	}
	while(ok && !(c=='\n' && n!=0));
	msg[n]=0;
//	if(!ok) return(NULL);
	LOGPRINTF(1, "bte_readline: %s", msg);
	return(msg);
}

char *bte_automaton()
{
	char *msg = bte_readline();
	int key;
	int key_up;
	
	while(pending==BTE_INIT)
	{
		// tty_reset() seems to leave some garbage in a buffer
		// so skip it
		if(pending==BTE_INIT && strncmp(msg,"E: ",3)==0)
			pending=BTE_SET_ECHO;
		msg = bte_readline();
	}

	if(msg==NULL) // read failed
		return(NULL);
	else if(strcmp(msg,"ERROR")==0) // "ERROR" received
	{
		logprintf(LOG_ERR,"bte_automaton: 'ERROR' received! "
			  "Previous command: %s", prev_cmd);
		return(NULL);
	}
	else if(strcmp(msg,"OK")==0) // Check for next cmd to send
	{
		switch(pending)
		{
		case BTE_SET_ECHO:
			bte_sendcmd("E1", BTE_CHARSET);
			break;
		case BTE_CHARSET:
			// set ISO-8859-1 charset
			bte_sendcmd("+CSCS=\"8859-1\"",
				    BTE_SET_ACCESSORY);
			break;
		case BTE_SET_ACCESSORY:
			// Set accessory menu item
			bte_sendcmd("*EAM=\"BTE remote\"", 0);
			break;
		case BTE_START_EVENTS:
			// start events forwarding
			bte_sendcmd("+CMER=3,2,0,0,0", 0);
			break;
		}
	}
	else if(strcmp(msg,"*EAAI")==0) // Accessory menu activated
	{
		// create input dialog
		bte_sendcmd("*EAID=13,1,\"BTE Remote\"", BTE_START_EVENTS);
//		bte_sendcmd("*EAID=4,1,\"BTE Remote\",10,20", BTE_START_EVENTS);
	}
	else if(strcmp(msg,"*EAII: 0")==0) // Input dialog closed
	{
		// stop events forwarding
		bte_sendcmd("+CMER=0,0,0,0,0", 0);
	}
	else if(strncmp(msg,"+CKEV:",6)==0) // Key-code event
	{
		key = msg[7];
		key_up = msg[9]=='0';
		code= key_up<<8 | key;
		LOGPRINTF(1, "bte_automaton: code 0x%x", code);
		if ( key=='G' && !key_up ) // MEMO key kills widget
		{
			// recreate widget
			LOGPRINTF(1, "bte_automaton: MEMO key", code);
			bte_sendcmd("*EAID=13,1,\"BTE Remote\"", 0);
		}
	}
	strcat(msg,"\n"); // pad with newline
	return(msg);
}

char *bte_rec(struct ir_remote *remotes)
{
	if( bte_automaton())
		return decode_all(remotes);
	else
		return NULL;
}

int bte_init(void)
{
	if(!tty_create_lock(hw.device))
	{
		logprintf(LOG_ERR,"could not create lock files");
		return(0);
	}
	if((hw.fd=open(hw.device,O_RDWR|O_NOCTTY))<0)
	{
		logprintf(LOG_ERR,"could not open %s",hw.device);
		logperror(LOG_ERR,"bte_init()");
		tty_delete_lock();
		return(0);
	}
	if(!tty_reset(hw.fd))
	{
		logprintf(LOG_ERR,"could not reset tty");
		bte_deinit();
		return(0);
	}
	if(!tty_setbaud(hw.fd,115200))
	{
		logprintf(LOG_ERR,"could not set baud rate");
		bte_deinit();
		return(0);
	}

	bte_sendcmd("E?", BTE_INIT); // Ask for echo state just to syncronise
	LOGPRINTF(1, "bte_init: done");
	return(1);
}

int bte_deinit(void)
{
	// stop events forwarding
	bte_sendcmd("+CMER=0,0,0,0,0", 0);

	close(hw.fd);
	tty_delete_lock();
	LOGPRINTF(1, "bte_deinit: OK");
	return(1);
}

int bte_decode(struct ir_remote *remote,
		  ir_code *prep,ir_code *codep,ir_code *postp,
		  int *repeat_flagp,lirc_t *remaining_gapp)
{
	*prep=pre;
	*codep=code;
	*postp=0;

	gap=0;
	if(start.tv_sec-last.tv_sec>=2) /* >1 sec */
	{
		*repeat_flagp=0;
	}
	else
	{
		gap=(start.tv_sec-last.tv_sec)*1000000+
		start.tv_usec-last.tv_usec;
		
		if(gap<120000)
			*repeat_flagp=1;
		else
			*repeat_flagp=0;
	}
	
	*remaining_gapp=0;
	LOGPRINTF(1,"code: %llx",(unsigned long long) *codep);
	return(1);
}

/*      $Id: hw_default.c,v 5.10 2000/04/18 19:46:21 columbus Exp $      */

/****************************************************************************
 ** hw_default.c ************************************************************
 ****************************************************************************
 *
 * routines for hardware that supports ioctl() interface
 * 
 * Copyright (C) 1999 Christoph Bartelmus <lirc@bartelmus.de>
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

/* disable daemonise if maintainer mode SIM_REC / SIM_SEND defined */
#if defined(SIM_REC) || defined (SIM_SEND)
# undef DAEMONIZE
#endif

#include "hardware.h"
#include "ir_remote.h"
#include "lircd.h"
#include "hw_default.h"

extern struct ir_remote *repeat_remote,*last_remote;

struct sbuf send_buffer;
struct rbuf rec_buffer;

unsigned long supported_send_modes[]=
{
	/* LIRC_CAN_SEND_STRING, I don't think there ever will be a driver 
	   that supports that */
	/* LIRC_CAN_SEND_LIRCCODE, */
        /* LIRC_CAN_SEND_CODE, */
	/* LIRC_CAN_SEND_MODE2, this one would be very easy */
	LIRC_CAN_SEND_PULSE,
	/* LIRC_CAN_SEND_RAW, */
	0
};
unsigned long supported_rec_modes[]=
{
	LIRC_CAN_REC_STRING,
	LIRC_CAN_REC_LIRCCODE,
        LIRC_CAN_REC_CODE,
	LIRC_CAN_REC_MODE2,
	/* LIRC_CAN_REC_PULSE, shouldn't be too hard */
	/* LIRC_CAN_REC_RAW, */
	0
};

struct hardware hw=
{
	-1,               /* fd */
	0,                /* features */
	0,                /* send_mode */
	0,                /* rec_mode */
	0,                /* code_length */
	default_init,     /* init_func */
	default_deinit,   /* deinit_func */
	default_send,     /* send_func */
	default_rec,      /* rec_func */
	default_decode,   /* decode_func */
};

inline void set_bit(ir_code *code,int bit,int data)
{
	(*code)&=~((((ir_code) 1)<<bit));
	(*code)|=((ir_code) (data ? 1:0)<<bit);
}


/*
  sending stuff
*/

inline lirc_t time_left(struct timeval *current,struct timeval *last,
			lirc_t gap)
{
	unsigned long secs,usecs,diff;
	
	secs=current->tv_sec-last->tv_sec;
	usecs=current->tv_usec-last->tv_usec;
	
	diff=1000000*secs+usecs;
	
	return((lirc_t) (diff<gap ? gap-diff:0));
}

inline void clear_send_buffer(void)
{
	send_buffer.wptr=0;
	send_buffer.too_long=0;
	send_buffer.is_shift=0;
	send_buffer.pendingp=0;
	send_buffer.pendings=0;
	send_buffer.sum=0;
}

inline void add_send_buffer(lirc_t data)
{
	if(send_buffer.wptr<WBUF_SIZE)
	{
		send_buffer.sum+=data;
		send_buffer.data[send_buffer.wptr]=data;
		send_buffer.wptr++;
	}
	else
	{
		send_buffer.too_long=1;
	}
}

inline void send_pulse(lirc_t data)
{
	if(send_buffer.pendingp>0)
	{
		send_buffer.pendingp+=data;
	}
	else
	{
		if(send_buffer.pendings>0)
		{
			add_send_buffer(send_buffer.pendings);
			send_buffer.pendings=0;
		}
		send_buffer.pendingp=data;
	}
}

inline void send_space(lirc_t data)
{
	if(send_buffer.wptr==0 && send_buffer.pendingp==0)
	{
#ifdef DEBUG
		logprintf(1,"first signal is a space!\n");
#endif
		return;
	}
	if(send_buffer.pendings>0)
	{
		send_buffer.pendings+=data;
	}
	else
	{
		if(send_buffer.pendingp>0)
		{
			add_send_buffer(send_buffer.pendingp);
			send_buffer.pendingp=0;
		}
		send_buffer.pendings=data;
	}
}

static inline int bad_send_buffer(void)
{
	if(send_buffer.too_long!=0) return(1);
	if(send_buffer.wptr==WBUF_SIZE && send_buffer.pendingp>0)
	{
		return(1);
	}
	return(0);
}

static inline void sync_send_buffer(void)
{
	if(send_buffer.pendingp>0)
	{
		add_send_buffer(send_buffer.pendingp);
		send_buffer.pendingp=0;
	}
	if(send_buffer.wptr>0 && send_buffer.wptr%2==0) send_buffer.wptr--;
}

inline void send_header(struct ir_remote *remote)
{
	if(has_header(remote))
	{
		send_pulse(remote->phead);
		send_space(remote->shead);
	}
}

inline void send_foot(struct ir_remote *remote)
{
	if(has_foot(remote))
	{
		send_space(remote->sfoot);
		send_pulse(remote->pfoot);
	}
}

inline void send_lead(struct ir_remote *remote)
{
	if(remote->plead!=0)
	{
		send_pulse(remote->plead);
	}
}

inline void send_trail(struct ir_remote *remote)
{
	if(remote->ptrail!=0)
	{
		send_pulse(remote->ptrail);
	}
}

inline void send_data(struct ir_remote *remote,ir_code data,int bits)
{
	int i;

	data=reverse(data,bits);
	for(i=0;i<bits;i++)
	{
		if(data&1)
		{
			if(is_shift(remote))
			{
				send_space(remote->sone);
				send_pulse(remote->pone);
			}
			else
			{
				send_pulse(remote->pone);
				send_space(remote->sone);
			}
		}
		else
		{
			send_pulse(remote->pzero);
			send_space(remote->szero);
		}
		data=data>>1;
	}
}

inline void send_pre(struct ir_remote *remote)
{
	if(has_pre(remote))
	{
		ir_code pre;

		pre=remote->pre_data;
		if(remote->repeat_bit>0)
		{
			if(remote->repeat_bit<=remote->pre_data_bits)
			{
				set_bit(&pre,
					remote->pre_data_bits
					-remote->repeat_bit,
					remote->repeat_state);
			}
		}

		send_data(remote,pre,remote->pre_data_bits);
		if(remote->pre_p>0 && remote->pre_s>0)
		{
			send_pulse(remote->pre_p);
			send_space(remote->pre_s);
		}
	}
}

inline void send_post(struct ir_remote *remote)
{
	if(has_post(remote))
	{
		ir_code post;

		post=remote->post_data;
		if(remote->repeat_bit>0)
		{
			if(remote->repeat_bit>remote->pre_data_bits
			   +remote->bits
			   &&
			   remote->repeat_bit<=remote->pre_data_bits
			   +remote->bits
			   +remote->post_data_bits)
			{
				set_bit(&post,
					remote->pre_data_bits
					+remote->bits
					+remote->post_data_bits
					-remote->repeat_bit,
					remote->repeat_state);
			}
		}
		
		if(remote->post_p>0 && remote->post_s>0)
		{
			send_pulse(remote->post_p);
			send_space(remote->post_s);
		}
		send_data(remote,post,remote->post_data_bits);
	}
}

inline void send_repeat(struct ir_remote *remote)
{
	send_lead(remote);
	send_pulse(remote->prepeat);
	send_space(remote->srepeat);
	send_trail(remote);
}

inline void send_code(struct ir_remote *remote,ir_code code)
{
	if(remote->repeat_bit>0)
	{
		if(remote->repeat_bit>remote->pre_data_bits
		   &&
		   remote->repeat_bit<=remote->pre_data_bits
		   +remote->bits)
		{
			set_bit(&code,
				remote->pre_data_bits
				+remote->bits
				-remote->repeat_bit,
				remote->repeat_state);
		}
		else if(remote->repeat_bit>remote->pre_data_bits
			+remote->bits
			+remote->post_data_bits)
		{
			logprintf(0,"bad repeat_bit\n");
		}
	}

	if(repeat_remote==NULL || !(remote->flags&NO_HEAD_REP))
		send_header(remote);
	send_lead(remote);
	send_pre(remote);
	send_data(remote,code,remote->bits);
	send_post(remote);
	send_trail(remote);
	if(repeat_remote==NULL || !(remote->flags&NO_FOOT_REP))
		send_foot(remote);

	if(repeat_remote==NULL && (remote->flags&(NO_HEAD_REP|CONST_LENGTH)))
	{
		send_buffer.sum-=remote->phead+remote->shead;
	}
}

int init_send(struct ir_remote *remote,struct ir_ncode *code)
{
	clear_send_buffer();
	if(is_shift(remote))
	{
		send_buffer.is_shift=1;
	}
	
	if(repeat_remote!=NULL && has_repeat(remote))
	{
		if(remote->flags&REPEAT_HEADER && has_header(remote))
		{
			send_header(remote);
		}
		send_repeat(remote);
	}
	else
	{
		if(!is_raw(remote))
		{
			send_code(remote,code->code);
		}
	}
	sync_send_buffer();
	if(bad_send_buffer())
	{
		logprintf(0,"buffer too small\n");
		return(0);
	}
	if(is_const(remote))
	{
		if(remote->gap>send_buffer.sum)
		{
			remote->remaining_gap=remote->gap
			-send_buffer.sum;
		}
		else
		{
			logprintf(0,"too short gap: %lu\n",remote->gap);
			remote->remaining_gap=remote->gap;
			return(0);
		}
	}
	else
	{
		if(has_repeat_gap(remote) &&
		   repeat_remote!=NULL &&
		   has_repeat(remote))
		{
			remote->remaining_gap=remote->repeat_gap;
		}
		else
		{
			remote->remaining_gap=remote->gap;
		}
	}
	return(1);
}


/*
  decoding stuff
*/

lirc_t readdata(void)
{
	lirc_t data;
	int ret;

#if defined(SIM_REC) && !defined(DAEMONIZE)
	while(1)
	{
		unsigned long scan;

		ret=fscanf(stdin,"space %ld\n",&scan);
		if(ret==1)
		{
			data=(lirc_t) scan;
			break;
		}
		ret=fscanf(stdin,"pulse %ld\n",&scan);
		if(ret==1)
		{
			data=(lirc_t) scan|PULSE_BIT;
			break;
		}
		ret=fscanf(stdin,"%*s\n");
		if(ret==EOF)
		{
			dosigterm(SIGTERM);
		}
	}
#else
	ret=read(hw.fd,&data,sizeof(data));
	if(ret!=sizeof(data))
	{
#               ifdef DEBUG
		logprintf(1,"error reading from lirc\n");
		logperror(1,NULL);
#               endif
		dosigterm(SIGTERM);
	}
#endif
	return(data);
}

lirc_t get_next_rec_buffer(unsigned long maxusec)
{
	if(rec_buffer.rptr<rec_buffer.wptr)
	{
#               ifdef DEBUG
		logprintf(3,"<%lu\n",(unsigned long)
			  rec_buffer.data[rec_buffer.rptr]&(PULSE_BIT-1));
#               endif
		rec_buffer.sum+=rec_buffer.data[rec_buffer.rptr]&(PULSE_BIT-1);
		return(rec_buffer.data[rec_buffer.rptr++]);
	}
	else
	{
		if(rec_buffer.wptr<RBUF_SIZE)
		{
			lirc_t data;
			
			if(!waitfordata(maxusec)) return(0);
			data=readdata();
                        rec_buffer.data[rec_buffer.wptr]=data;
                        if(rec_buffer.data[rec_buffer.wptr]==0) return(0);
                        rec_buffer.sum+=rec_buffer.data[rec_buffer.rptr]
				&(PULSE_BIT-1);
                        rec_buffer.wptr++;
                        rec_buffer.rptr++;
#                       ifdef DEBUG
                        logprintf(3,"+%lu\n",(unsigned long)
				  rec_buffer.data[rec_buffer.rptr-1]
                                  &(PULSE_BIT-1));
#                       endif
                        return(rec_buffer.data[rec_buffer.rptr-1]);
		}
		else
		{
			rec_buffer.too_long=1;
			return(0);
		}
	}
	return(0);
}

int clear_rec_buffer()
{
	int move,i;


	if(hw.rec_mode==LIRC_MODE_LIRCCODE)
	{
		unsigned char buffer[sizeof(ir_code)];
		size_t count;
		
		count=hw.code_length/CHAR_BIT;
		if(hw.code_length%CHAR_BIT) count++;
		
		if(read(hw.fd,buffer,count)!=count)
		{
			logprintf(0,"reading in mode LIRC_MODE_LIRCCODE "
				  "failed\n");
			return(0);
		}
		for(i=0,rec_buffer.decoded=0;i<count;i++)
		{
			rec_buffer.decoded=(rec_buffer.decoded<<CHAR_BIT)+
			((ir_code) buffer[i]);
		}
	}
	else if(hw.rec_mode==LIRC_MODE_CODE)
	{
		unsigned char c;
		
		if(read(hw.fd,&c,1)!=1)
		{
			logprintf(0,"reading in mode LIRC_MODE_CODE "
				  "failed\n");
			return(0);
		}
		rec_buffer.decoded=(ir_code) c;
	}
	else
	{
		lirc_t data;
		
		move=rec_buffer.wptr-rec_buffer.rptr;
		if(move>0 && rec_buffer.rptr>0)
		{
			memmove(&rec_buffer.data[0],
				&rec_buffer.data[rec_buffer.rptr],
				sizeof(rec_buffer.data[0])*move);
			rec_buffer.wptr-=rec_buffer.rptr;
		}
		else
		{
			rec_buffer.wptr=0;
		}
		
		data=readdata();
		
#               ifdef DEBUG
		logprintf(3,"c%lu\n",(unsigned long) data&(PULSE_BIT-1));
#               endif
		
		rec_buffer.data[rec_buffer.wptr]=data;
		rec_buffer.wptr++;
	}
	rec_buffer.rptr=0;
	
	rec_buffer.too_long=0;
	rec_buffer.is_shift=0;
	rec_buffer.pendingp=0;
	rec_buffer.pendings=0;
	rec_buffer.sum=0;
	
	return(1);
}

void rewind_rec_buffer()
{
	rec_buffer.rptr=0;
	rec_buffer.too_long=0;
	rec_buffer.pendingp=0;
	rec_buffer.pendings=0;
	rec_buffer.sum=0;
}

inline void unget_rec_buffer(int count)
{
	if(count==1 || count==2)
	{
		rec_buffer.rptr-=count;
		rec_buffer.sum-=rec_buffer.data[rec_buffer.rptr]&(PULSE_BIT-1);
		if(count==2)
		{
			rec_buffer.sum-=rec_buffer.data[rec_buffer.rptr+1]
			&(PULSE_BIT-1);
		}
	}
}

inline lirc_t get_next_pulse()
{
	lirc_t data;

	data=get_next_rec_buffer(0);
	if(data==0) return(0);
	if(!is_pulse(data))
	{
#               ifdef DEBUG
		logprintf(2,"pulse expected\n");
#               endif
		return(0);
	}
	return(data&(PULSE_BIT-1));
}

inline lirc_t get_next_space()
{
	lirc_t data;

	data=get_next_rec_buffer(0);
	if(data==0) return(0);
	if(!is_space(data))
	{
#               ifdef DEBUG
		logprintf(2,"space expected\n");
#               endif
		return(0);
	}
	return(data);
}

int expectpulse(struct ir_remote *remote,int exdelta)
{
	lirc_t deltas,deltap;
	int retval;

	if(rec_buffer.pendings>0)
	{
		deltas=get_next_space();
		if(deltas==0) return(0);
		retval=expect(remote,deltas,rec_buffer.pendings);
		if(!retval) return(0);
		rec_buffer.pendings=0;
	}
	
	deltap=get_next_pulse();
	if(deltap==0) return(0);
	if(rec_buffer.pendingp>0)
	{
		retval=expect(remote,deltap,
			      rec_buffer.pendingp+exdelta);
		if(!retval) return(0);
		rec_buffer.pendingp=0;
	}
	else
	{
		retval=expect(remote,deltap,exdelta);
	}
	return(retval);
}

int expectspace(struct ir_remote *remote,int exdelta)
{
	lirc_t deltas,deltap;
	int retval;

	if(rec_buffer.pendingp>0)
	{
		deltap=get_next_pulse();
		if(deltap==0) return(0);
		retval=expect(remote,deltap,rec_buffer.pendingp);
		if(!retval) return(0);
		rec_buffer.pendingp=0;
	}
	
	deltas=get_next_space();
	if(deltas==0) return(0);
	if(rec_buffer.pendings>0)
	{
		retval=expect(remote,deltas,
			      rec_buffer.pendings+exdelta);
		if(!retval) return(0);
		rec_buffer.pendings=0;
	}
	else
	{
		retval=expect(remote,deltas,exdelta);
	}
	return(retval);
}

inline int expectone(struct ir_remote *remote)
{
	if(is_shift(remote))
	{
		if(remote->sone>0 && !expectspace(remote,remote->sone))
		{
			unget_rec_buffer(1);
			return(0);
		}
		rec_buffer.pendingp=remote->pone;
	}
	else
	{
		if(remote->pone>0 && !expectpulse(remote,remote->pone))
		{
			unget_rec_buffer(1);
			return(0);
		}
		if(remote->ptrail>0)
		{
			if(remote->sone>0 &&
			   !expectspace(remote,remote->sone))
			{
				unget_rec_buffer(2);
				return(0);
			}
		}
		else
		{
			rec_buffer.pendings=remote->sone;
		}
	}
	return(1);
}

inline int expectzero(struct ir_remote *remote)
{
	if(is_shift(remote))
	{
		if(!expectpulse(remote,remote->pzero))
		{
			unget_rec_buffer(1);
			return(0);
		}
		rec_buffer.pendings=remote->szero;
	}
	else
	{
		if(!expectpulse(remote,remote->pzero))
		{
			unget_rec_buffer(1);
			return(0);
		}
		if(remote->ptrail>0)
		{
			if(!expectspace(remote,remote->szero))
			{
				unget_rec_buffer(2);
				return(0);
			}
		}
		else
		{
			rec_buffer.pendings=remote->szero;
		}
	}
	return(1);
}

inline lirc_t sync_rec_buffer(struct ir_remote *remote)
{
	int count;
	lirc_t deltas,deltap;
	
	count=0;
	deltas=get_next_space();
	if(deltas==0) return(0);
	
	if(last_remote!=NULL)
	{
		while(deltas<last_remote->remaining_gap*
		      (100-last_remote->eps)/100 &&
		      deltas<last_remote->remaining_gap-last_remote->aeps)
		{
			deltap=get_next_pulse();
			if(deltap==0) return(0);
			deltas=get_next_space();
			if(deltas==0) return(0);
			count++;
			if(count>REC_SYNC) /* no sync found, 
					      let's try a diffrent remote */
			{
				return(0);
			}
		}
	}
	rec_buffer.sum=0;
	return(deltas);
}

inline int get_header(struct ir_remote *remote)
{
	if(!expectpulse(remote,remote->phead))
	{
		unget_rec_buffer(1);
		return(0);
	}
	rec_buffer.pendings=remote->shead;
	return(1);
}

inline int get_foot(struct ir_remote *remote)
{
	if(!expectspace(remote,remote->sfoot)) return(0);
	if(!expectpulse(remote,remote->pfoot)) return(0);
	return(1);
}

inline int get_lead(struct ir_remote *remote)
{
	if(remote->plead==0) return(1);
	rec_buffer.pendingp=remote->plead;
	return(1);	
}

inline int get_trail(struct ir_remote *remote)
{
	if(remote->ptrail!=0)
	{
		if(!expectpulse(remote,remote->ptrail)) return(0);
	}
	if(rec_buffer.pendingp>0)
	{
		if(!expectpulse(remote,0)) return(0);
	}
	return(1);
}

inline int get_gap(struct ir_remote *remote,lirc_t gap)
{
	lirc_t data;

	data=get_next_rec_buffer(gap*(100-remote->eps)/100);
	if(data==0) return(1);
	if(!is_space(data))
	{
#               ifdef DEBUG
		logprintf(2,"space expected\n");
#               endif
		return(0);
	}
#       ifdef DEBUG
	logprintf(2,"sum: %ld\n",rec_buffer.sum);
#       endif
	if(data<gap*(100-remote->eps)/100 &&
	   data<gap-remote->aeps)
	{
#               ifdef DEBUG
		logprintf(1,"end of signal not found\n");
#               endif
		return(0);
	}
	else
	{
		unget_rec_buffer(1);
	}
	return(1);	
}

inline int get_repeat(struct ir_remote *remote)
{
	if(!get_lead(remote)) return(0);
	if(is_shift(remote))
	{
		if(!expectspace(remote,remote->srepeat)) return(0);
		if(!expectpulse(remote,remote->prepeat)) return(0);
	}
	else
	{
		if(!expectpulse(remote,remote->prepeat)) return(0);
		rec_buffer.pendings=remote->srepeat;
	}
	if(!get_trail(remote)) return(0);
	if(!get_gap(remote,
		    is_const(remote) ? 
		    (remote->gap>rec_buffer.sum ? remote->gap-rec_buffer.sum:0):
		    (has_repeat_gap(remote) ? remote->repeat_gap:remote->gap)
		    )) return(0);
	return(1);
}

ir_code get_data(struct ir_remote *remote,int bits)
{
	ir_code code;
	int i;

	code=0;

	for(i=0;i<bits;i++)
	{
		code=code<<1;
		if(expectone(remote))
		{
#                       ifdef DEBUG
			logprintf(2,"1\n");
#                       endif
			code|=1;
		}
		else if(expectzero(remote))
		{
#                       ifdef DEBUG
			logprintf(2,"0\n");
#                       endif
			code|=0;
		}
		else
		{
#                       ifdef DEBUG
			logprintf(1,"failed on bit %d\n",i+1);
#                       endif
			return((ir_code) -1);
		}
	}
	return(code);
}

ir_code get_pre(struct ir_remote *remote)
{
	ir_code pre;

	pre=get_data(remote,remote->pre_data_bits);

	if(pre==(ir_code) -1)
	{
#               ifdef DEBUG
		logprintf(1,"failed on pre_data\n");
#               endif
		return((ir_code) -1);
	}
	if(remote->pre_p>0 && remote->pre_s>0)
	{
		if(!expectpulse(remote,remote->pre_p))
			return((ir_code) -1);
		rec_buffer.pendings=remote->pre_s;
	}
	return(pre);
}

ir_code get_post(struct ir_remote *remote)
{
	ir_code post;

	if(remote->post_p>0 && remote->post_s>0)
	{
		if(!expectpulse(remote,remote->post_p))
			return((ir_code) -1);
		rec_buffer.pendings=remote->post_s;
	}

	post=get_data(remote,remote->post_data_bits);

	if(post==(ir_code) -1)
	{
#               ifdef DEBUG
		logprintf(1,"failed on post_data\n");
#               endif
		return((ir_code) -1);
	}
	return(post);
}

int default_decode(struct ir_remote *remote,
		   ir_code *prep,ir_code *codep,ir_code *postp,
		   int *repeat_flagp,lirc_t *remaining_gapp)
{
	ir_code pre,code,post,code_mask=0,post_mask=0;
	lirc_t sync;
	int header;

	sync=0; /* make compiler happy */
	code=pre=post=0;
	header=0;

	if(hw.rec_mode==LIRC_MODE_MODE2 ||
	   hw.rec_mode==LIRC_MODE_PULSE ||
	   hw.rec_mode==LIRC_MODE_RAW)
	{
		rewind_rec_buffer();
		rec_buffer.is_shift=is_shift(remote) ? 1:0;
		
		/* we should get a long space first */
		if(!(sync=sync_rec_buffer(remote)))
		{
#                       ifdef DEBUG
			logprintf(1,"failed on sync\n");
#                       endif
			return(0);
		}

#               ifdef DEBUG
		logprintf(1,"sync\n");
#               endif

		if(has_repeat(remote) && last_remote==remote)
		{
			if(remote->flags&REPEAT_HEADER && has_header(remote))
			{
				if(!get_header(remote))
				{
#                                       ifdef DEBUG
					logprintf(1,"failed on repeat "
						  "header\n");
#                                       endif
					return(0);
				}
#                               ifdef DEBUG
				logprintf(1,"repeat header\n");
#                               endif
			}
			if(get_repeat(remote))
			{
				if(remote->last_code==NULL)
				{
					logprintf(0,"repeat code without last_code "
						  "received\n");
					return(0);
				}

				*prep=remote->pre_data;
				*codep=remote->last_code->code;
				*postp=remote->post_data;
				*repeat_flagp=1;

				*remaining_gapp=
				is_const(remote) ? 
				(remote->gap>rec_buffer.sum ?
				 remote->gap-rec_buffer.sum:0):
				(has_repeat_gap(remote) ?
				 remote->repeat_gap:remote->gap);
				return(1);
			}
			else
			{
#                               ifdef DEBUG
				logprintf(1,"no repeat\n");
#                               endif
				rewind_rec_buffer();
				sync_rec_buffer(remote);
			}

		}

		if(has_header(remote))
		{
			header=1;
			if(!get_header(remote))
			{
				header=0;
				if(!(remote->flags&NO_HEAD_REP && 
				     (sync<=remote->gap+remote->gap*remote->eps/100
				      || sync<=remote->gap+remote->aeps)))
				{
#                                       ifdef DEBUG
					logprintf(1,"failed on header\n");
#                                       endif
					return(0);
				}
			}
#                       ifdef DEBUG
			logprintf(1,"header\n");
#                       endif
		}
	}

	if(is_raw(remote))
	{
		struct ir_ncode *codes,*found;
		int i;

		if(hw.rec_mode==LIRC_MODE_CODE ||
		   hw.rec_mode==LIRC_MODE_LIRCCODE)
			return(0);

		codes=remote->codes;
		found=NULL;
		while(codes->name!=NULL && found==NULL)
		{
			found=codes;
			for(i=0;i<codes->length;)
			{
				if(!expectpulse(remote,codes->signals[i++]))
				{
					found=NULL;
					rewind_rec_buffer();
					sync_rec_buffer(remote);
					break;
				}
				if(i<codes->length &&
				   !expectspace(remote,codes->signals[i++]))
				{
					found=NULL;
					rewind_rec_buffer();
					sync_rec_buffer(remote);
					break;
				}
			}
			codes++;
		}
		if(found!=NULL)
		{
			if(!get_gap(remote,
				    is_const(remote) ? 
				    remote->gap-rec_buffer.sum:
				    remote->gap)) 
				found=NULL;
		}
		if(found==NULL) return(0);
		code=found->code;
	}
	else
	{
		if(hw.rec_mode==LIRC_MODE_CODE ||
		   hw.rec_mode==LIRC_MODE_LIRCCODE)
		{
			int i;

#                       ifdef DEBUG
#                       ifdef LONG_IR_CODE
			logprintf(1,"decoded: %llx\n",rec_buffer.decoded);
#                       else
			logprintf(1,"decoded: %lx\n",rec_buffer.decoded);
#                       endif		
#                       endif
			if((hw.rec_mode==LIRC_MODE_CODE &&
			    hw.code_length<remote->pre_data_bits
			    +remote->bits+remote->post_data_bits)
			   ||
			   (hw.rec_mode==LIRC_MODE_LIRCCODE && 
			    hw.code_length!=remote->pre_data_bits
			    +remote->bits+remote->post_data_bits))
			{
				return(0);
			}
			
			for(i=0;i<remote->post_data_bits;i++)
			{
				post_mask=(post_mask<<1)+1;
			}
			post=rec_buffer.decoded&post_mask;
			post_mask=0;
			rec_buffer.decoded=
			rec_buffer.decoded>>remote->post_data_bits;
			for(i=0;i<remote->bits;i++)
			{
				code_mask=(code_mask<<1)+1;
			}
			code=rec_buffer.decoded&code_mask;
			code_mask=0;
			pre=rec_buffer.decoded>>remote->bits;
			sync=remote->gap;
		}
		else
		{
			if(!get_lead(remote))
			{
#                               ifdef DEBUG
				logprintf(1,"failed on leading pulse\n");
#                               endif
				return(0);
			}
			
			if(has_pre(remote))
			{
				pre=get_pre(remote);
				if(pre==(ir_code) -1)
				{
#                                       ifdef DEBUG
					logprintf(1,"failed on pre\n");
#                                       endif
					return(0);
				}
#                               ifdef DEBUG
#                               ifdef LONG_IR_CODE
				logprintf(1,"pre: %llx\n",pre);
#                               else
				logprintf(1,"pre: %lx\n",pre);
#                               endif
#                               endif
			}
			
			code=get_data(remote,remote->bits);
			if(code==(ir_code) -1)
			{
#                               ifdef DEBUG
				logprintf(1,"failed on code\n");
#                               endif
				return(0);
			}
#                       ifdef DEBUG
#                       ifdef LONG_IR_CODE
			logprintf(1,"code: %llx\n",code);
#                       else
			logprintf(1,"code: %lx\n",code);
#                       endif
#                       endif
			
			if(has_post(remote))
			{
				post=get_post(remote);
				if(post==(ir_code) -1)
				{
#                                       ifdef DEBUG
					logprintf(1,"failed on post\n");
#                                       endif
					return(0);
				}
#                               ifdef DEBUG
#                               ifdef LONG_IR_CODE
				logprintf(1,"post: %llx\n",post);
#                               else
				logprintf(1,"post: %lx\n",post);
#                               endif
#                               endif
			}
			if(!get_trail(remote))
			{
#                               ifdef DEBUG
				logprintf(1,"failed on trailing pulse\n");
#                               endif
				return(0);
			}
			if(has_foot(remote))
			{
				if(!get_foot(remote))
				{
#                                       ifdef DEBUG
					logprintf(1,"failed on foot\n");
#                                       endif
					return(0);
				}
			}
			if(header==1 && is_const(remote) &&
			   (remote->flags&NO_HEAD_REP))
			{
				rec_buffer.sum-=remote->phead+remote->shead;
			}
			if(!get_gap(remote,
				    is_const(remote) ? 
				    (remote->gap>rec_buffer.sum ?
				     remote->gap-rec_buffer.sum:0):
				    remote->gap)) 
				return(0);
		} /* end of mode specific code */
	}
	*prep=pre;*codep=code;*postp=post;
	if(sync<=remote->remaining_gap*(100+remote->eps)/100
	   || sync<=remote->remaining_gap+remote->aeps)
		*repeat_flagp=1;
	else
		*repeat_flagp=0;
	*remaining_gapp=is_const(remote) ?
	(remote->gap>rec_buffer.sum ? remote->gap-rec_buffer.sum:0):
	remote->gap;
	return(1);
}

/*
  interface functions
*/

int default_init()
{
#if defined(SIM_SEND) && !defined(DAEMONIZE)
	hw.fd=STDOUT_FILENO;
	hw.features=LIRC_CAN_SEND_PULSE;
	hw.send_mode=LIRC_MODE_PULSE;
	hw.rec_mode=0;
#elif defined(SIM_REC) && !defined(DAEMONIZE)
	hw.fd=STDIN_FILENO;
	hw.features=LIRC_CAN_REC_MODE2;
	hw.send_mode=0;
	hw.rec_mode=LIRC_MODE_MODE2;
#else
	struct stat s;
	int i;
	
	memset(&rec_buffer,0,sizeof(rec_buffer));
	memset(&send_buffer,0,sizeof(send_buffer));
	
	if((hw.fd=open(LIRC_DRIVER_DEVICE,O_RDWR))<0)
	{
		logprintf(0,"could not open lirc\n");
		logperror(0,"default_init()");
		return(0);
	}
	if(fstat(hw.fd,&s)==-1)
	{
		default_deinit();
		logprintf(0,"could not get file information\n");
		logperror(0,"default_init()");
		return(0);
	}
	if(S_ISFIFO(s.st_mode))
	{
#               ifdef DEBUG
		logprintf(1,"using defaults for the Irman\n");
#               endif
		hw.features=LIRC_CAN_REC_MODE2;
		hw.rec_mode=LIRC_MODE_MODE2; /* this might change in future */
		return(1);
	}
	else if(!S_ISCHR(s.st_mode))
	{
		default_deinit();
		logprintf(0,"%s is not a character device!!!\n",
			  LIRC_DRIVER_DEVICE);
		logperror(0,"something went wrong during installation\n");
		return(0);
	}
	else if(ioctl(hw.fd,LIRC_GET_FEATURES,&hw.features)==-1)
	{
		logprintf(0,"could not get hardware features\n");
		logprintf(0,"this device driver does not "
			  "support the new LIRC interface\n");
		logprintf(0,"make sure you use a current "
			  "version of the driver\n");
		default_deinit();
		return(0);
	}
#       ifdef DEBUG
	else
	{
		if(!(LIRC_CAN_SEND(hw.features) || 
		     LIRC_CAN_REC(hw.features)))
		{
			logprintf(1,"driver supports neither "
				  "sending nor receiving of IR signals\n");
		}
		if(LIRC_CAN_SEND(hw.features) && LIRC_CAN_REC(hw.features))
		{
			logprintf(1,"driver supports both sending and "
				  "receiving\n");
		}
		else if(LIRC_CAN_SEND(hw.features))
		{
			logprintf(1,"driver supports sending\n");
		}
		else if(LIRC_CAN_REC(hw.features))
		{
			logprintf(1,"driver supports receiving\n");
		}
	}
#       endif
	
	/* set send/receive method */
	hw.send_mode=0;
	if(LIRC_CAN_SEND(hw.features))
	{
		for(i=0;supported_send_modes[i]!=0;i++)
		{
			if(hw.features&supported_send_modes[i])
			{
				unsigned long mode;

				mode=LIRC_SEND2MODE(supported_send_modes[i]);
				if(ioctl(hw.fd,LIRC_SET_SEND_MODE,&mode)==-1)
				{
					logprintf(0,"could not set "
						  "send mode\n");
					logperror(0,"default_init()");
					default_deinit();
					return(0);
				}
				hw.send_mode=LIRC_SEND2MODE
				(supported_send_modes[i]);
				break;
			}
		}
		if(supported_send_modes[i]==0)
		{
			logprintf(0,"the send method of the driver is "
				  "not yet supported by lircd\n");
		}
	}
	hw.rec_mode=0;
	if(LIRC_CAN_REC(hw.features))
	{
		for(i=0;supported_rec_modes[i]!=0;i++)
		{
			if(hw.features&supported_rec_modes[i])
			{
				unsigned long mode;

				mode=LIRC_REC2MODE(supported_rec_modes[i]);
				if(ioctl(hw.fd,LIRC_SET_REC_MODE,&mode)==-1)
				{
					logprintf(0,"could not set "
						  "receive mode\n");
					logperror(0,"default_init()");
					exit(EXIT_FAILURE);
				}
				hw.rec_mode=LIRC_REC2MODE
				(supported_rec_modes[i]);
				break;
			}
		}
		if(supported_rec_modes[i]==0)
		{
			logprintf(0,"the receive method of the driver "
				  "is not yet supported by lircd\n");
		}
	}
	if(hw.rec_mode==LIRC_MODE_CODE)
	{
		hw.code_length=8;
	}
	else if(hw.rec_mode==LIRC_MODE_LIRCCODE)
	{
		if(ioctl(hw.fd,LIRC_GET_LENGTH,&hw.code_length)==-1)
		{
			logprintf(0,"could not get code length\n");
			logperror(0,"default_init()");
			default_deinit();
			return(0);
		}
		if(hw.code_length>sizeof(ir_code)*CHAR_BIT)
		{
			logprintf(0,"lircd can not handle %lu bit "
				  "codes\n",hw.code_length);
			default_deinit();
			return(0);
		}
	}
	if(!(hw.send_mode || hw.rec_mode))
	{
		default_deinit();
		return(0);
	}
#endif
	return(1);
}

int default_deinit(void)
{
#if (!defined(SIM_SEND) || !defined(SIM_SEND)) || defined(DAEMONIZE)
	close(hw.fd);
#endif
	return(1);
}

int write_send_buffer(int lirc,int length,lirc_t *signals)
{
#if defined(SIM_SEND) && !defined(DAEMONIZE)
	int i;

	if(send_buffer.wptr==0 && length>0 && signals!=NULL)
	{
		for(i=0;;)
		{
			printf("pulse %lu\n",(unsigned long) signals[i++]);
			if(i>=length) break;
			printf("space %lu\n",(unsigned long) signals[i++]);
		}
		return(length*sizeof(lirc_t));
	}
	
	if(send_buffer.wptr==0) 
	{
#               ifdef DEBUG
		logprintf(1,"nothing to send\n");
#               endif
		return(0);
	}
	for(i=0;;)
	{
		printf("pulse %lu\n",(unsigned long) send_buffer.data[i++]);
		if(i>=send_buffer.wptr) break;
		printf("space %lu\n",(unsigned long) send_buffer.data[i++]);
	}
	return(send_buffer.wptr*sizeof(lirc_t));
#else
	if(send_buffer.wptr==0 && length>0 && signals!=NULL)
	{
		return(write(lirc,signals,length*sizeof(lirc_t)));
	}
	
	if(send_buffer.wptr==0) 
	{
#               ifdef DEBUG
		logprintf(1,"nothing to send\n");
#               endif
		return(0);
	}
	return(write(lirc,send_buffer.data,
		     send_buffer.wptr*sizeof(lirc_t)));
#endif
}

int default_send(struct ir_remote *remote,struct ir_ncode *code)
{
	lirc_t remaining_gap;

	/* things are easy, because we only support one mode */
	if(hw.send_mode!=LIRC_MODE_PULSE)
		return(0);

#if !defined(SIM_SEND) || defined(DAEMONIZE)
	if(hw.features&LIRC_CAN_SET_SEND_CARRIER)
	{
		unsigned long freq;
		
		freq=remote->freq==0 ? 38000:remote->freq;
		if(ioctl(hw.fd,LIRC_SET_SEND_CARRIER,&freq)==-1)
		{
			logprintf(0,"could not set modulation frequency\n");
			logperror(0,NULL);
			return(0);
		}
	}
#endif
	remaining_gap=remote->remaining_gap;
	if(!init_send(remote,code)) return(0);
	
#if !defined(SIM_SEND) || defined(DAEMONIZE)
	if(remote->last_code!=NULL)
	{
		struct timeval current;
		unsigned long usecs;
		
		gettimeofday(&current,NULL);
		usecs=time_left(&current,&remote->last_send,remaining_gap);
		if(usecs>0) usleep(usecs);
	}
#endif

	if(write_send_buffer(hw.fd,code->length,code->signals)==-1)
	{
		logprintf(0,"write failed\n");
		logperror(0,NULL);
		return(0);
	}
	else
	{
		gettimeofday(&remote->last_send,NULL);
		remote->last_code=code;
#if defined(SIM_SEND) && !defined(DAEMONIZE)
		printf("space %lu\n",(unsigned long) remote->remaining_gap);
#endif
	}
	return(1);
}

char *default_rec(struct ir_remote *remotes)
{
	char c;
	int n;
	static char message[PACKET_SIZE+1];


	if(hw.rec_mode==LIRC_MODE_STRING)
	{
		int failed=0;

		/* inefficient but simple, fix this if you want */
		n=0;
		do
		{
			if(read(hw.fd,&c,1)!=1)
			{
				logprintf(0,"reading in mode LIRC_MODE_STRING "
					  "failed\n");
				return(NULL);
			}
			if(n>=PACKET_SIZE-1)
			{
				failed=1;
				n=0;
			}
			message[n++]=c;
		}
		while(c!='\n');
		message[n]=0;
		if(failed) return(NULL);
		return(message);
	}
	else
	{
		if(!clear_rec_buffer()) return(NULL);
		return(decode_all(remotes));
	}
}

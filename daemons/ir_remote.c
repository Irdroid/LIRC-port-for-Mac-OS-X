/*      $Id: ir_remote.c,v 5.10 1999/08/13 18:57:57 columbus Exp $      */

/****************************************************************************
 ** ir_remote.c *************************************************************
 ****************************************************************************
 *
 * ir_remote.c - sends and decodes the signals from IR remotes
 * 
 * Copyright (C) 1996,97 Ralph Metzler (rjkm@thp.uni-koeln.de)
 * Copyright (C) 1998 Christoph Bartelmus (columbus@hit.handshake.de)
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>

#include <sys/ioctl.h>

#include "drivers/lirc.h"

#include "lircd.h"
#include "ir_remote.h"
#include "hardware.h"

struct ir_remote *decoding=NULL;

struct ir_remote *last_remote=NULL;
struct ir_remote *repeat_remote=NULL;
struct ir_ncode *repeat_code;

extern struct hardware hw;

struct ir_remote *get_ir_remote(struct ir_remote *remotes,char *name)
{
	struct ir_remote *all;

	/* use remotes carefully, it may be changed on SIGHUP */
	all=remotes;
	while(all)
	{
		if(strcasecmp(all->name,name)==0)
		{
			return(all);
		}
		all=all->next;
	}
	return(NULL);
}

struct ir_ncode *get_ir_code(struct ir_remote *remote,char *name)
{
	struct ir_ncode *all;

	all=remote->codes;
	while(all->name!=NULL)
	{
		if(strcasecmp(all->name,name)==0)
		{
			return(all);
		}
		all++;
	}
	return(0);
}

struct ir_ncode *get_code(struct ir_remote *remote,
			  ir_code pre,ir_code code,ir_code post,
			  int *repeat_statep)
{
	ir_code pre_mask,code_mask,post_mask;
	int repeat_state;
	struct ir_ncode *codes,*found;
	
	pre_mask=code_mask=post_mask=0;
	repeat_state=0;
	if(remote->repeat_bit>0)
	{
		if(remote->repeat_bit<=remote->pre_data_bits)
		{
			repeat_state=
			pre&(1<<(remote->pre_data_bits
				 -remote->repeat_bit)) ? 1:0;
			pre_mask=1<<(remote->pre_data_bits
				     -remote->repeat_bit);
		}
		else if(remote->repeat_bit<=remote->pre_data_bits
			+remote->bits)
		{
			repeat_state=
			code&(1<<(remote->pre_data_bits
				  +remote->bits
				  -remote->repeat_bit)) ? 1:0;
			code_mask=1<<(remote->pre_data_bits
				      +remote->bits
				      -remote->repeat_bit);
		}
		else if(remote->repeat_bit<=remote->pre_data_bits
			+remote->bits
			+remote->post_data_bits)
		{
			repeat_state=
			post&(1<<(remote->pre_data_bits
				  +remote->bits
				  +remote->post_data_bits
				  -remote->repeat_bit)) ? 1:0;
			post_mask=1<<(remote->pre_data_bits
				      +remote->bits
				      +remote->post_data_bits
				      -remote->repeat_bit);
		}
		else
		{
			logprintf(0,"bad repeat_bit\n");
		}
	}
	if(has_pre(remote))
	{
		if((pre|pre_mask)!=(remote->pre_data|pre_mask))
		{
#                       ifdef DEBUG
			logprintf(1,"bad pre data\n");
#                       endif
#                       ifdef DEBUG
#                       ifdef LONG_IR_CODE
			logprintf(2,"%llx %llx\n",pre,remote->pre_data);
#                       else
			logprintf(2,"%lx %lx\n",pre,remote->pre_data);
#                       endif
#                       endif
			return(0);
		}
#               ifdef DEBUG
		logprintf(1,"pre\n");
#               endif
	}
	
	if(has_post(remote))
	{
		if((post|post_mask)!=(remote->post_data|post_mask))
		{
#                       ifdef DEBUG
			logprintf(1,"bad post data\n");
#                       endif
			return(0);
		}
#               ifdef DEBUG
		logprintf(1,"post\n");
#               endif
	}
	found=NULL;
	codes=remote->codes;
	if(codes!=NULL)
	{
		while(codes->name!=NULL)
		{
			if((codes->code|code_mask)==(code|code_mask))
			{
				found=codes;
				break;
			}
			codes++;
		}
	}
	*repeat_statep=repeat_state;
	return(found);
}

inline unsigned long time_elapsed(struct timeval *last,
				  struct timeval *current)
{
	unsigned long secs,usecs,diff;
	
	secs=current->tv_sec-last->tv_sec;
	usecs=current->tv_usec-last->tv_usec;
	
	diff=1000000*secs+usecs;
	
	return(diff);
}

unsigned long long set_code(struct ir_remote *remote,struct ir_ncode *found,
			    int repeat_state,int repeat_flag,
			    unsigned long remaining_gap)
{
	unsigned long long code;
	struct timeval current;

#       ifdef DEBUG
	logprintf(1,"found: %s\n",found->name);
#       endif

	gettimeofday(&current,NULL);
	if(found==remote->last_code && repeat_flag &&
	   time_elapsed(&remote->last_send,&current)<1000000 &&
	   (!(remote->repeat_bit>0) || repeat_state==remote->repeat_state))
	{
		remote->reps++;
	}
	else
	{
		remote->reps=0;
		remote->last_code=found;
		if(remote->repeat_bit>0)
		{
			remote->repeat_state=repeat_state;
		}
	}
	last_remote=remote;
	remote->last_send=current;
	remote->remaining_gap=remaining_gap;
	
	code=0;
	if(has_pre(remote))
	{
		code|=remote->pre_data;
		code=code<<remote->bits;
	}
	code|=remote->last_code->code;
	if(has_post(remote))
	{
		code=code<<remote->post_data_bits;
		code|=remote->post_data;
	}
	if(remote->flags&REVERSE)
	{
		code=reverse(code,remote->pre_data_bits+
			     remote->bits+remote->post_data_bits);
	}
	return(code);
}

char *decode_all(struct ir_remote *remotes)
{
	struct ir_remote *remote;
	static char message[PACKET_SIZE+1];
	ir_code pre,code,post;
	struct ir_ncode *ncode;
	int repeat_flag,repeat_state;
	unsigned long remaining_gap;

	/* use remotes carefully, it may be changed on SIGHUP */
	decoding=remote=remotes;
	while(remote)
	{
#               ifdef DEBUG
		logprintf(1,"trying \"%s\" remote\n",remote->name);
#               endif
		
		if(hw.decode_func(remote,&pre,&code,&post,&repeat_flag,
				   &remaining_gap) &&
		   (ncode=get_code(remote,pre,code,post,&repeat_state)))
		{
			int len;

			code=set_code(remote,ncode,repeat_state,repeat_flag,
				      remaining_gap);
#ifdef __GLIBC__
			/* It seems you can't print 64-bit longs on glibc */
			
			len=snprintf(message,PACKET_SIZE+1,"%08lx%08lx %02x %s %s\n",
				     (unsigned long)
				     (code>>32),
				     (unsigned long)
				     (code&0xFFFFFFFF),
				     remote->reps,
				     remote->last_code->name,
				     remote->name);
#else
			len=snprintf(message,PACKET_SIZE,"%016llx %02x %s %s\n",
				     code,
				     remote->reps,
				     remote->last_code->name,
				     remote->name);
#endif
			decoding=NULL;
			if(len==PACKET_SIZE+1)
			{
				logprintf(0,"message buffer overflow\n");
				return(NULL);
			}
			else
			{
				return(message);
			}
		}
		else
		{
#                       ifdef DEBUG
			logprintf(1,"failed \"%s\" remote\n",remote->name);
#                       endif
		}
		remote=remote->next;
	}
	decoding=NULL;
	last_remote=NULL;
#       ifdef DEBUG
	logprintf(1,"decoding failed for all remotes\n");
#       endif DEBUG
	return(NULL);
}

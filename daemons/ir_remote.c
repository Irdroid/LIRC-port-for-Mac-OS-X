/*      $Id: ir_remote.c,v 5.23 2003/09/21 10:15:02 lirc Exp $      */

/****************************************************************************
 ** ir_remote.c *************************************************************
 ****************************************************************************
 *
 * ir_remote.c - sends and decodes the signals from IR remotes
 * 
 * Copyright (C) 1996,97 Ralph Metzler (rjkm@thp.uni-koeln.de)
 * Copyright (C) 1998 Christoph Bartelmus (lirc@bartelmus.de)
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

void get_frequency_range(struct ir_remote *remotes,
			 unsigned int *min_freq,unsigned int *max_freq)
{
	struct ir_remote *scan;
	
	/* use remotes carefully, it may be changed on SIGHUP */
	scan=remotes;
	if(scan==NULL)
	{
		*min_freq=DEFAULT_FREQ;
		*max_freq=DEFAULT_FREQ;
	}
	else
	{
		*min_freq=scan->freq;
		*max_freq=scan->freq;
		scan=scan->next;
	}
	while(scan)
	{
		if(scan->freq!=0)
		{
			if(scan->freq>*max_freq)
			{
				*max_freq=scan->freq;
			}
			else if(scan->freq<*min_freq)
			{
				*min_freq=scan->freq;
			}
		}
		scan=scan->next;
	}
}

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
	int repeat_state, found_code;
	struct ir_ncode *codes,*found;
	
	pre_mask=code_mask=post_mask=0;
	repeat_state=0;
	if(remote->toggle_bit>0)
	{
		if(remote->toggle_bit<=remote->pre_data_bits)
		{
			repeat_state=
			pre&(1<<(remote->pre_data_bits
				 -remote->toggle_bit)) ? 1:0;
			pre_mask=1<<(remote->pre_data_bits
				     -remote->toggle_bit);
		}
		else if(remote->toggle_bit<=remote->pre_data_bits
			+remote->bits)
		{
			repeat_state=
			code&(1<<(remote->pre_data_bits
				  +remote->bits
				  -remote->toggle_bit)) ? 1:0;
			code_mask=1<<(remote->pre_data_bits
				      +remote->bits
				      -remote->toggle_bit);
		}
		else if(remote->toggle_bit<=remote->pre_data_bits
			+remote->bits
			+remote->post_data_bits)
		{
			repeat_state=
			post&(1<<(remote->pre_data_bits
				  +remote->bits
				  +remote->post_data_bits
				  -remote->toggle_bit)) ? 1:0;
			post_mask=1<<(remote->pre_data_bits
				      +remote->bits
				      +remote->post_data_bits
				      -remote->toggle_bit);
		}
		else
		{
			logprintf(LOG_ERR,"bad toggle_bit");
		}
	}
	if(has_toggle_mask(remote) && remote->toggle_mask_state%2)
	{
		ir_code *affected,mask,mask_bit;
		int bit,current_bit;
		
		affected=&post;
		mask=remote->toggle_mask;
		for(bit=current_bit=0;bit<remote->pre_data_bits+
			    remote->bits+
			    remote->post_data_bits;bit++,current_bit++)
		{
			if(bit==remote->post_data_bits)
			{
				affected=&code;
				current_bit=0;
			}
			if(bit==remote->post_data_bits+remote->bits)
			{
				affected=&pre;
				current_bit=0;
			}
			mask_bit=mask&1;
			(*affected)^=(mask_bit<<current_bit);
			mask>>=1;
		}
	}
	if(has_pre(remote))
	{
		if((pre|pre_mask)!=(remote->pre_data|pre_mask))
		{
			LOGPRINTF(1,"bad pre data");
#                       ifdef LONG_IR_CODE
			LOGPRINTF(2,"%llx %llx",pre,remote->pre_data);
#                       else
			LOGPRINTF(2,"%lx %lx",pre,remote->pre_data);
#                       endif
			return(0);
		}
		LOGPRINTF(1,"pre");
	}
	
	if(has_post(remote))
	{
		if((post|post_mask)!=(remote->post_data|post_mask))
		{
			LOGPRINTF(1,"bad post data");
#                       ifdef LONG_IR_CODE
			LOGPRINTF(2,"%llx %llx",post,remote->post_data);
#                       else
			LOGPRINTF(2,"%lx %lx",post,remote->post_data);
#                       endif
			return(0);
		}
		LOGPRINTF(1,"post");
	}
	found=NULL;
	found_code=0;
	codes=remote->codes;
	if(codes!=NULL)
	{
		while(codes->name!=NULL)
		{
			ir_code next_code;
			
			if(codes->next!=NULL && codes->current!=NULL)
			{
				next_code=codes->current->code;
			}
			else
			{
				next_code=codes->code;
			}
			if((next_code|code_mask)==(code|code_mask))
			{
				found_code=1;
				if(codes->next!=NULL)
				{
					if(codes->current==NULL)
					{
						codes->current=codes->next;
					}
					else
					{
						codes->current=
							codes->next->next;
					}
				}
				found=codes;
				if(codes->current==NULL)
				{
					break;
				}
			}
			else
			{
				codes->current=NULL;
			}
			codes++;
		}
	}
#       ifdef DYNCODES
	if(!found_code)
	{
		if((remote->dyncodes[remote->dyncode].code|code_mask)!=
		   (code|code_mask))
		{
			remote->dyncode++;
			remote->dyncode%=2;
		}
		remote->dyncodes[remote->dyncode].code=code&(~code_mask);
		found=&(remote->dyncodes[remote->dyncode]);
		found_code=1;
	}
#       endif
	if(found_code && found!=NULL && has_toggle_mask(remote))
	{
		if(!(remote->toggle_mask_state%2))
		{
			remote->toggle_code=found;
			LOGPRINTF(1,"toggle_mask_start");
		}
		else
		{
			if(found!=remote->toggle_code)
			{
				remote->toggle_code=NULL;
				return(NULL);
			}
			remote->toggle_code=NULL;
		}
	}
	*repeat_statep=repeat_state;
	return(found);
}

unsigned long long set_code(struct ir_remote *remote,struct ir_ncode *found,
			    int repeat_state,int repeat_flag,
			    lirc_t remaining_gap)
{
	unsigned long long code;
	struct timeval current;

	LOGPRINTF(1,"found: %s",found->name);

	gettimeofday(&current,NULL);
	if(remote==last_remote &&
	   ((found->next==NULL && found==remote->last_code) || 
	    (found->next!=NULL)) &&
	   repeat_flag &&
	   time_elapsed(&remote->last_send,&current)<1000000 &&
	   (!(remote->toggle_bit>0) || repeat_state==remote->repeat_state))
	{
		if(has_toggle_mask(remote))
		{
			remote->toggle_mask_state++;
			if(remote->toggle_mask_state==4)
			{
				remote->reps++;
				remote->toggle_mask_state=2;
			}
		}
		else if(found->current==NULL)
		{
			remote->reps++;
		}
	}
	else
	{
		if(found->next!=NULL && found->current==NULL)
		{
			remote->reps=1;
		}
		else
		{
			remote->reps=0;
		}
		if(has_toggle_mask(remote))
		{
			remote->toggle_mask_state=1;
			remote->toggle_code=found;
		}
		if(remote->toggle_bit>0)
		{
			remote->repeat_state=repeat_state;
		}
	}
	last_remote=remote;
	remote->last_code=found;
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
	if(remote->flags&COMPAT_REVERSE)
	{
		/* actually this is wrong: pre, code and post should
		   be rotated separately but we have to stay
		   compatible with older software
		 */
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
	lirc_t remaining_gap;
	
	/* use remotes carefully, it may be changed on SIGHUP */
	decoding=remote=remotes;
	while(remote)
	{
		LOGPRINTF(1,"trying \"%s\" remote",remote->name);
		
		if(hw.decode_func(remote,&pre,&code,&post,&repeat_flag,
				   &remaining_gap) &&
		   (ncode=get_code(remote,pre,code,post,&repeat_state)))
		{
			int len;

			code=set_code(remote,ncode,repeat_state,repeat_flag,
				      remaining_gap);
			if((has_toggle_mask(remote) &&
			    remote->toggle_mask_state%2) ||
			   ncode->current!=NULL)
			{
				decoding=NULL;
				return(NULL);
			}

#ifdef __GLIBC__
			/* It seems you can't print 64-bit longs on glibc */
			
			len=snprintf(message,PACKET_SIZE+1,"%08lx%08lx %02x %s %s\n",
				     (unsigned long)
				     (code>>32),
				     (unsigned long)
				     (code&0xFFFFFFFF),
				     remote->reps-(ncode->next ? 1:0),
				     remote->last_code->name,
				     remote->name);
#else
			len=snprintf(message,PACKET_SIZE,"%016llx %02x %s %s\n",
				     code,
				     remote->reps-(ncode->next ? 1:0),
				     remote->last_code->name,
				     remote->name);
#endif
			decoding=NULL;
			if(len==PACKET_SIZE+1)
			{
				logprintf(LOG_ERR,"message buffer overflow");
				return(NULL);
			}
			else
			{
				return(message);
			}
		}
		else
		{
			LOGPRINTF(1,"failed \"%s\" remote",remote->name);
		}
		remote->toggle_mask_state=0;
		remote=remote->next;
	}
	decoding=NULL;
	last_remote=NULL;
	LOGPRINTF(1,"decoding failed for all remotes");
	return(NULL);
}

/*      $Id: irrecord.c,v 1.1 1999/04/29 21:17:21 columbus Exp $      */

/***************************************************************************
 ** This file is part of the lirc-0.5.4 package ****************************
 ** LIRC - Linux Infrared Remote Control ***********************************
 ***************************************************************************
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


/****************************************************************************
 ** irrecord.c **************************************************************
 ****************************************************************************
 *
 * irrecord -  application for recording IR-codes for usage with lircd
 *
 * Copyright (C) 1998,99 Christoph Bartelmus <columbus@hit.handshake.de>
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <limits.h>

#include "daemons/dump_config.h"
#include "daemons/ir_remote.h"
#include "daemons/config_file.h"

#define IRRECORD_VERSION "0.3"
#define BUTTON 80+1

#define min(a,b) (a>b ? b:a)
#define max(a,b) (a>b ? a:b)


/* the longest signal I've seen up to now was 48-bit signal with header */

#define MAX_SIGNALS 200
#define AEPS 100
#define EPS 30

struct lengths
{
	unsigned int count;
	unsigned long sum,upper_bound,lower_bound,min,max;
	struct lengths *next;
};

int lirc;

struct ir_remote remote;
struct ir_ncode code;
char *progname;

extern struct rbuf rec_buffer;

int analyse(struct ir_remote *remotes);
int get_codes(struct ir_remote *remote);
void get_pre_data(struct ir_remote *remote);
void get_post_data(struct ir_remote *remote);

int main(int argc,char **argv)
{
	fd_set fds;
	struct timeval timeout;
	char *filename;
	FILE *fout,*fin;
	struct ir_remote *remotes;
	int ret;
	unsigned long data,average,repeat[3],signals[MAX_SIGNALS+1];
	int count,mode;

	progname=argv[0];
	return_code=1;

	while(1)
	{
		int c;
		static struct option long_options[] =
		{
			{"help",no_argument,NULL,'h'},
			{"version",no_argument,NULL,'v'},
			{0, 0, 0, 0}
		};
		c = getopt_long(argc,argv,"hv",long_options,NULL);
		if(c==-1)
			break;
		switch (c)
		{
		case 'h':
			printf("Usage: %s file\n",progname);
			printf("   or: %s [options]\n",progname);
			printf("\t -h --help\t\tdisplay this message\n");
			printf("\t -v --version\t\tdisplay version\n");
			return(EXIT_SUCCESS);
		case 'v':
			printf("irrecord %s\n",IRRECORD_VERSION);
			return(EXIT_SUCCESS);
		default:
			return(EXIT_FAILURE);
		}
	}
	if(optind+1!=argc)
	{
		fprintf(stderr,"%s: invalid argument count\n",progname);
		return(EXIT_FAILURE);
	}
	filename=argv[optind];
	fin=fopen(filename,"r");
	if(fin!=NULL)
	{
		remotes=read_config(fin);
		fclose(fin);
		if(remotes==NULL)
		{
			fprintf(stderr,"%s: config file contains no valid "
				"remote control definition\n",progname);
			return(EXIT_FAILURE);
		}
		if(remotes==(void *) -1)
		{
			fprintf(stderr,"%s: reading of config file failed\n",
				progname);
			return(EXIT_FAILURE);
		}
		if(-1==analyse(remotes))
		{
			fprintf(stderr,"%s: conversion failed\n",progname);
			return(EXIT_FAILURE);
		}
		fprint_remotes(stdout,remotes);
		free_config(remotes);
		return(EXIT_SUCCESS);
	}
	fout=fopen(filename,"w");
	if(fout==NULL)
	{
		fprintf(stderr,"%s: could not open file %s\n",progname,
			filename);
		perror(progname);
		return(EXIT_FAILURE);
	}
	printf("record -  application for recording IR-codes"
	       " for usage with lirc\n"
	       "\n"  
	       "Copyright (C) 1998 Christoph Bartelmus"
	       "(columbus@hit.handshake.de)\n");
	printf("\n\n");
	
	lirc=open("/dev/lirc",O_RDONLY|O_NONBLOCK);
	if(lirc==-1)
	{
		fprintf(stderr,"%s: could not open /dev/lirc\n",progname);
		perror(progname);
		fclose(fout);
		unlink(filename);
		return(EXIT_FAILURE);
	}

	printf("This program will record the signals from your "
	       "remote control\n"
	       "and create a config file for lircd\n\n"
	       "Please send new config files to: "
	       "columbus@hit.handshake.de\n\n");

	remote.flags=RAW_CODES;
	remote.aeps=AEPS;
	remote.eps=EPS;                 /* should work with all remotes */
	remote.name=filename;

	while(read(lirc,&data,sizeof(unsigned long))==sizeof(unsigned long));

	printf("First, please hold down an arbitrary button\n");

	mode=count=0;
	average=0;
	while(1)
	{
		FD_ZERO(&fds);
		FD_SET(lirc,&fds);
		/* timeout after 10 secs */
		timeout.tv_sec=10;
		timeout.tv_usec=0;
		ret=select(lirc+1,&fds,NULL,NULL,&timeout);
		if(ret==-1)
		{
			fprintf(stderr,"%s: select() failed\n",progname);
			perror(progname);
			close(lirc);
			fclose(fout);
			unlink(filename);
			exit(EXIT_FAILURE);
		}
		else if(ret==0)
		{
			fprintf(stderr,"%s: no data for 10 secs, aborting\n",
				progname);
			close(lirc);
			fclose(fout);
			unlink(filename);
			exit(EXIT_FAILURE);
		}

		ret=read(lirc,&data,sizeof(unsigned long));
		if(ret!=sizeof(unsigned long))
		{
			fprintf(stderr,"%s: read() failed\n",progname);
			perror(progname);
			close(lirc);
			fclose(fout);
			unlink(filename);
			exit(EXIT_FAILURE);
		}
		if(is_space(data)) /* currently I am only interested in 
				      spaces */
		{
			if(mode==0)
			{
				if(average==0)
				{
					if(data>100000) continue;
					average=data;
				}
				else
				{
					if(data>10*average ||
					   /* this MUST be a gap */
					   (count>10 && data>5*average))  
						/* this should be a gap */
					{
						remote.gap=data;
						mode=1;
					}
					average=(average*count+data)/(count+1);
				}
				if(mode==1)
				{
					average=0;
					count=0;
					printf("\nFound gap: %ld\n",
					       remote.gap);
					printf("Please wait a moment...\n");
					sleep(3);
					while(read(lirc,&data,
						   sizeof(unsigned long))
					      ==sizeof(unsigned long));
					printf("\nNow, please hold down an"
					       " arbitrary button again to"
					       " confirm this value.\n");
					continue;
				}
			}
			else if(mode==1)
			{
				if(count>MAX_SIGNALS)
				{
					printf("Failed to confirm value.\n");
					printf("Please try again.\n");
					close(lirc);
					fclose(fout);
					unlink(filename);
					exit(EXIT_FAILURE);
				}
				if(average==0)
				{
					if(data>100000) continue;
					average=data;
				}
				else
				{
					
					if(data>10*average ||
					   /* this MUST be a gap */
					   (count>10 && data>5*average))  
						/* this should be a gap */
					{
						if(expect(&remote,data,
							  remote.gap))
						{
							/* Succeded to
							   confirm value */
							count=0;
							mode=2;
							continue;
						}
					}
					average=(average*count+data)/(count+1);
				}
			}
		}
		if(mode==2)
		{
			if(count<3)
			{
				repeat[count]=data&(PULSE_BIT-1);
			}
			else
			{
				if(expect(&remote,data,remote.gap))
				{
					printf("Found repeat code: %ld %ld "
					       "%ld\n",
					       repeat[0],repeat[1],repeat[2]);
					remote.prepeat=repeat[0];
					remote.srepeat=repeat[1];
					remote.ptrail=repeat[2];
				}
				break;
			}
		}
		count++;
	}
	printf("\nSucceded to confirm value.\n");
	printf("Now enter the names for the buttons.\n");

	fprint_comment(fout,&remote);
	fprint_remote_head(fout,&remote);
	fprint_remote_signal_head(fout,&remote);
	
	while(1)
	{
		char buffer[BUTTON];
		char *string;

		printf("\nPlease enter the name for the next button\n");
		string=fgets(buffer,BUTTON,stdin);
		
		if(string!=buffer)
		{
			fprintf(stderr,"%s: fgets() failed\n",progname);
			close(lirc);
			fclose(fout);
			return(EXIT_FAILURE);
		}
		buffer[strlen(buffer)-1]=0;
		if(strchr(buffer,' ') || strchr(buffer,'\t'))
		{
			printf("The name must not contain any whitespace.\n");
			printf("Please try again.\n");
			continue;
		}
		if(strcasecmp(buffer,"begin")==0 
		   || strcasecmp(buffer,"end")==0)
		{
			printf("'%s' is not allowed as button name\n",buffer);
			printf("Please try again.\n");
			continue;
		}
		if(buffer[0]=='#')
		{
			printf("The name must not start with '#'.\n");
			printf("Please try again.\n");
			continue;			
		}
		if(strlen(buffer)==0)
		{
			break;
		}
		
		while(read(lirc,&data,sizeof(unsigned long))
		      ==sizeof(unsigned long));
		printf("\nNow press button \"%s\".\n",buffer);

		count=0;
		while(count<MAX_SIGNALS)
		{
			FD_ZERO(&fds);
			FD_SET(lirc,&fds);
			/* timeout after 10 secs */
			if(count==0)
			{
				timeout.tv_sec=10;
				timeout.tv_usec=0;
			}
			else
			{
				timeout.tv_sec=0;
				timeout.tv_usec=remote.gap*10
				+remote.gap*remote.eps/100;
			}
			ret=select(lirc+1,&fds,NULL,NULL,&timeout);
			if(ret==-1)
			{
				fprintf(stderr,"%s: select() failed\n",
					progname);
				perror(progname);
				close(lirc);
				fclose(fout);
				exit(EXIT_FAILURE);
			}
			if(ret==0)
			{
				if(count==0)
				{
					printf("%s: no data for 10 secs, "
					       "aborting\n",progname);
					fprint_remote_signal_foot(fout,
								  &remote);
					fprint_remote_foot(fout,&remote);
					
					close(lirc);
					fclose(fout);
					return(EXIT_SUCCESS);
				}
				data=remote.gap;
			}
			else
			{
				ret=read(lirc,&data,sizeof(unsigned long));
				if(ret!=sizeof(unsigned long))
				{
					fprintf(stderr,"%s: read() failed\n",
						progname);
					perror(progname);

					fprint_remote_signal_foot(fout,
								  &remote);
					fprint_remote_foot(fout,&remote);
					close(lirc);
					fclose(fout);
					exit(EXIT_FAILURE);
				}
			}

			if(count==0)
			{
				if(!is_space(data) ||
				   data<remote.gap-remote.gap*remote.eps/100)
				{
					printf("Sorry, something went wrong."
					       "\n");
					printf("Try again.\n");
					break;
				}
			}
			else
			{
				if(is_space(data) && data>
				   remote.gap-remote.gap*remote.eps/100)
				{
					printf("Got it.\n");
					printf("Signal length is %d\n",
					       count-1);
					if(count%2)
					{
						printf("That's weired because "
						       "the signal length "
						       "must be odd!\n");
						printf("Try again.\n");
						count=0;
						continue;
					}
					else
					{
						code.name=buffer;
						code.length=count-1;
						code.signals=signals;
						fprint_remote_signal(fout,
								     &remote,
								     &code);
						break;
					}
				}
				signals[count-1]=data&(PULSE_BIT-1);
			}
			count++;
		}
		if(count==MAX_SIGNALS)
		{
			printf("Signal is too long.\n");
		}
	}
	fprint_remote_signal_foot(fout,&remote);
	fprint_remote_foot(fout,&remote);

	close(lirc);
	fclose(fout);
	return(EXIT_SUCCESS);
}

void logprintf(char *format_str, ...)
{
#if 0
	time_t current;
	char *currents;
	va_list ap;  

	current=time(&current);
	currents=ctime(&current);
		    
	fprintf(stderr,"%15.15s: ",currents+4);
	va_start(ap,format_str);
	vfprintf(stderr,format_str,ap);
#ifndef DAEMONIZE
	vfprintf(stderr,format_str,ap);
#endif DAEMONIZE
	va_end(ap);
	fflush(stderr);
#endif
}

void logperror(const char *s)
{
#if 0
	const char *errmsg;

	if(errno<sys_nerr)
	{
		errmsg=sys_errlist[errno];
	}
	else
	{
		errmsg="unknown error";
	}
		
	if(s!=NULL)
	{
		logprintf("%s: %s\n",s,errmsg);
	}
	else
	{
		logprintf("%s\n",errmsg);
	}
#endif
}

int analyse(struct ir_remote *remotes)
{
	int get_scheme(struct ir_remote *remote);
	int check_lengths(struct ir_remote *remote);
	int get_lengths(struct ir_remote *remote);

	int scheme;

	while(remotes!=NULL)
	{
		if(remotes->flags&RAW_CODES)
		{
			scheme=get_scheme(remotes);
			switch(scheme)
			{
			case 0:
				fprintf(stderr,"%s: config file does not "
					"contain any buttons\n",
					progname);
				return(-1);
				break;
			case SPACE_ENC:
				if(-1==check_lengths(remotes))
				{
					return(-1);
				}
				if(-1==get_lengths(remotes))
				{
					return(-1);
				}
				remotes->flags&=~RAW_CODES;
				remotes->flags|=SPACE_ENC;
				if(-1==get_codes(remotes))
				{
					remotes->flags&=~SPACE_ENC;
					remotes->flags|=RAW_CODES;
					return(-1);
				}
				get_pre_data(remotes);
				get_post_data(remotes);
				break;
			case SHIFT_ENC:
				fprintf(stderr,"%s: shift encoded remote "
					"controls are not supported yet\n",
					progname);
				return(-1);
				break;
			}
		}
		remotes=remotes->next;
	}
	return(0);
}

int get_sum(struct ir_remote *remote)
{
	int sum,i;
	struct ir_ncode *codes;

	sum=0;
	codes=remote->codes;
	for(i=0;codes[i].name!=NULL;i++)
	{
		sum++;
	}
	return(sum);
}

int get_scheme(struct ir_remote *remote)
{
	struct ir_ncode *codes;
	int match,sum,i,j;

	sum=get_sum(remote);
	if(sum==0)
	{
		return(0);
	}
	codes=remote->codes;
	for(i=0;i<sum;i++)
	{
		match=0;
		for(j=i+1;j<sum;j++)
		{
			if(codes[i].length==codes[j].length)
			{
				match++;
				/* I want less than 20% mismatches */
				if(match>=80*sum/100) 
				{
					/* this is not yet the
					   number of bits */
					remote->bits=codes[i].length;
					return(SPACE_ENC);
				}
			}
		}
	}
	return(SHIFT_ENC);
}

int check_lengths(struct ir_remote *remote)
{
	int i,flag;
	struct ir_ncode *codes;

	flag=0;
	codes=remote->codes;
	for(i=0;codes[i].name!=NULL;i++)
	{
		if(codes[i].length!=remote->bits)
		{
			fprintf(stderr,"%s: button \"%s\" has wrong signal "
				"length\n"
				"%s: try to record it again\n",
				progname,codes[i].name,progname);
			flag=-1;
		}
	}
	return(flag);
}

struct lengths *new_length(unsigned long length)
{
	struct lengths *l;

	l=malloc(sizeof(struct lengths));
	if(l==NULL) return(NULL);
	l->count=1;
	l->sum=length;
	l->lower_bound=length/100*100;
	l->upper_bound=length/100*100+99;
	l->min=l->max=length;
	l->next=NULL;
	return(l);
}

int add_length(struct lengths **first,unsigned long length)
{
	struct lengths *l,*last;

	if(*first==NULL)
	{
		*first=new_length(length);
		if(*first==NULL) return(-1);
		return(0);
	}
	l=*first;
	while(l!=NULL)
	{
		if(l->lower_bound<=length && length<=l->upper_bound)
		{
			l->count++;
			l->sum+=length;
			l->min=min(l->min,length);
			l->max=max(l->max,length);
			return(0);
		}
		last=l;
		l=l->next;
	}
	last->next=new_length(length);
	if(last->next==NULL) return(-1);
	return(0);
}

void free_lengths(struct lengths *first)
{
	struct lengths *next;

	if(first==NULL) return;
	while(first!=NULL)
	{
		next=first->next;
		free(first);
		first=next;
	}
}

void merge_lengths(struct lengths *first)
{
	struct lengths *l,*inner,*last;
	unsigned long new_sum;
	int new_count;

	l=first;
	while(l!=NULL)
	{
		last=l;
		inner=l->next;
		while(inner!=NULL)
		{
			new_sum=l->sum+inner->sum;
			new_count=l->count+inner->count;
			
			if((l->max<=new_sum/new_count+AEPS &&
			    l->min>=new_sum/new_count-AEPS &&
			    inner->max<=new_sum/new_count+AEPS &&
			    inner->min>=new_sum/new_count-AEPS)
			   ||
			   (l->max<=new_sum/new_count*(100+EPS) &&
			    l->min>=new_sum/new_count*(100-EPS) &&
			    inner->max<=new_sum/new_count*(100+EPS) &&
			    inner->min>=new_sum/new_count*(100-EPS)))
			{
				l->sum=new_sum;
				l->count=new_count;
				l->upper_bound=max(l->upper_bound,
						   inner->upper_bound);
				l->lower_bound=min(l->lower_bound,
						   inner->lower_bound);
				l->min=min(l->min,inner->min);
				l->max=max(l->max,inner->max);
				
				last->next=inner->next;
				free(inner);
				inner=last;
			}
			last=inner;
			inner=inner->next;
		}
		l=l->next;
	}
}

void get_header_length(struct ir_remote *remote,struct lengths **first_pulse,
		       struct lengths **first_space)
{
	int sum,match,i;
	struct lengths *p,*s,*plast,*slast;
	struct ir_ncode *codes;

	sum=get_sum(remote);
	p=*first_pulse;
	plast=NULL;
	while(p!=NULL)
	{
		if(p->count>=90*sum/100)
		{
			s=*first_space;
			slast=NULL;
			while(s!=NULL)
			{
				if(s->count>=90*sum/100 &&
				   (p->count<=sum || s->count<=sum))
				{
					
					codes=remote->codes;
					match=0;
					for(i=0;codes[i].name!=NULL;i++)
					{
						if(expect(remote,
							  remote->codes[i].signals[0],
							  (int) (p->sum/p->count))
						   &&
						   expect(remote,
							  remote->codes[i].signals[1],
							  (int) (s->sum/s->count)))
						{
							match++;
						}
					}
					if(match>=sum*90/100)
					{
						remote->phead=p->sum/p->count;
						remote->shead=s->sum/s->count;
						p->sum-=sum*p->sum/p->count;
						s->sum-=sum*s->sum/s->count;
						p->count-=sum;
						s->count-=sum;
						if(p->count<=0)
						{
							if(plast==NULL)
							{
								plast=*first_pulse;
								*first_pulse=plast->next;
								free(plast);
							}
							else
							{
								plast->next=p->next;
								free(p);
							}
						}
						if(s->count<=0)
						{
							if(slast==NULL)
							{
								slast=*first_space;
								*first_space=slast->next;
								free(slast);
							}
							else
							{
								slast->next=s->next;
								free(s);
							}
						}
						return;
					}
				}
				slast=s;
				s=s->next;
			}
		}
		plast=p;
		p=p->next;
	}
}

unsigned long get_length(struct ir_remote *remote,struct lengths *first,
			 unsigned long l)
{
	while(first!=NULL)
	{
		if(expect(remote,l,first->sum/first->count))
		{
			return(first->sum/first->count);
		}
		first=first->next;
	}
	return(0);
}

int is_bit(struct ir_remote *remote,unsigned long pulse, unsigned long space)
{
	int i,j,match,sum;
	struct ir_ncode *codes;

	sum=get_sum(remote);
	match=0;
	codes=remote->codes;
	for(i=0;codes[i].name!=NULL;i++)
	{
		for(j=has_header(remote) ? 2:0;j+2<codes[i].length;j+=2)
		{
			if(expect(remote,codes[i].signals[j],pulse) &&
			   expect(remote,codes[i].signals[j+1],space))
			{
				match++;
			}
		}
	}
	sum*=(remote->bits-1-(has_header(remote) ? 2:0));
	sum/=2;
	if(match>=20*sum/100)
	{
		return(1);
	}
	return(0);
}

int get_one_length(struct ir_remote *remote,struct lengths **first_pulse,
		    struct lengths **first_space)
{
	int i,j;
	struct ir_ncode *codes;
	unsigned long pulse,space;

	codes=remote->codes;
	for(i=0;codes[i].name!=NULL;i++)
	{
		j=has_header(remote) ? 2:0;
		pulse=get_length(remote,*first_pulse,codes[i].signals[j]);
		space=get_length(remote,*first_space,codes[i].signals[j+1]);

		if(pulse!=0 && space!=0)
		{
			if(is_bit(remote,pulse,space))
			{
				remote->pone=pulse;
				remote->sone=space;
				return(0);
			}
		}
	}
	return(-1);
}

int get_zero_length(struct ir_remote *remote,struct lengths **first_pulse,
		    struct lengths **first_space)
{
	int i,j;
	struct ir_ncode *codes;
	unsigned long pulse,space;

	codes=remote->codes;
	for(i=0;codes[i].name!=NULL;i++)
	{
		for(j=has_header(remote) ? 2:0;j+2<codes[i].length;j+=2)
		{

			if(expect(remote,codes[i].signals[j],
				  remote->pone)==0 
			   || expect(remote,codes[i].signals[j+1],
				     remote->sone)==0)
			{
				pulse=get_length(remote,*first_pulse,
						 codes[i].signals[j]);
				space=get_length(remote,*first_space,
						 codes[i].signals[j+1]);
				if(is_bit(remote,pulse,space))
				{
					remote->pzero=pulse;
					remote->szero=space;
					return(0);
				}
			}
		}
	}
	return(-1);
}

int get_trail_length(struct ir_remote *remote,struct lengths **first_pulse)
{
	int sum,match,i;
	struct lengths *p,*plast;
	struct ir_ncode *codes;

	sum=get_sum(remote);
	p=*first_pulse;
	plast=NULL;
	while(p!=NULL)
	{
		if(p->count>=sum)
		{
			codes=remote->codes;
			match=0;
			for(i=0;codes[i].name!=NULL;i++)
			{
				if(expect(remote,
					  remote->codes[i].signals[remote->codes[i].length-1],
					  (int) (p->sum/p->count)))
				{
					match++;
				}
			}
			if(match>=sum*90/100)
			{
				remote->ptrail=p->sum/p->count;
				p->sum-=sum*p->sum/p->count;
				p->count-=sum;
				if(p->count==0)
				{
					if(plast==NULL)
					{
						plast=*first_pulse;
						*first_pulse=plast->next;
						free(plast);
					}
					else
					{
						plast->next=p->next;
						free(p);
					}
				}
				return(0);
			}
		}
		plast=p;
		p=p->next;
	}
	return(-1);
}

int get_lengths(struct ir_remote *remote)
{
	struct lengths *first_space=NULL,*first_pulse=NULL;
	int i,j;
	struct ir_ncode *codes;

	/* get all spaces */

	codes=remote->codes;
	for(i=0;codes[i].name!=NULL;i++)
	{
		for(j=1;codes[i].signals[j]!=0;j+=2)
		{
			if(-1==add_length(&first_space,codes[i].signals[j]))
			{
				free_lengths(first_space);
				return(-1);
			}
		}
	}
	merge_lengths(first_space);

	/* and now all pulses */

	codes=remote->codes;
	for(i=0;codes[i].name!=NULL;i++)
	{
		for(j=0;j<codes[i].length;j+=2)
		{
			if(-1==add_length(&first_pulse,codes[i].signals[j]))
			{
				free_lengths(first_space);
				free_lengths(first_pulse);
				return(-1);
			}
		}
	}
	merge_lengths(first_pulse);
	
	get_header_length(remote,&first_pulse,&first_space);
	if(-1==get_trail_length(remote,&first_pulse))
	{
		free_lengths(first_space);
		free_lengths(first_pulse);
		return(-1);
	}
	if(-1==get_one_length(remote,&first_pulse,&first_space))
	{
		free_lengths(first_space);
		free_lengths(first_pulse);
		return(-1);
	}
	if(-1==get_zero_length(remote,&first_pulse,&first_space))
	{
		free_lengths(first_space);
		free_lengths(first_pulse);
		return(-1);
	}

	remote->bits--;
	if(has_header(remote)) remote->bits-=2;
	remote->bits/=2;
	if(remote->bits>64) /* can't handle more than 64 bits in normal mode */
	{
		free_lengths(first_space);
		free_lengths(first_pulse);
		return(-1);
	}
#ifndef LONG_IR_CODE
	if(remote->bits>32)
	{
		fprintf(stderr,"%s: this remote control sends more than "
			"32 bits\n",progname);
		fprintf(stderr,"%s: recompile the package using "
			"LONG_IR_CODE\n",progname);
		return(-1);
	}
#endif

	free_lengths(first_space);
	free_lengths(first_pulse);
	return(0);
}

struct ir_ncode *current;

int get_codes(struct ir_remote *remote)
{
	struct ir_ncode *codes;

	codes=remote->codes;
	while(codes->name!=NULL)
	{
		rec_buffer.rptr=rec_buffer.wptr=0;
		clear_rec_buffer(remote->gap);

		current=codes;
		if(decode(remote))
		{
			codes->code=remote->post_data;
			remote->post_data=0;
		}
		else
		{
			return(-1);
		}
		codes++;
	}
	return(0);
}

unsigned long readdata(unsigned long maxusec)
{
	static int i=0;
	static struct ir_ncode *codes=NULL;
	unsigned long data;

	if(codes!=current)
	{
		i=0;
		codes=current;
	}

	if(i<current->length)
	{
		data=current->signals[i];
		i++;
		return(i%2 ? data|PULSE_BIT:data);
	}
	return(0);
}

void get_pre_data(struct ir_remote *remote)
{
	struct ir_ncode *codes;
	ir_code mask,last;
	int count,i;
	
	mask=(-1);
	codes=remote->codes;
	if(codes->name!=NULL)
	{
		last=codes->code;
		codes++;
	}
	if(codes->name==NULL) return; /* at least 2 codes needed */
	while(codes->name!=NULL)
	{
		mask&=~(last^codes->code);
		last=codes->code;
		codes++;
	}
	count=0;
#ifdef LONG_IR_CODE
	while(mask&0x8000000000000000LL)
#else
	while(mask&0x80000000L)
#endif
	{
		count++;
		mask=mask<<1;
	}
	count-=sizeof(ir_code)*CHAR_BIT-remote->bits;
	if(count>0)
	{
		mask=0;
		for(i=0;i<count;i++)
		{
			mask=mask<<1;
			mask|=1;
		}
		remote->bits-=count;
		mask=mask<<(remote->bits);
		remote->pre_data_bits=count;
		remote->pre_data=(last&mask)>>(remote->bits);

		codes=remote->codes;
		while(codes->name!=NULL)
		{
			codes->code&=~mask;
			codes++;
		}
	}
}

void get_post_data(struct ir_remote *remote)
{
	struct ir_ncode *codes;
	ir_code mask,last;
	int count,i;
	
	mask=(-1);
	codes=remote->codes;
	if(codes->name!=NULL)
	{
		last=codes->code;
		codes++;
	}
	if(codes->name==NULL) return; /* at least 2 codes needed */
	while(codes->name!=NULL)
	{
		mask&=~(last^codes->code);
		last=codes->code;
		codes++;
	}
	count=0;
	while(mask&0x1)
	{
		count++;
		mask=mask>>1;
	}
	if(count>0)
	{
		mask=0;
		for(i=0;i<count;i++)
		{
			mask=mask<<1;
			mask|=1;
		}
		remote->bits-=count;
		remote->post_data_bits=count;
		remote->post_data=last&mask;

		codes=remote->codes;
		while(codes->name!=NULL)
		{
			codes->code=codes->code>>count;
			codes++;
		}
	}
}

/*      $Id: config_file.c,v 5.4 2000/04/24 19:41:42 columbus Exp $      */

/****************************************************************************
 ** config_file.c ***********************************************************
 ****************************************************************************
 *
 * config_file.c - parses the config file of lircd
 *
 * Copyright (C) 1998 Pablo d'Angelo <pablo@ag-trek.allgaeu.org>
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#include "lircd.h"
#include "ir_remote.h"
#include "config_file.h"

#define LINE_LEN 1024

int line;
int parse_error;

void **init_void_array(struct void_array *ar,size_t chunk_size, size_t item_size)
{
        ar->chunk_size=chunk_size;
        ar->item_size=item_size;
	ar->nr_items=0;
        if(!(ar->ptr=calloc(chunk_size, ar->item_size))){
                logprintf(0,"out of memory\n");
                parse_error=1;
                return(NULL);
        }
	return(ar->ptr);
}

int add_void_array (struct void_array *ar, void * dataptr)
{
	void *ptr;

        if ((ar->nr_items%ar->chunk_size)==(ar->chunk_size)-1){
                /* I hope this works with the right alignment,
		   if not we're screwed */
                if (!(ptr=realloc(ar->ptr,ar->item_size*((ar->nr_items)+(ar->chunk_size+1))))){
                        logprintf(0,"out of memory\n");
                        parse_error=1;
                        return(0);
                }
		ar->ptr=ptr;
        }
        memcpy((ar->ptr)+(ar->item_size*ar->nr_items), dataptr, ar->item_size);
        ar->nr_items=(ar->nr_items)+1;
        memset((ar->ptr)+(ar->item_size*ar->nr_items), 0, ar->item_size);
        return(1);
}

inline void *get_void_array(struct void_array *ar)
{
        return(ar->ptr);
}

void *s_malloc(size_t size)
{
        void *ptr;
        if((ptr=malloc(size))==NULL){
                logprintf(0,"out of memory\n");
                parse_error=1;
                return(NULL);
        }
        memset(ptr, 0, size);
        return (ptr);
}

inline char *s_strdup(char * string)
{
        char *ptr;
        if(!(ptr=strdup(string))){
                logprintf(0,"out of memory\n");
                parse_error=1;
                return(NULL);
        }
        return (ptr);
}

inline ir_code s_strtocode(char *val)
{
	ir_code code=0;
	char *endptr;

	errno=0;
#       ifdef LONG_IR_CODE
	code=strtouq(val,&endptr,0);
	if((code==(unsigned long long) -1 && errno==ERANGE) ||
	    strlen(endptr)!=0 || strlen(val)==0)
	{
		logprintf(0,"error in configfile line %d:\n",line);
		logprintf(0,"\"%s\": must be a valid (unsigned long long) "
			  "number\n",val);
		parse_error=1;
		return(0);
	}
#       else
	code=strtoul(val,&endptr,0);
	if(code==ULONG_MAX && errno==ERANGE)
	{
		logprintf(0,"error in configfile line %d:\n",line);
		logprintf(0,"code is out of range\n");
		logprintf(0,"try compiling lircd with the LONG_IR_CODE "
			  "option\n");
		parse_error=1;
		return(0);
	}
	else if(strlen(endptr)!=0 || strlen(val)==0)
	{
		logprintf(0,"error in configfile line %d:\n",line);
		logprintf(0,"\"%s\": must be a valid (unsigned long) "
			  "number\n",val);
		parse_error=1;
		return(0);
	}
#       endif
	return(code);
}

unsigned long s_strtoul(char *val)
{
	unsigned long n;
	char *endptr;

	n=strtoul(val,&endptr,0);
	if(!*val || *endptr)
	{
		logprintf(0,"error in configfile line %d:\n",line);
		logprintf(0,"\"%s\": must be a valid (unsigned long) "
			  "number\n",val);
		parse_error=1;
		return(0);
	}
	return(n);
}

int s_strtoi(char *val)
{
	char *endptr;
	long n;
	int h;
	
	n=strtol(val,&endptr,0);
	h=(int) n;
	if(!*val || *endptr || n!=((long) h))
	{
		logprintf(0,"error in configfile line %d:\n",line);
		logprintf(0,"\"%s\": must be a valid (int) number\n",val);
		parse_error=1;
		return(0);
	}
	return(h);
}

unsigned int s_strtoui(char *val)
{
	char *endptr;
	unsigned long n;
	unsigned int h;
	
	n=strtoul(val,&endptr,0);
	h=(unsigned int) n;
	if(!*val || *endptr || n!=((unsigned long) h))
	{
		logprintf(0,"error in configfile line %d:\n",line);
		logprintf(0,"\"%s\": must be a valid (unsigned int) number\n",val);
		parse_error=1;
		return(0);
	}
	return(h);
}

lirc_t s_strtolirc_t(char *val)
{
	unsigned long n;
	lirc_t h;
	char *endptr;
	
	n=strtoul(val,&endptr,0);
	h=(lirc_t) n;
	if(!*val || *endptr || n!=((unsigned long) h))
	{
		logprintf(0,"error in configfile line %d:\n",line);
		logprintf(0,"\"%s\": must be a valid (lirc_t) "
			  "number\n",val);
		parse_error=1;
		return(0);
	}
	return(h);
}

int checkMode(int is_mode, int c_mode, char *error)
{
        if (is_mode!=c_mode)
	{
		logprintf(0,"fatal error in configfile line %d:\n",line);
		logprintf(0,"\"%s\" isn´t valid at this position\n",error);
		parse_error=1;
		return(0);
	}
        return(1);
}

int addSignal(struct void_array *signals, char *val)
{
	lirc_t t;
	
	t=s_strtolirc_t(val);
	if(parse_error) return(0);
	if(!add_void_array(signals, &t)){
		return(0);
	}
        return(1);
}
	      
struct ir_ncode *defineCode(char *key, char *val, struct ir_ncode *code)
{
        code->name=s_strdup(key);
        code->code=s_strtocode(val);
#       ifdef DEBUG
#       ifdef LONG_IR_CODE
        logprintf(3,"      %-20s 0x%016llX\n",code->name, code->code);
#       else
        logprintf(3,"      %-20s 0x%016lX\n",code->name, code->code);
#       endif
#       endif
        return(code);
}

int parseFlags(char *val)
{
        struct flaglist *flaglptr;
	int flags=0;
	char *flag,*help;

	flag=help=val;
	while(flag!=NULL)
	{
		while(*help!='|' && *help!=0) help++;
		if(*help=='|')
		{
			*help=0;help++;
		}
		else
		{
			help=NULL;
		}
	
		flaglptr=all_flags;
		while(flaglptr->name!=NULL){
			if(strcasecmp(flaglptr->name,flag)==0){
				flags=flags|flaglptr->flag;
#                               ifdef DEBUG
				logprintf(3,"flag %s recognized\n",
					  flaglptr->name);
#                               endif
				break;
			}
			flaglptr++;
		}
		if(flaglptr->name==NULL)
		{
			logprintf(0,"error in configfile line %d:\n",line);
			logprintf(0,"unknown flag: \"%s\"\n",flag);
			parse_error=1;
			return(0);
		}
		flag=help;
	}
#       ifdef DEBUG
        logprintf(2,"flags value: %d\n",flags);
#       endif

        return(flags);
}

int defineRemote(char * key, char * val, char *val2, struct ir_remote *rem)
{
	if ((strcasecmp("name",key))==0){
		if(rem->name!=NULL) free(rem->name);
		rem->name=s_strdup(val);
#               ifdef DEBUG
		logprintf(1,"parsing %s remote\n",val);
#               endif
		return(1);
	}
	else if ((strcasecmp("bits",key))==0){
		rem->bits=s_strtoi(val);
		return(1);
	}
	else if (strcasecmp("flags",key)==0){
		rem->flags|=parseFlags(val);
		return(1);
	}
	else if (strcasecmp("eps",key)==0){
		rem->eps=s_strtoi(val);
		return(1);
	}
	else if (strcasecmp("aeps",key)==0){
		rem->aeps=s_strtoi(val);
		return(1);
	}
	else if (strcasecmp("plead",key)==0){
		rem->plead=s_strtolirc_t(val);
		return(1);
	}
	else if (strcasecmp("ptrail",key)==0){
		rem->ptrail=s_strtolirc_t(val);
		return(1);
	}
	else if (strcasecmp("pre_data_bits",key)==0){
		rem->pre_data_bits=s_strtoi(val);
		return(1);
	}
	else if (strcasecmp("pre_data",key)==0){
		rem->pre_data=s_strtocode(val);
		return(1);
	}
	else if (strcasecmp("post_data_bits",key)==0){
		rem->post_data_bits=s_strtoi(val);
		return(1);
	}
	else if (strcasecmp("post_data",key)==0){
		rem->post_data=s_strtocode(val);
		return(1);
	}
	else if (strcasecmp("gap",key)==0){
		rem->gap=s_strtoul(val);
		return(1);
	}
	else if (strcasecmp("repeat_gap",key)==0){
		rem->repeat_gap=s_strtoul(val);
		return(1);
	}
	else if (strcasecmp("repeat_bit",key)==0){
		rem->repeat_bit=s_strtoi(val);
		return(1);
	}
	else if (strcasecmp("frequency",key)==0){
		rem->freq=s_strtoui(val);
		return(1);
	}
	else if (strcasecmp("duty_cycle",key)==0){
		rem->duty_cycle=s_strtoui(val);
		return(1);
	}
	else if (val2!=NULL)
	{
		if (strcasecmp("header",key)==0){
			rem->phead=s_strtolirc_t(val);
			rem->shead=s_strtolirc_t(val2);
			return(2);
		}
		else if (strcasecmp("one",key)==0){
			rem->pone=s_strtolirc_t(val);
			rem->sone=s_strtolirc_t(val2);
			return(2);
		}
		else if (strcasecmp("zero",key)==0){
			rem->pzero=s_strtolirc_t(val);
			rem->szero=s_strtolirc_t(val2);
			return(2);
		}
		else if (strcasecmp("foot",key)==0){
			rem->pfoot=s_strtolirc_t(val);
			rem->sfoot=s_strtolirc_t(val2);
			return(2);
		}
		else if (strcasecmp("repeat",key)==0){
			rem->prepeat=s_strtolirc_t(val);
			rem->srepeat=s_strtolirc_t(val2);
			return(2);
		}
		else if (strcasecmp("pre",key)==0){
			rem->pre_p=s_strtolirc_t(val);
			rem->pre_s=s_strtolirc_t(val2);
			return(2);
		}
		else if (strcasecmp("post",key)==0){
			rem->post_p=s_strtolirc_t(val);
			rem->post_s=s_strtolirc_t(val2);
			return(2);
		}
	}
	if(val2){
		logprintf(0,"error in configfile line %d:\n",line);
		logprintf(0,"unknown definiton: \"%s %s %s\"\n",
			  key, val, val2);
	}else{
		logprintf(0,"error in configfile line %d:\n",line);
		logprintf(0,"unknown definiton or too few arguments: "
			  "\"%s %s\"\n",key, val);
	}
	parse_error=1;
	return(0);
}
    
struct ir_remote * read_config(FILE *f)
{
	char buf[LINE_LEN+1], *key, *val, *val2;
        int len,argc;
	struct ir_remote *top_rem=NULL,*rem=NULL;
        struct void_array codes_list,raw_codes,signals;
	struct ir_ncode raw_code={NULL,0,0,NULL};
	struct ir_ncode name_code={NULL,0,0,NULL};
	int mode=ID_none;

	line=0;
	parse_error=0;

	while(fgets(buf,LINE_LEN,f)!=NULL)
	{
		line++;
		len=strlen(buf);
		if(len==LINE_LEN && buf[len-1]!='\n')
		{
			logprintf(0,"line %d too long in config file\n",
				line);
			parse_error=1;
			break;
		}

                /* ignore comments */
		len--;
		if(buf[len]=='\n') buf[len]=0;
                if(buf[0]=='#'){
			continue;
                }
		key=strtok(buf," \t");
		/* ignore empty lines */
		if(key==NULL) continue;
		val=strtok(NULL, " \t");
		if(val!=NULL){
			val2=strtok(NULL, " \t");
#                       ifdef DEBUG
                        logprintf(3,"\"%s\" \"%s\"\n",key,val);
#                       endif
                        if (strcasecmp("begin",key)==0){
				if (strcasecmp("codes", val)==0){
                                        /* init codes mode */
#                                       ifdef DEBUG
                                        logprintf(2,"    begin codes\n");
#                                       endif
					if (!checkMode(mode, ID_remote,
						       "begin codes")) break;
					if (rem->codes){
						logprintf(0,"error in configfile line %d:\n",line);
						logprintf(0,"codes are already defined\n");
						parse_error=1;
						break;
					}
					
                                        init_void_array(&codes_list,30, sizeof(struct ir_ncode));
                                        mode=ID_codes;
                                }else if(strcasecmp("raw_codes",val)==0){
                                        /* init raw_codes mode */
#                                       ifdef DEBUG
                                        logprintf(2,"    begin raw_codes\n");
#                                       endif
					if(!checkMode(mode, ID_remote,
						  "begin raw_codes")) break;
					if (rem->codes){
						logprintf(0,"error in configfile line %d:\n",line);
						logprintf(0,"codes are already defined\n");
						parse_error=1;
						break;
					}
					rem->flags|=RAW_CODES;
					raw_code.code=0;
                                        init_void_array(&raw_codes,30, sizeof(struct ir_ncode));
                                        mode=ID_raw_codes;
                                }else if(strcasecmp("remote",val)==0){
					/* create new remote */
#                                       ifdef DEBUG
					logprintf(1,"parsing remote\n");
#                                       endif
					if(!checkMode(mode, ID_none,
						  "begin remote")) break;
                                        mode=ID_remote;
                                        if (!top_rem){
                                                /* create first remote */
#                                               ifdef DEBUG
						logprintf(2,"creating first remote\n");
#                                               endif
                                                rem=top_rem=s_malloc(sizeof(struct ir_remote));
                                        }else{
                                                /* create new remote */
#                                               ifdef DEBUG
						logprintf(2,"creating next remote\n");
#                                               endif
                                                rem->next=s_malloc(sizeof(struct ir_remote));;
                                                rem=rem->next;
                                        }
                                }else{
                                        logprintf(0,"error in configfile line %d:\n",line);
					logprintf(0,"unknown section \"%s\"\n",val);
                                        parse_error=1;
                                }
				if(!parse_error && val2!=NULL)
				{
					logprintf(0,"WARNING: garbage after '%s'"
						  " token in line %d ignored\n",
						  val,line);
				}
                        }else if (strcasecmp("end",key)==0){

				if (strcasecmp("codes", val)==0){
					/* end Codes mode */
#                                       ifdef DEBUG
                                        logprintf(2,"    end codes\n");
#                                       endif
                                        if (!checkMode(mode, ID_codes,
						       "end codes")) break;
                                        rem->codes=get_void_array(&codes_list);
                                        mode=ID_remote;     /* switch back */

                                }else if(strcasecmp("raw_codes",val)==0){
                                        /* end raw codes mode */
#                                       ifdef DEBUG
                                        logprintf(2,"    end raw_codes\n");
#                                       endif

					if(mode==ID_raw_name){
						raw_code.signals=get_void_array(&signals);
						raw_code.length=signals.nr_items;
						if(raw_code.length%2==0)
						{
							logprintf(0,"error in configfile line %d:\n",line);
							logprintf(0,"bad signal length\n",val);
							parse_error=1;
						}
						if(!add_void_array(&raw_codes, &raw_code))
							break;
						mode=ID_raw_codes;
					}
                                        if(!checkMode(mode,ID_raw_codes,
						      "end raw_codes")) break;
					rem->codes=get_void_array(&raw_codes);
					mode=ID_remote;     /* switch back */
                                }else if(strcasecmp("remote",val)==0){
                                        /* end remote mode */
#                                       ifdef DEBUG
                                        logprintf(2,"end remote\n");
#                                       endif
					/* print_remote(rem); */
                                        if (!checkMode(mode,ID_remote,
                                                  "end remote")) break;
                                        if (!rem->name){
                                                logprintf(0,"you must specify a remote name\n");
                                                parse_error=1;
                                                break;
                                        }

					/* not really necessary because we
					   clear the alloced memory */
                                        rem->next=NULL;
					rem->last_code=NULL;
                                        mode=ID_none;     /* switch back */
					if(has_repeat_gap(rem) && 
					   is_const(rem))
					{
						logprintf(0,"WARNING: repeat_gap will be ignored if CONST_LENGTH flag is set\n");
					}
                                }else{
                                        logprintf(0,"error in configfile line %d:\n",line);
					logprintf(0,"unknown section %s",val);
                                        parse_error=1;
                                }
				if(!parse_error && val2!=NULL)
				{
					logprintf(0,"WARNING: garbage after '%s'"
						  " token in line %d ignored\n",
						  val,line);
				}
                        } else {
				switch (mode){
				case ID_remote:
					argc=defineRemote(key, val, val2, rem);
					if(!parse_error && ((argc==1 && val2!=NULL) || 
					   (argc==2 && val2!=NULL && strtok(NULL," \t")!=NULL)))
					{
						logprintf(0,"WARNING: garbage after '%s'"
							  " token in line %d ignored\n",
							  key,line);
					}
					break;
				case ID_codes:
					add_void_array(&codes_list, defineCode(key, val, &name_code));
					if(!parse_error && val2!=NULL)
					{
						logprintf(0,"WARNING: garbage after '%s'"
							  " code in line %d ignored\n",
							  key,line);
					}
					break;
				case ID_raw_codes:
				case ID_raw_name:
					if(strcasecmp("name",key)==0){
#                                               ifdef DEBUG
						logprintf(3,"Button: \"%s\"\n",val);
#                                               endif
						if(mode==ID_raw_name)
						{
                                                        raw_code.signals=get_void_array(&signals);
							raw_code.length=signals.nr_items;
							if(raw_code.length%2==0)
							{
								logprintf(0,"error in configfile line %d:\n",line);
								logprintf(0,"bad signal length\n",val);
								parse_error=1;
							}
							if(!add_void_array(&raw_codes, &raw_code))
								break;
						}
						if(!(raw_code.name=s_strdup(val))){
							break;
						}
						raw_code.code++;
						init_void_array(&signals,50,sizeof(lirc_t));
						mode=ID_raw_name;
						if(!parse_error && val2!=NULL)
						{
							logprintf(0,"WARNING: garbage after '%s'"
								  " token in line %d ignored\n",
								  key,line);
						}
					}else{
						if(mode==ID_raw_codes)
						{
							logprintf(0,"no name for signal defined at line %d\n",line);
							parse_error=1;
							break;
						}
						if(!addSignal(&signals, key)) break;
						if(!addSignal(&signals, val)) break;
						if (val2){
							if (!addSignal(&signals, val2)){
								break;
							}
						}
						while ((val=strtok(NULL," \t"))){
							if (!addSignal(&signals, val)) break;
						}
					}
					break;
				}
			}
		}else if(mode==ID_raw_name){
                        if(!addSignal(&signals, key)){
				break;
			}
		}else{
                        logprintf(0,"error in configfile line %d\n", line);
			parse_error=1;
			break;
                }
                if (parse_error){
                        break;
                }
        }
	if(mode!=ID_none)
	{
		switch(mode)
		{
		case ID_raw_name:
			if(raw_code.name!=NULL)
			{
				free(raw_code.name);
				if(get_void_array(&signals)!=NULL)
					free(get_void_array(&signals));
			}
		case ID_raw_codes:
			rem->codes=get_void_array(&raw_codes);
			break;
		case ID_codes:
			rem->codes=get_void_array(&codes_list);
			break;
		}
		if(!parse_error)
		{
			logprintf(0,"unexpected end of file\n");
			parse_error=1;
		}
	}
        if (parse_error){
		free_config(top_rem);
                return((void *) -1);
        }
	/* kick reverse flag */
	rem=top_rem;
	while(rem!=NULL)
	{
		if((!is_raw(rem)) && rem->flags&REVERSE)
		{
			struct ir_ncode *codes;
			
			if(has_pre(rem))
			{
				rem->pre_data=reverse(rem->pre_data,
						      rem->pre_data_bits);
			}
			if(has_post(rem))
			{
				rem->post_data=reverse(rem->post_data,
						       rem->post_data_bits);
			}
			codes=rem->codes;
			while(codes->name!=NULL)
			{
				codes->code=reverse(codes->code,rem->bits);
				codes++;
			}
			/* rem->flags=rem->flags&(~REVERSE); */
			/* don't delete the flag because we still need
			   it to remain compatible with older versions
			*/
		}
		rem=rem->next;
	}

#       if defined(DEBUG) && !defined(DAEMONIZE)
        /*fprint_remotes(stderr, top_rem);*/
#       endif
        return (top_rem);
}

void free_config(struct ir_remote *remotes)
{
	struct ir_remote *next;
	struct ir_ncode *codes;
	
	while(remotes!=NULL)
	{
		next=remotes->next;

		if(remotes->name!=NULL) free(remotes->name);
		if(remotes->codes!=NULL)
		{
			codes=remotes->codes;
			while(codes->name!=NULL)
			{
				free(codes->name);
				if(codes->signals!=NULL)
					free(codes->signals);
				codes++;
			}
			free(remotes->codes);
		}
		free(remotes);
		remotes=next;
	}
}

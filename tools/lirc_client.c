/*      $Id: lirc_client.c,v 5.1 1999/06/11 15:29:28 columbus Exp $      */

/****************************************************************************
 ** lirc_client.c ***********************************************************
 ****************************************************************************
 *
 * lirc_client - common routines for lircd clients
 *
 * Copyright (C) 1998 Trent Piepho <xyzzy@u.washington.edu>
 * Copyright (C) 1998 Christoph Bartelmus <columbus@hit.handshake.de>
 *
 */ 

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "lirc_client.h"

static int lirc_lircd;
static char *lirc_prog=NULL;
static char *lirc_buffer=NULL;

int lirc_init(char *prog)
{
	struct sockaddr_un addr;

	/* connect to lircd */

	if(prog==NULL || lirc_prog!=NULL) return(-1);
	lirc_prog=strdup(prog);
	if(lirc_prog==NULL)
	{
		fprintf(stderr,"%s: out of memory\n",progname);
		return(-1);
	}
	
	addr.sun_family=AF_UNIX;
	strcpy(addr.sun_path,LIRCD);
	lirc_lircd=socket(AF_UNIX,SOCK_STREAM,0);
	if(lirc_lircd==-1)
	{
		fprintf(stderr,"%s: could not open socket\n",progname);
		perror(progname);
		return(-1);
	}
	if(connect(lirc_lircd,(struct sockaddr *)&addr,sizeof(addr))==-1)
	{
		close(lirc_lircd);
		fprintf(stderr,"%s: could not connect to socket\n",progname);
		perror(progname);
		return(-1);
	}
	return(lirc_lircd);
}

int lirc_deinit()
{
	if(lirc_prog!=NULL) free(lirc_prog);
	if(lirc_buffer!=NULL) free(lirc_buffer);
	return(close(lirc_lircd));
}

#define LIRC_READ 255

int lirc_readline(char **line,FILE *f)
{
	char *newline,*ret,*enlargeline;
	int len;
	
	newline=(char *) malloc(LIRC_READ+1);
	if(newline==NULL)
	{
		fprintf(stderr,"%s: out of memory\n",progname);
		return(-1);
	}
	len=0;
	while(1)
	{
		ret=fgets(newline+len,LIRC_READ+1,f);
		if(ret==NULL)
		{
			free(newline);
			*line=NULL;
			return(0);
		}
		len=strlen(newline);
		if(newline[len-1]=='\n')
		{
			newline[len-1]=0;
			*line=newline;
			return(0);
		}
		
		enlargeline=(char *) realloc(newline,len+1+LIRC_READ);
		if(enlargeline==NULL)
		{
			free(newline);
			fprintf(stderr,"%s: out of memory\n",progname);
			return(-1);
		}
		newline=enlargeline;
	}
}

char *lirc_trim(char *s)
{
	int len;
	
	while(s[0]==' ' || s[0]=='\t') s++;
	len=strlen(s);
	while(len>0)
	{
		len--;
		if(s[len]==' ' || s[len]=='\t') s[len]=0;
		else break;
	}
	return(s);
}

/* parse standard C escape sequences + \@,\A-\Z is ^@,^A-^Z */

char lirc_parse_escape(char **s,int line)
{

	char c;
	unsigned int i,overflow,count;
	int digits_found,digit;

	c=**s;
	(*s)++;
	switch(c)
	{
	case 'a':
		return('\a');
	case 'b':
		return('\b');
	case 'e':
	case 'E':
		return(033);
	case 'f':
		return('\f');
	case 'n':
		return('\n');
	case 'r':
		return('\r');
	case 't':
		return('\t');
	case 'v':
		return('\v');
	case '\n':
		return(0);
	case 0:
		(*s)--;
		return 0;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
		i=c-'0';
		count=0;
		
		while(++count<3)
		{
			c=*(*s)++;
			if(c>='0' && c<='7')
			{
				i=(i << 3)+c-'0';
			}
			else
			{
				(*s)--;
				break;
			}
		}
		if(i>(1<<CHAR_BIT)-1)
		{
			i&=(1<<CHAR_BIT)-1;
			fprintf(stderr,"%s: octal escape sequence "
				"out of range in line %d\n",progname,
				line);
		}
		return((char) i);
	case 'x':
		{
			i=0;
			overflow=0;
			digits_found=0;
			for (;;)
			{
				c = *(*s)++;
				if(c>='0' && c<='9')
					digit=c-'0';
				else if(c>='a' && c<='f')
					digit=c-'a'+10;
				else if(c>='A' && c<='F')
					digit=c-'A'+10;
				else
				{
					(*s)--;
					break;
				}
				overflow|=i^(i<<4>>4);
				i=(i<<4)+digit;
				digits_found=1;
			}
			if(!digits_found)
			{
				fprintf(stderr,"%s: \\x used with no "
					"following hex digits in line %d\n",
					progname,line);
			}
			if(overflow || i>(1<<CHAR_BIT)-1)
			{
				i&=(1<<CHAR_BIT)-1;
				fprintf(stderr,"%s: hex escape sequence out "
					"of range in line %d\n",
					progname,line);
			}
			return((char) i);
		}
	default:
		if(c>='@' && c<='Z') return(c-'@');
		return(c);
	}
}

void lirc_parse_string(char *s,int line)
{
	char *t;

	t=s;
	while(*s!=0)
	{
		if(*s=='\\')
		{
			s++;
			*t=lirc_parse_escape(&s,line);
			t++;
		}
		else
		{
			*t=*s;
			s++;
			t++;
		}
	}
	*t=0;
}

int lirc_mode(char *token,char *token2,char **mode,
	      struct lirc_config_entry **new_config,
	      struct lirc_config_entry **first_config,
	      struct lirc_config_entry **last_config,
	      int (check)(char *s),
	      int line)
{
	struct lirc_config_entry *new_entry;

	new_entry=*new_config;
	if(strcasecmp(token,"begin")==0)
	{
		if(token2==NULL)
		{
			if(new_entry==NULL)
			{
				new_entry=(struct lirc_config_entry *)
				malloc(sizeof(struct lirc_config_entry));
				if(new_entry==NULL)
				{
					fprintf(stderr,"%s: out of memory\n",
						progname);
					return(-1);
				}
				else
				{
					new_entry->prog=NULL;
					new_entry->code=NULL;
					new_entry->rep=0;
					new_entry->config=NULL;
					new_entry->change_mode=NULL;
					new_entry->flags=none;
					new_entry->mode=NULL;
					new_entry->next_config=NULL;
					new_entry->next_code=NULL;
					new_entry->next=NULL;

					*new_config=new_entry;
				}
			}
			else
			{
				fprintf(stderr,"%s: bad file format, "
					"line %d\n",progname,line);
				return(-1);
			}
		}
		else
		{
			if(new_entry==NULL && *mode==NULL)
			{
				*mode=strdup(token2);
				if(*mode==NULL)
				{
					return(-1);
				}
			}
			else
			{
				fprintf(stderr,"%s: bad file format, "
					"line %d\n",progname,line);
				return(-1);
			}
		}
	}
	else if(strcasecmp(token,"end")==0)
	{
		if(token2==NULL)
		{
			if(new_entry!=NULL)
			{
#if 0
				if(new_entry->prog==NULL)
				{
					fprintf(stderr,"%s: prog missing in "
						"config before line %d\n",
						progname,line);
					lirc_freeconfigentries(new_entry);
					return(-1);
				}
				if(strcasecmp(new_entry->prog,lirc_prog)!=0)
				{
					lirc_freeconfigentries(new_entry);
					*new_config=NULL;
					return(0);
				}
#endif
				new_entry->next_code=new_entry->code;
				new_entry->next_config=new_entry->config;
				if(*last_config==NULL)
				{
					*first_config=new_entry;
					*last_config=new_entry;
				}
				else
				{
					(*last_config)->next=new_entry;
					*last_config=new_entry;
				}
				*new_config=NULL;

				if(*mode!=NULL) 
				{
					new_entry->mode=strdup(*mode);
					if(new_entry->mode==NULL)
					{
						fprintf(stderr,"%s: out of "
							"memory\n",progname);
						return(-1);
					}
				}

				if(check!=NULL &&
				   new_entry->prog!=NULL &&
				   strcasecmp(new_entry->prog,lirc_prog)==0)
				{					
					struct lirc_list *list;

					list=new_entry->config;
					while(list!=NULL)
					{
						if(check(list->string)==-1)
						{
							return(-1);
						}
						list=list->next;
					}
				}
				
			}
			else
			{
				fprintf(stderr,"%s: line %d: 'end' without "
					"'begin'\n",progname,line);
				return(-1);
			}
		}
		else
		{
			if(*mode!=NULL)
			{
				if(strcasecmp(*mode,token2)==0)
				{
					free(*mode);
					*mode=NULL;
				}
				else
				{
					fprintf(stderr,"%s: \"%s\" doesn't "
						"match mode \"%s\"\n",
						progname,token2,*mode);
					return(-1);
				}
			}
		}
	}
	else
	{
		fprintf(stderr,"%s: unknown "
			"token \"%s\" in line "
			"%d ignored\n",
			progname,token,line);
	}
	return(0);
}

unsigned int lirc_flags(char *string)
{
	char *s;
	unsigned int flags;

	flags=none;
	s=strtok(string," \t|");
	while(s)
	{
		if(strcasecmp(s,"once")==0)
		{
			flags|=once;
		}
		else if(strcasecmp(s,"quit")==0)
		{
			flags|=quit;
		}
		else if(strcasecmp(s,"mode")==0)
		{
			flags|=mode;
		}
		else
		{
			fprintf(stderr,"%s: unknown flag \"%s\"\n",progname,s);
		}
		s=strtok(NULL," \t");
	}
	return(flags);
}

int lirc_readconfig(char *file,
		    struct lirc_config **config,
		    int (check)(char *s))
{
	char *home,*filename,*string,*eq,*token,*token2,*token3;
	FILE *fin;
	struct lirc_config_entry *new_entry,*first,*last;
	char *mode,*remote;
	int line,ret;
	
	if(file==NULL)
	{
		home=getenv("HOME");
		filename=(char *) malloc(strlen(home)+1+strlen(LIRCCFGFILE)+1);
		if(filename==NULL)
			return(-1);
		strcpy(filename,home);
		if(strlen(home)>0 && filename[strlen(filename)-1]!='/')
		{
			strcat(filename,"/");
		}
		strcat(filename,LIRCCFGFILE);
	}
	else
	{
		filename=file;
	}

	fin=fopen(filename,"r");
	if(file==NULL) free(filename);
	if(fin==NULL)
	{
		fprintf(stderr,"%s: could not open config file\n",progname);
		perror(progname);
		return(-1);
	}
	line=1;
	first=new_entry=last=NULL;
	mode=NULL;
	remote=LIRC_ALL;
	while((ret=lirc_readline(&string,fin))!=-1 && string!=NULL)
	{
		eq=strchr(string,'=');
		if(eq==NULL)
		{
			token=strtok(string," \t");
			if(token==NULL)
			{
				/* ignore empty line */
			}
			else if(token[0]=='#')
			{
				/* ignore comment */
			}
			else
			{
				token2=strtok(NULL," \t");
				if(token2!=NULL && 
				   (token3=strtok(NULL," \t"))!=NULL)
				{
					fprintf(stderr,
						"%s: unexpected "
						"token in line %d\n",
						progname,line);
				}
				else
				{
					ret=lirc_mode(token,token2,&mode,
						      &new_entry,&first,&last,
						      check,
						      line);
					if(ret==0)
					{
						if(remote!=LIRC_ALL)
							free(remote);
						remote=LIRC_ALL;
					}
				}
			}
		}
		else
		{
			eq[0]=0;
			token=lirc_trim(string);
			token2=lirc_trim(eq+1);
			if(token[0]=='#')
			{
				/* ignore comment */
			}
			else if(new_entry==NULL)
			{
				fprintf(stderr,"%s: bad file format, "
					"line %d\n",progname,line);
				ret=-1;
			}
			else
			{
				token2=strdup(token2);
				if(token2==NULL)
				{
					fprintf(stderr,"%s: out of memory\n",
						progname);
					ret=-1;
				}
				else if(strcasecmp(token,"prog")==0)
				{
					if(new_entry->prog!=NULL) free(new_entry->prog);
					new_entry->prog=token2;
				}
				else if(strcasecmp(token,"remote")==0)
				{
					if(remote!=LIRC_ALL)
						free(remote);
					
					if(strcasecmp("*",token2)==0)
					{
						remote=LIRC_ALL;
						free(token2);
					}
					else
					{
						remote=token2;
					}
				}
				else if(strcasecmp(token,"button")==0)
				{
					struct lirc_code *code;
					
					code=(struct lirc_code *)
					malloc(sizeof(struct lirc_code));
					if(code==NULL)
					{
						free(token2);
						fprintf(stderr,"%s: out of "
							"memory\n",
							progname);
						ret=-1;
					}
					else
					{
						code->remote=remote;
						if(strcasecmp("*",token2)==0)
						{
							code->button=LIRC_ALL;
							free(token2);
						}
						else
						{
							code->button=token2;
						}
						code->next=NULL;

						if(new_entry->code==NULL)
						{
							new_entry->code=code;
						}
						else
						{
							new_entry->next_code->next
							=code;
						}
						new_entry->next_code=code;
						if(remote!=LIRC_ALL)
						{
							remote=strdup(remote);
							if(remote==NULL)
							{
								fprintf(stderr,"%s: out of memory\n",progname);
								ret=-1;
							}
						}
					}
				}
				else if(strcasecmp(token,"repeat")==0)
				{
					char *end;

					errno=ERANGE+1;
					new_entry->rep=strtoul(token2,&end,0);
					if((new_entry->rep==ULONG_MAX 
					    && errno==ERANGE)
					   || end[0]!=0
					   || strlen(token2)==0)
					{
						fprintf(stderr,"%s: \"%s\" not"
							" a  valid number for "
							"repeat\n",progname,
							token2);
					}
					free(token2);
				}
				else if(strcasecmp(token,"config")==0)
				{
					struct lirc_list *new_list;

					new_list=(struct lirc_list *) 
					malloc(sizeof(struct lirc_list));
					if(new_list==NULL)
					{
						free(token2);
						fprintf(stderr,"%s: out of "
							"memory\n",
							progname);
						ret=-1;
					}
					else
					{
						lirc_parse_string(token2,line);
						new_list->string=token2;
						new_list->next=NULL;
						if(new_entry->config==NULL)
						{
							new_entry->config=new_list;
						}
						else
						{
							new_entry->next_config->next
							=new_list;
						}
						new_entry->next_config=new_list;
					}
				}
				else if(strcasecmp(token,"mode")==0)
				{
					if(new_entry->change_mode!=NULL) free(new_entry->change_mode);
					new_entry->change_mode=token2;
				}
				else if(strcasecmp(token,"flags")==0)
				{
					new_entry->flags=lirc_flags(token2);
					free(token2);
				}
				else
				{
					free(token2);
					fprintf(stderr,"%s: unknown token "
						"\"%s\" in line %d ignored\n",
						progname,token,line);
				}
			}
		}
		free(string);
		line++;
		if(ret==-1) break;
	}
	if(remote!=LIRC_ALL)
		free(remote);
	if(mode!=NULL)
		free(mode);

	fclose(fin);
	if(ret==0)
	{
		*config=(struct lirc_config *) malloc(sizeof(struct lirc_config));
		if(*config==NULL)
		{
			lirc_freeconfigentries(first);
			return(-1);
		}
		(*config)->first=first;
		(*config)->next=first;
		(*config)->current_mode=NULL;
	}
	else
	{
		*config=NULL;
		lirc_freeconfigentries(first);
	}
	return(ret);
}

void lirc_freeconfig(struct lirc_config *config)
{
	if(config!=NULL)
	{
		lirc_freeconfigentries(config->first);
		free(config);
	}
}

void lirc_freeconfigentries(struct lirc_config_entry *first)
{
	struct lirc_config_entry *c,*config_temp;
	struct lirc_list *list,*list_temp;
	struct lirc_code *code,*code_temp;

	c=first;
	while(c!=NULL)
	{
		if(c->prog) free(c->prog);
		if(c->change_mode) free(c->change_mode);
		if(c->mode) free(c->mode);

		code=c->code;
		while(code!=NULL)
		{
			if(code->remote!=NULL && code->remote!=LIRC_ALL)
				free(code->remote);
			if(code->button!=NULL && code->button!=LIRC_ALL)
				free(code->button);
			code_temp=code->next;
			free(code);
			code=code_temp;
		}

		list=c->config;
		while(list!=NULL)
		{
			if(list->string) free(list->string);
			list_temp=list->next;
			free(list);
			list=list_temp;
		}
		config_temp=c->next;
		free(c);
		c=config_temp;
	}
}

void lirc_clearmode(struct lirc_config *config)
{
	struct lirc_config_entry *scan;

	if(config->current_mode==NULL)
	{
		return;
	}
	scan=config->first;
	while(scan!=NULL)
	{
		if(scan->change_mode!=NULL)
		{
			if(strcasecmp(scan->change_mode,config->current_mode)==0)
			{
				scan->flags&=~ecno;
			}
		}
		scan=scan->next;
	}
	config->current_mode=NULL;
}

char *lirc_execute(struct lirc_config *config,struct lirc_config_entry *scan)
{
	char *s;
	int do_once=1;
	
	if(scan->flags&quit)
	{
		config->next=NULL;
	}
	else
	{
		config->next=scan->next;
	}
	if(scan->flags&mode)
	{
		lirc_clearmode(config);
	}
	if(scan->change_mode!=NULL)
	{
		config->current_mode
		=scan->change_mode;
		if(scan->flags&once)
		{
			if(scan->flags&ecno)
			{
				do_once=0;
			}
			else
			{
				scan->flags|=ecno;
			}
		}
	}
	if(scan->next_config!=NULL &&
	   scan->prog!=NULL &&
	   strcasecmp(scan->prog,lirc_prog)==0 &&
	   do_once==1)
	{
		s=scan->next_config->string;
		scan->next_config
		=scan->next_config->next;
		if(scan->next_config==NULL)
			scan->next_config
			=scan->config;
		return(s);
	}
	return(NULL);
}

int lirc_iscode(struct lirc_config_entry *scan,char *remote,char *button,int rep)
{
	struct lirc_code *codes;
	
	if(scan->code==NULL)
		return(1);

	if(scan->next_code->remote==LIRC_ALL || 
	   strcasecmp(scan->next_code->remote,remote)==0)
	{
		if(scan->next_code->button==LIRC_ALL || 
		   strcasecmp(scan->next_code->button,button)==0)
		{
			if(scan->code->next==NULL || rep==1)
			{
				scan->next_code=scan->next_code->next;
			}
			if(scan->next_code==NULL)
			{
				scan->next_code=scan->code;
                                if(scan->code->next!=NULL || 
                                   (scan->rep==0 ? rep==1:(rep%scan->rep)==0))
                                {
                                        return(1);
                                }
                                else
                                {
                                        return(0);
                                }
                        }
			else
			{
				return(0);
			}
		}
	}
        if(rep!=1) return(0);
	codes=scan->code;
        if(codes==scan->next_code) return(0);
	codes=codes->next;
	while(codes!=scan->next_code->next)
	{
                struct lirc_code *prev,*next;
                int flag=1;

                prev=scan->code;
                next=codes;
                while(next!=scan->next_code)
                {
                        if(prev->remote==LIRC_ALL ||
                           strcasecmp(prev->remote,next->remote)==0)
                        {
                                if(prev->button==LIRC_ALL ||
                                   strcasecmp(prev->button,next->button)==0)
                                {
                                        prev=prev->next;
                                        next=next->next;
                                }
                                else
                                {
                                        flag=0;break;
                                }
                        }
                        else
                        {
                                flag=0;break;
                        }
                }
                if(flag==1)
                {
                        if(prev->remote==LIRC_ALL ||
                           strcasecmp(prev->remote,remote)==0)
                        {
                                if(prev->button==LIRC_ALL ||
                                   strcasecmp(prev->button,button)==0)
                                {
                                        if(rep==1)
                                        {
                                                scan->next_code=prev->next;
                                                return(0);
                                        }
                                }
                        }
                }
                codes=codes->next;
	}
	scan->next_code=scan->code;
	return(0);
}

char *lirc_ir2char(struct lirc_config *config,char *string)
{
	int rep;
	char *backup;
	char *remote,*button;
	struct lirc_config_entry *scan;

	if(sscanf(string,"%*llx %x %*s %*s\n",&rep)==1)
	{
		rep++;
		backup=strdup(string);
		if(backup==NULL) return(NULL);

		strtok(backup," ");
		strtok(NULL," ");
		button=strtok(NULL," ");
		remote=strtok(NULL,"\n");

		if(button==NULL || remote==NULL)
		{
			free(backup);
			return(NULL);
		}
		
		scan=config->next;
		while(scan!=NULL)
		{
			if(lirc_iscode(scan,remote,button,rep) &&
			   (scan->mode==NULL ||
			    (scan->mode!=NULL && 
			     config->current_mode!=NULL &&
			     strcasecmp(scan->mode,config->current_mode)==0))
			   )
			{
				char *s;
				s=lirc_execute(config,scan);
				if(s!=NULL)
				{
					free(backup);
					return(s);
				}
			}
			if(config->next!=NULL)
			{
				scan=scan->next;
			}
			else
			{
				scan=NULL;
			}
		}
		
		free(backup);
	}
	config->next=config->first;
	return(NULL);
}

#define PACKET_SIZE 100

char *lirc_nextir()
{
	static int packet_size=PACKET_SIZE;
	static int end_len=0;
	ssize_t len=0;
	char *end,c,*string;


	if(lirc_buffer==NULL)
	{
		lirc_buffer=(char *) malloc(packet_size+1);
		lirc_buffer[0]=0;
	}
	if(lirc_buffer==NULL)
		return(NULL);
	
	while((end=strchr(lirc_buffer,'\n'))==NULL)
	{
		if(end_len<packet_size)
		{
			len=read(lirc_lircd,lirc_buffer+end_len,
				 packet_size-end_len);
			if(len<=0)
			{
				return(NULL);
			}
		}
		else
		{
			char *new_buffer;

			packet_size+=PACKET_SIZE;
			new_buffer=(char *) realloc(lirc_buffer,packet_size);
			if(new_buffer==NULL)
			{
				return(NULL);
			}
			lirc_buffer=new_buffer;
			len=read(lirc_lircd,lirc_buffer+end_len,
				 packet_size-end_len);
			if(len<=0)
			{
				return(NULL);
			}
		}
		end_len+=len;
		lirc_buffer[end_len]=0;
	}
	end++;
	end_len=strlen(end);
	c=end[0];
	end[0]=0;
	string=strdup(lirc_buffer);
	end[0]=c;
	memmove(lirc_buffer,end,end_len+1);
	return(string);
}

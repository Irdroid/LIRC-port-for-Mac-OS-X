/*      $Id: serial.c,v 5.1 1999/08/02 19:56:49 columbus Exp $      */

/****************************************************************************
 ** serial.c ****************************************************************
 ****************************************************************************
 *
 * common routines for hardware that uses the standard serial port driver
 * 
 * Copyright (C) 1999 Christoph Bartelmus <lirc@bartelmus.de>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "lircd.h"

int tty_reset(int fd)
{
	struct termios options;

	if(tcgetattr(fd,&options)==-1)
	{
#               ifdef DEBUG
		logprintf(1,"tty_reset(): tcgetattr() failed\n");
		logperror(1,"tty_reset()");
#               endif
		return(0);
	}
	cfmakeraw(&options);
	if(tcsetattr(fd,TCSAFLUSH,&options)==-1)
	{
#               ifdef DEBUG
		logprintf(1,"tty_reset(): tcsetattr() failed\n");
		logperror(1,"tty_reset()");
#               endif
		return(0);
	}
	return(1);
}

int tty_setbaud(int fd,int baud)
{
	struct termios options;
	int speed;

	switch(baud)
	{
	case 300:
		speed=B300;
		break;
	case 1200:
		speed=B1200;
		break;
	case 2400:
                speed=B2400;
                break;
	case 4800:
                speed=B4800;
                break;
	case 9600:
                speed=B9600;
                break;
	case 19200:
                speed=B19200;
                break;
	case 38400:
                speed=B38400;
                break;
	case 57600:
                speed=B57600;
                break;
	case 115200:
                speed=B115200;
                break;
	default:
#               ifdef DEBUG
		logprintf(1,"tty_setbaud(): bad baud rate %d\n",baud);
#               endif
		return(0);
	}		
	if(tcgetattr(fd, &options)==-1)
	{
#               ifdef DEBUG
		logprintf(1,"tty_setbaud(): tcgetattr() failed\n");
		logperror(1,"tty_setbaud()");
#               endif
		return(0);
	}
	(void) cfsetispeed(&options,speed);
	(void) cfsetospeed(&options,speed);
	if(tcsetattr(fd,TCSAFLUSH,&options)==-1)
	{
#               ifdef DEBUG
		logprintf(1,"tty_setbaud(): tcsetattr() failed\n");
		logperror(1,"tty_setbaud()");
#               endif
		return(0);
	}
	return(1);
}

int tty_create_lock(char *name)
{
	char filename[FILENAME_MAX+1];
	char symlink[FILENAME_MAX+1];
	char cwd[FILENAME_MAX+1];
	char *last,*s;
	char id[10+1];
	int lock;
	int len;
	
	strcpy(filename,"/var/lock/LCK..");
	
	last=strrchr(name,'/');
	if(last!=NULL)
		s=last+1;
	else
		s=name;
	
	if(strlen(filename)+strlen(s)>FILENAME_MAX)
	{
		logprintf(0,"%s: invalid filename \"%s%s\"\n",filename,s);
		return(0);
	}
	strcat(filename,s);
	
	if((len=snprintf(id,10+1,"%d",getpid()))==-1)
	{
		logprintf(0,"invalid pid \"%d\"\n",getpid());
		return(0);
	}

	lock=open(filename,O_CREAT|O_EXCL|O_WRONLY,0644);
	if(lock==-1)
	{
		logprintf(0,"could not create lock file \"%s\"\n",filename);
		logperror(0,NULL);
		lock=open(filename,O_RDONLY);
		if(lock==-1) return(0);
		len=read(lock,id,10);
		if(len<=0) return(0);
		if(read(lock,id,10)!=0) return(0);
		logprintf(0,"%s is locked by PID %s\n",name,id);
		close(lock);
		return(0);
	}
	if(write(lock,id,len)!=len)
	{
		logprintf(0,"%s: could not write pid to lock file\n");
		logperror(0,NULL);
		close(lock);
		if(unlink(filename)==-1)
		{
			logprintf(0,"could not delete file \"%s\"\n",filename);
			logperror(0,NULL);
			/* FALLTHROUGH */
		}
		return(0);
	}
	if(close(lock)==-1)
	{
		logprintf(0,"could not close lock file\n");
		logperror(0,NULL);
		if(unlink(filename)==-1)
		{
			logprintf(0,"could not delete file \"%s\"\n",filename);
			logperror(0,NULL);
			/* FALLTHROUGH */
		}
		return(0);
	}

	if((len=readlink(name,symlink,FILENAME_MAX))==-1)
	{
		if(errno!=EINVAL) /* symlink */
		{
			logprintf(0,"readlink() failed for \"%s\"\n",name);
			logperror(0,NULL);
			if(unlink(filename)==-1)
			{
				logprintf(0,"could not delete file \"%s\"\n",
					  filename);
				logperror(0,NULL);
				/* FALLTHROUGH */
			}
			return(0);
		}
	}
	else
	{
		symlink[len]=0;

		if(last)
		{
			char dirname[FILENAME_MAX+1];

			if(getcwd(cwd,FILENAME_MAX)==NULL)
			{
				logprintf(0,"getcwd() failed\n");
				logperror(0,NULL);
				if(unlink(filename)==-1)
				{
					logprintf(0,"%s: could not delete "
						  "file \"%s\"\n",filename);
					logperror(0,NULL);
				        /* FALLTHROUGH */
				}
				return(0);
			}
			
			strcat(dirname,name);
			dirname[strlen(name)-strlen(last)]=0;
			if(chdir(dirname)==-1)
			{
				last[0]='/';
				logprintf(0,"chdir() to \"%s\" "
					  "failded \n",dirname);
				logperror(0,NULL);
				if(unlink(filename)==-1)
				{
					logprintf(0,"could not delete "
						  "file \"%s\"\n",filename);
					logperror(0,NULL);
				        /* FALLTHROUGH */
				}
				return(0);
			}
		}
		if(tty_create_lock(symlink)==-1)
		{
			if(unlink(filename)==-1)
			{
				logprintf(0,"could not delete file "
					  "\"%s\"\n",filename);
				logperror(0,NULL);
				/* FALLTHROUGH */
			}
			return(0);
		}
		if(last)
		{
			if(chdir(cwd)==-1)
			{
				last[0]='/';
				logprintf(0,"chdir() to \"%s\" "
					  "failded \n",cwd);
				logperror(0,NULL);
				if(unlink(filename)==-1)
				{
					logprintf(0,"could not delete "
						  "file \"%s\"\n",filename);
					logperror(0,NULL);
				        /* FALLTHROUGH */
				}
				return(0);
			}
		}
	}
	return(1);
}

int tty_delete_lock(void)
{
	DIR *dp;
	struct dirent *ep;
	int lock;
	int len;
	char id[20+1],*endptr;
	char filename[FILENAME_MAX+1];
	long pid;
	int retval=1;
	
	dp=opendir("/var/lock/");
	if(dp!=NULL)
	{
		while((ep=readdir(dp)))
		{
			strcpy(filename,"/var/lock/");
			if(strlen(filename)+strlen(ep->d_name)>FILENAME_MAX) 
			{retval=0;continue;}
			strcat(filename,ep->d_name);
			lock=open(filename,O_RDONLY);
			if(lock==-1) {retval=0;continue;}
			len=read(lock,id,20);
			if(len<=0) {retval=0;continue;}
			id[len]=0;
			pid=strtol(id,&endptr,10);
			if(!*id || *endptr) {retval=0;continue;}
			if(pid==getpid())
			{
				if(unlink(filename)==-1)
				{
					logprintf(0,"could not delete "
						  "file \"%s\"\n",filename);
					perror(NULL);
					retval=0;
					continue;
				}
			}
		}
		closedir(dp);
	}
	else
	{
		logprintf(0,"could not open directory \"/var/lock/\"");
		return(0);
	}
	return(retval);
}

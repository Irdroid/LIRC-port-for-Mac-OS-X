/*      $Id: irexec.c,v 5.2 2000/02/02 20:28:42 columbus Exp $      */

/****************************************************************************
 ** irexec.c ****************************************************************
 ****************************************************************************
 *
 * irexec  - execute programs according to the pressed remote control buttons
 *
 * Copyright (C) 1998 Trent Piepho <xyzzy@u.washington.edu>
 * Copyright (C) 1998 Christoph Bartelmus <lirc@bartelmus.de>
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lirc_client.h"

char *progname;

int main(int argc, char *argv[])
{
	struct lirc_config *config;

	progname=argv[0];
	if(argc>2)
	{
		fprintf(stderr,"Usage: %s <config file>\n",progname);
		exit(EXIT_FAILURE);
	}
	if(lirc_init("irexec",1)==-1) exit(EXIT_FAILURE);

	if(lirc_readconfig(argc==2 ? argv[1]:NULL,&config,NULL)==0)
	{
		char *code;
		char *c;
		int ret;

		while(lirc_nextcode(&code)==0)
		{
			if(code==NULL) continue;
			while((ret=lirc_code2char(config,code,&c))==0 &&
			      c!=NULL)
			{
#ifdef DEBUG
				printf("Execing command \"%s\"\n",c);
#endif
				system(c);
			}
			free(code);
			if(ret==-1) break;
		}
		lirc_freeconfig(config);
	}

	lirc_deinit();
	exit(EXIT_SUCCESS);
}

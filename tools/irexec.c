/*      $Id: irexec.c,v 5.1 1999/09/13 05:52:41 columbus Exp $      */

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
		char *ir;
		char *c;

		while((ir=lirc_nextir())!=NULL)
		{
			while((c=lirc_ir2char(config,ir))!=NULL)
			{
#ifdef DEBUG
				printf("Execing command \"%s\"\n",c);
#endif
				system(c);
			}
			free(ir);
		}
		lirc_freeconfig(config);
	}

	lirc_deinit();
	exit(EXIT_SUCCESS);
}

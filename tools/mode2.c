/*      $Id: mode2.c,v 5.3 1999/09/13 05:52:41 columbus Exp $      */

/****************************************************************************
 ** mode2.c *****************************************************************
 ****************************************************************************
 *
 * mode2 - shows the pulse/space length of a remote button
 *
 * Copyright (C) 1998 Trent Piepho <xyzzy@u.washington.edu>
 * Copyright (C) 1998 Christoph Bartelmus <lirc@bartelmus.de>
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "drivers/lirc.h"

int main()
{
	int fd;
	lirc_t data;
	
	fd=open(LIRC_DRIVER_DEVICE,O_RDONLY);
	if(fd==-1)  {
		printf("error opening " LIRC_DRIVER_DEVICE "\n");
		perror("mode2");
		exit(1);
	};

	while(1)
	{
		int result;

		result=read(fd,&data,sizeof(data));
		if(result!=sizeof(data))
		{
			fprintf(stderr,"read() failed\n");
			break;
		}
		
		printf("%s %lu\n",(data&PULSE_BIT)?"pulse":"space",
		       (unsigned long) (data&PULSE_MASK));
		fflush(stdout);
	};
	return(0);
}

/*      $Id: mode2.c,v 5.1 1999/05/07 17:50:49 columbus Exp $      */

/****************************************************************************
 ** mode2.c *****************************************************************
 ****************************************************************************
 *
 * mode2 - shows the pulse/space length of a remote button
 *
 * Copyright (C) 1998 Trent Piepho <xyzzy@u.washington.edu>
 * Copyright (C) 1998 Christoph Bartelmus <columbus@hit.handshake.de>
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

int main()
{
	int fd;
	int data;

	fd=open(LIRC_DRIVER_DEVICE,O_RDONLY);
	if(fd==-1)  {
		printf("error opening " LIRC_DRIVER_DEVICE "\n");
		perror("mode2");
		exit(1);
	};

	while(1)
	{
		int result;

		result=read(fd,&data,4);
		if(result!=4)
		{
			fprintf(stderr,"read() failed\n");
			break;
		}
		
		printf("%s %d\n",(data&0x1000000)?"pulse":"space", data&0xffffff);
		fflush(stdout);
	};
	return(0);
}

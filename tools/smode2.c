/*      $Id: smode2.c,v 1.1 1999/04/29 21:17:08 columbus Exp $      */

/****************************************************************************
 ** smode2.c ****************************************************************
 ****************************************************************************
 *
 * smode2 - shows the ir waveform of an IR signal
 *
 * Copyright (C) 1998.11.18 Sinkovics Zoltan <sinko@szarvas.hu>
 *
 * This program is based on the mode2.c file which is a part of the LIRC
 * distribution. The main purpose of this program to check operation of lirc
 * receiver hardware, and to see the ir waveform of the remote controller 
 * without an expensive oscilloscope. The time division is variable from 
 * 1 ms/div to extremly high valiues (integer type) but there is no point about
 * increasing this value above 20 ms/div, because one pulse is about 1 ms.
 * I think this kind of show is much more exciting as the simple pulse&space 
 * showed by mode2.
 *
 * Usage: smode2 [-t (ms/div)] , default division is 5 ms/div
 *
 */ 

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <vga.h>
#include <vgagl.h>

GraphicsContext *screen;
GraphicsContext *physicalscreen;
GraphicsContext *backscreen;

void initscreen(void)
{
    int vgamode;
    
    vga_init();
    
    vgamode = G640x480x16;

    if (!vga_hasmode(vgamode)) {
	printf("Mode not available.\n");
	exit(-1);
    }
    vga_setmode(vgamode);
    
	/* Create virtual screen. */
	gl_setcontextvgavirtual(vgamode);
	backscreen = gl_allocatecontext();
	gl_getcontext(backscreen);

	/* Physical screen context. */
	vga_setmode(vgamode);
	gl_setcontextvga(vgamode);
	physicalscreen = gl_allocatecontext();
	gl_getcontext(physicalscreen);
	
	gl_setcontext(backscreen);
	/*drawgraypalette();*/

	gl_clearscreen(0);

//    gl_setcontextvga(vgamode);
    printf("1\n");
    gl_enableclipping();
    printf("1\n");    
    gl_setclippingwindow(0,0,639,479);
    printf("1\n");
    gl_setwritemode(WRITEMODE_OVERWRITE | FONT_COMPRESSED);
    printf("1\n");
    gl_setfont(8, 8, gl_font8x8);
    printf("1\n");
    gl_setfontcolors(0, 1) ;
    printf("1\n");    
}

void closescreen(void)
{
    vga_setmode(TEXT);
}

int main(int argc, char **argv)
{
	int fd;
	int data;
	int x1,y1,x2,y2;
	int result;
	int c=10;
	char textbuffer[80];
	int d,div=5;

	while ((d = getopt (argc, argv, "t:")) != EOF)
	{
	switch (d)
	    {
	    case 't':				// timediv
		div = strtol(optarg,NULL,10);
		break;
	    }
	}

	fd=open("/dev/lirc",O_RDONLY);
	if(fd==-1)  {
		perror("mode2");
		printf("error opening /dev/lirc\n");
		exit(1);
	};
	
	initscreen();
	
	y1=20;
	x1=x2=0;
	printf("5\n");
	for (y2=0;y2<640;y2+=20) gl_line(y2,0,y2,480,1);
	printf("6\n");
	sprintf(textbuffer,"%d ms/div",div);
	printf("7\n");
	gl_write(500,10,textbuffer);
	printf("7\n");
	gl_copyscreen(physicalscreen);

	while(1)
	{
		result=read(fd,&data,4);
		if (result!=0)
		    {
//		    printf("%.8x\t",data);
		    x2=(data&0xffffff)/(div*50);
		    if (x2>400)
			{
			y1+=15;
			x1=0;
			gl_copyscreen(physicalscreen);
			}
		      else
			{
			if (x1<640) 
			    {
			    gl_line(x1, ((data&0x1000000)?y1:y1+10), x1+x2, ((data&0x1000000)?y1:y1+10), c) ;
			    x1+=x2;
			    gl_line(x1, ((data&0x1000000)?y1:y1+10), x1, ((data&0x1000000)?y1+10:y1), c) ;
			    }
			}
		    if (y1>480) 
			{
			y1=20;
			gl_clearscreen(0);
			for (y2=0;y2<640;y2+=10) gl_line(y2,0,y2,480,1);
			gl_write(500,10,textbuffer);
			}
		    }
//		gl_copyscreen(physicalscreen);
	};
    closescreen();

}

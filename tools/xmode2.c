/*      $Id: xmode2.c,v 5.2 1999/08/25 08:44:04 columbus Exp $      */

/****************************************************************************
 ** xmode2.c ****************************************************************
 ****************************************************************************
 *
 * xmode2 - shows the ir waveform of an IR signal
 *
 * patched together on Feb. 18th 1999 by 
 * Heinrich Langos <heinrich@mad.scientist.com>
 *
 * This program is based on the smode2.c file by Sinkovics Zoltan
 * <sinko@szarvas.hu> which is a part of the LIRC distribution. It is
 * just a conversion from svga to X with some basic support for resizing. 
 * I copied most of this comment. 
 * The main purpose of this program to check operation of lirc
 * receiver hardware, and to see the ir waveform of the remote
 * controller without an expensive oscilloscope. The time division is
 * variable from 1 ms/div to extremly high valiues (integer type) but
 * there is no point about increasing this value above 20 ms/div,
 * because one pulse is about 1 ms.  I think this kind of show is much
 * more exciting as the simple pulse&space showed by mode2.
 *
 * Usage: xmode2 [-t (ms/div)] , default division is 5 ms/div
 * 
 *
 * compile: gcc -o xmode2 xmode2.c -L/usr/X11R6/lib -lX11
 *
 * version 0.01  Feb 18 1999
 *   initial release
 *
 * version 0.02  Aug 24 1999
 *   using select() to make the whole thing more responsive
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>

Display         *d1;
Window          w0,w1; /*w0 = root*/
char            w1_wname[]="xmode2";
char            w1_iname[]="xmode2";
char            font1_name[]="-*-Courier-medium-r-*-*-8-*-*-m-*-iso8859-1";

unsigned int    w1_x=0,w1_y=0,w1_w=640,w1_h=480,w1_border=0;

XFontStruct     *f1_str;
XColor          xc1,xc2;
Colormap        cm1;
XGCValues       gcval1;
GC              gc1,gc2;
XSetWindowAttributes  winatt1;

long            event_mask1; 
XEvent          event_return1; 

void initscreen(void)
{
  d1=XOpenDisplay(0);
  if (d1==NULL)
    {
      printf("Can't open display.\n");
      exit(0);
    }
  


  /*Aufbau der XWindowsAttribStr*/
  w0 = DefaultRootWindow(d1);
  winatt1.background_pixel = BlackPixel(d1,0);
  winatt1.backing_store = WhenMapped;
  winatt1.event_mask = KeyPressMask|StructureNotifyMask;
  w1 = XCreateWindow(d1,w0,w1_x,w1_y,w1_w,w1_h,w1_border,CopyFromParent,InputOutput, CopyFromParent,CWBackPixel|CWBackingStore|CWEventMask,&winatt1);

  XStoreName(d1,w1,w1_wname);
  XSetIconName(d1,w1,w1_iname);
  XMapWindow(d1,w1);

  cm1=DefaultColormap(d1,0);
  if (!XAllocNamedColor(d1,cm1,"blue",&xc1,&xc2)) printf("coudn't allocate blue color\n");
  f1_str=XLoadQueryFont(d1,font1_name);
  if (f1_str==NULL) printf("could't load font\n");

  gcval1.foreground = xc1.pixel;
  gcval1.font = f1_str -> fid;
  gcval1.line_style = LineSolid;
  gc1 = XCreateGC(d1,w1, GCForeground|GCLineStyle, &gcval1);
  gcval1.foreground = WhitePixel(d1,0);
  gc2 = XCreateGC(d1,w1, GCForeground|GCLineStyle|GCFont, &gcval1);
}

void closescreen(void)
{
  XUnmapWindow(d1,w1);
  XCloseDisplay(d1);
}


int main(int argc, char **argv)
{
  fd_set rfds;
  struct timeval tv;
  int retval;
      
  int fd;
  int data;
  int x1,y1,x2,y2;
  int result;
  char textbuffer[80];
  int d,div=5;

  while ((d = getopt (argc, argv, "t:T")) != EOF)
    {
      switch (d)
	{
	case 't':				// timediv
	  div = strtol(optarg,NULL,10);
	  break;
	case 'T':				// timediv
	  div = strtol(optarg,NULL,10);
	  break;
	}
    }

  fd=open(LIRC_DRIVER_DEVICE,O_RDONLY);
  if(fd==-1)  {
    perror("xmode2");
    printf("error opening %s\n",LIRC_DRIVER_DEVICE);
    exit(1);
  };
	
  initscreen();
	
  y1=20;
  x1=x2=0;
  sprintf(textbuffer,"%d ms/div",div);
  for (y2=0;y2<w1_w;y2+=10) XDrawLine(d1,w1,gc1,y2,0,y2,w1_h);
  XDrawString(d1,w1,gc2,w1_w-100,10,textbuffer,strlen(textbuffer));
  XFlush(d1);
  while(1)
    {
      if (XCheckWindowEvent(d1, w1, KeyPressMask|StructureNotifyMask, &event_return1))
	{
	  switch(event_return1.type)
	    {
	    case KeyPress:
	      if (event_return1.xkey.keycode==XKeysymToKeycode(d1,XStringToKeysym("q")))
		{
		  closescreen();
		  exit(1);
		}
	      break;
	    case ConfigureNotify:
	      w1_w=event_return1.xconfigure.width;
	      w1_h=event_return1.xconfigure.height;
	      for (y2=0;y2<w1_w;y2+=10) XDrawLine(d1,w1,gc1,y2,0,y2,w1_h);
	      XDrawString(d1,w1,gc2,w1_w-100,10,textbuffer,strlen(textbuffer));

	      //	      printf("resize \n");
	      break;
	    default:
	      ;
	    }
	} 
      
      
      /* Watch stdin (fd 0) to see when it has input. */
      FD_ZERO(&rfds);
      FD_SET(fd, &rfds);
      /* Wait up to one second. */
      tv.tv_sec = 1;
      tv.tv_usec = 0;
      
      retval = select(fd+1, &rfds, NULL, NULL, &tv);
  
      if (FD_ISSET(fd,&rfds)) {
	result=read(fd,&data,4);
	if (result!=0)
	  {
	    //		    printf("%.8x\t",data);
	    x2=(data&0xffffff)/(div*50);
	    if (x2>400)
	      {
		y1+=15;
		x1=0;
	      }
	    else
	      {
		if (x1<w1_w) 
		  {
		    XDrawLine(d1,w1,gc2,x1, ((data&0x1000000)?y1:y1+10), x1+x2, ((data&0x1000000)?y1:y1+10)) ;
		    x1+=x2;
		    XDrawLine(d1,w1,gc2,x1, ((data&0x1000000)?y1:y1+10), x1, ((data&0x1000000)?y1+10:y1)) ;
		  }
	      }
	    if (y1>w1_h) 
	      {
		y1=20;
		XClearWindow(d1,w1);
		for (y2=0;y2<w1_w;y2+=10) XDrawLine(d1,w1,gc1,y2,0,y2,w1_h);
		XDrawString(d1,w1,gc2,w1_w-100,10,textbuffer,strlen(textbuffer));
	      }
	  }
      }
      //	       	gl_copyscreen(physicalscreen);
      //      XFlush(d1);
    };
}

/*      $Id: hw_pixelview.h,v 5.1 1999/08/02 19:56:49 columbus Exp $      */

/****************************************************************************
 ** hw_pixelview.h **********************************************************
 ****************************************************************************
 *
 * routines for PixelView Play TV receiver
 * 
 * Copyright (C) 1999 Christoph Bartelmus <lirc@bartelmus.de>
 *
 */

#ifndef _HW_DEFAULT_H
#define _HW_DEFAULT_H

int pixelview_decode(struct ir_remote *remote,
		  ir_code *prep,ir_code *codep,ir_code *postp,
		  int *repeat_flagp,unsigned long *remaining_gapp);
int pixelview_init(void);
int pixelview_deinit(void);
char *pixelview_rec(struct ir_remote *remotes);

#endif

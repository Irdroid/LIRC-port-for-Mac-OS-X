/*      $Id: hw_logitech.h,v 1.1 1999/08/12 18:49:17 columbus Exp $      */

/****************************************************************************
 ** hw_logitech.h **********************************************************
 ****************************************************************************
 *
 * routines for Logitech receiver 
 * 
 * Copyright (C) 1999 Christoph Bartelmus <lirc@bartelmus.de>
 *	modified for logitech receiver by Isaac Lauer <inl101@alumni.psu.edu>
 */

#ifndef _HW_DEFAULT_H
#define _HW_DEFAULT_H

int logitech_decode(struct ir_remote *remote,
		  ir_code *prep,ir_code *codep,ir_code *postp,
		  int *repeat_flagp,unsigned long *remaining_gapp);
int logitech_init(void);
int logitech_deinit(void);
char *logitech_rec(struct ir_remote *remotes);

#endif

/*      $Id: serial.h,v 5.1 1999/08/02 19:56:49 columbus Exp $      */

/****************************************************************************
 ** serial.c ****************************************************************
 ****************************************************************************
 *
 * common routines for hardware that uses the standard serial port driver
 * 
 * Copyright (C) 1999 Christoph Bartelmus <lirc@bartelmus.de>
 *
 */

#ifndef _SERIAL_H
#define _SERIAL_H

int tty_reset(int fd);
int tty_setbaud(int fd,int baud);
int tty_create_lock(char *name);
int tty_delete_lock(void);

#endif

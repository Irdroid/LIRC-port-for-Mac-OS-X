/*      $Id: hw_default.h,v 5.2 1999/08/12 18:45:59 columbus Exp $      */

/****************************************************************************
 ** hw_default.h ************************************************************
 ****************************************************************************
 *
 * routines for hardware that supports ioctl() interface
 * 
 * Copyright (C) 1999 Christoph Bartelmus <lirc@bartelmus.de>
 *
 */

#ifndef _HW_DEFAULT_H
#define _HW_DEFAULT_H

#include "ir_remote.h"

#define WBUF_SIZE (256)
#define RBUF_SIZE (256)

#define REC_SYNC 8

struct sbuf
{
	unsigned long data[WBUF_SIZE];
	int wptr;
	int too_long;
	int is_shift;
	unsigned long pendingp;
	unsigned long pendings;
	unsigned long sum;
};

struct rbuf
{
	unsigned long data[RBUF_SIZE];
	ir_code decoded;
	int rptr;
	int wptr;
	int too_long;
	int is_shift;
	unsigned long pendingp;
	unsigned long pendings;
	unsigned long sum;
};

inline void set_bit(ir_code *code,int bit,int data);
inline unsigned long time_left(struct timeval *current,struct timeval *last,
			       unsigned long gap);
int init_send(struct ir_remote *remote,struct ir_ncode *code);

int default_init(void);
int default_deinit(void);
int write_send_buffer(int lirc,int length,unsigned long *signals);
int default_send(struct ir_remote *remote,struct ir_ncode *code);
char *default_rec(struct ir_remote *remotes);
int default_decode(struct ir_remote *remote,
		   ir_code *prep,ir_code *codep,ir_code *postp,
		   int *repeat_flag,unsigned long *remaining_gapp);
int clear_rec_buffer(void);
void rewind_rec_buffer(void);


#endif

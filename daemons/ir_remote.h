/*      $Id: ir_remote.h,v 5.1 1999/05/05 14:57:55 columbus Exp $      */

/****************************************************************************
 ** ir_remote.h *************************************************************
 ****************************************************************************
 *
 * ir_remote.h - describes and decodes the signals from IR remotes
 *
 * Copyright (C) 1996,97 Ralph Metzler <rjkm@thp.uni-koeln.de>
 * Copyright (C) 1998 Christoph Bartelmus <columbus@hit.handshake.de>
 *
 */ 

#ifndef _IR_REMOTE_H
#define _IR_REMOTE_H

#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#ifdef LONG_IR_CODE
typedef unsigned long long ir_code;
#else
typedef unsigned long ir_code;
#endif

#define WBUF_SIZE (256)
#define RBUF_SIZE (256)

#define PULSE_BIT 0x1000000

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
	int rptr;
	int wptr;
	int too_long;
	int is_shift;
	unsigned long pendingp;
	unsigned long pendings;
	unsigned long sum;
};

/*
  Code with name string
*/

struct ir_ncode {
	char *name;
	ir_code code;
        int length;
        unsigned long *signals;
};

/*
  struct ir_remote
  defines the encoding of a remote control 
*/

/* definitions for flags */

/*
  Don't forget to take a look at parseFlags in read_config
  when adding new flags
*/

#define NR_FLAGS 7

#define SHIFT_ENC	0x0001    /* IR data is shift encoded */
#define SPACE_ENC	0x0002	  /* IR data is space encoded */
#define REVERSE		0x0004
#define NO_HEAD_REP	0x0008	  /* no header for key repeates */
#define NO_FOOT_REP	0x0010	  /* no foot for key repeates */
#define CONST_LENGTH    0x0020    /* signal length+gap is always constant */
#define RAW_CODES       0x0040    /* for internal use only */
#define REPEAT_HEADER   0x0080    /* header is also sent before repeat code */

struct ir_remote 
{
	char *name;                 /* name of remote control */
	struct ir_ncode *codes;
	int bits;                   /* bits (length of code) */
	int flags;                  /* flags */
	int eps;                    /* eps (_relative_ tolerance) */
	int aeps;                   /* detecing _very short_ pulses is
				       difficult with relative tolerance
				       for some remotes,
				       this is an _absolute_ tolerance
				       to solve this problem
				       usually you can say 0 here */
	
	/* pulse and space lengths of: */
	
	int phead,shead;            /* header */
	int pone,sone;              /* 1 */
	int pzero,szero;            /* 0 */
	int plead;		    /* leading pulse */
	int ptrail;                 /* trailing pulse */
	int pfoot,sfoot;            /* foot */
	int prepeat,srepeat;	    /* indicate repeating */

	int pre_data_bits;          /* length of pre_data */
	ir_code pre_data;           /* data which the remote sends before
				       actual keycode */
	int post_data_bits;         /* length of post_data */
	ir_code post_data;          /* data which the remote sends after
				       actual keycode */
	int pre_p,pre_s;            /* signal between pre_data and keycode */
	int post_p, post_s;         /* signal between keycode and post_code */

	unsigned long gap;          /* time between signals in usecs */
	unsigned long repeat_gap;   /* time between two repeat codes
				       if different from gap */
	int repeat_bit;             /* 1..bits */


	/* end of user editable values */

        int repeat_state;
	struct ir_ncode *last_code;
	int reps;
	struct timeval last_send;
	unsigned long remaining_gap;/* remember gap for CONST_LENGTH remotes */
        struct ir_remote *next;
};

static inline int is_shift(struct ir_remote *remote)
{
	if(remote->flags&SHIFT_ENC) return(1);
	else return(0);
}

static inline int is_raw(struct ir_remote *remote)
{
	if(remote->flags&RAW_CODES) return(1);
	else return(0);
}

static inline int is_const(struct ir_remote *remote)
{
	if(remote->flags&CONST_LENGTH) return(1);
	else return(0);
}

static inline int has_header(struct ir_remote *remote)
{
	if(remote->phead>0 && remote->shead>0) return(1);
	else return(0);
}

static inline int has_foot(struct ir_remote *remote)
{
	if(remote->pfoot>0 && remote->sfoot>0) return(1);
	else return(0);
}

static inline int has_repeat(struct ir_remote *remote)
{
	if(remote->prepeat>0 && remote->srepeat>0) return(1);
	else return(0);
}

static inline int has_repeat_gap(struct ir_remote *remote)
{
	if(remote->repeat_gap>0) return(1);
	else return(0);
}

static inline int has_pre(struct ir_remote *remote)
{
	if(remote->pre_data_bits>0) return(1);
	else return(0);
}

static inline int has_post(struct ir_remote *remote)
{
	if(remote->post_data_bits>0) return(1);
	else return(0);
}

static inline int is_pulse(unsigned long data)
{
	return(data&PULSE_BIT ? 1:0);
}

static inline int is_space(unsigned long data)
{
	return(!is_pulse(data));
}

/* check if delta is inside exdelta +/- exdelta*eps/100 */

static inline int expect(struct ir_remote *remote,int delta,int exdelta)
{
	if(abs(exdelta-delta)<exdelta*remote->eps/100 ||
	   abs(exdelta-delta)<remote->aeps)
		return 1;
	return 0;
}

struct ir_remote *get_ir_remote(char *name);
struct ir_ncode *get_ir_code(struct ir_remote *remote,char *name);
inline unsigned long time_left(struct timeval *current,struct timeval *last,
			       unsigned long gap);
void send_command(struct ir_remote *remote,struct ir_ncode *code);
void clear_rec_buffer(unsigned long data);
int decode(struct ir_remote *remote);
char *decode_command(unsigned long data);

extern struct ir_remote *repeat_remote;
extern struct ir_ncode *repeat_code;
extern struct ir_remote *last_remote;

extern struct ir_remote *remotes,*free_remotes,*decoding;

extern int return_code;
extern unsigned long send_mode,rec_mode;
extern unsigned long code_length;
#endif

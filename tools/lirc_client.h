/*      $Id: lirc_client.h,v 5.0 1999/04/29 21:30:59 columbus Exp $      */

/****************************************************************************
 ** lirc_client.h ***********************************************************
 ****************************************************************************
 *
 * lirc_client - common routines for lircd clients
 *
 * Copyright (C) 1998 Trent Piepho <xyzzy@u.washington.edu>
 * Copyright (C) 1998 Christoph Bartelmus <columbus@hit.handshake.de>
 *
 */ 
 
#ifndef LIRC_CLIENT_H
#define LIRC_CLIENT_H

#define LIRC_ALL ((char *) (-1))

enum lirc_flags {none=0x00,
		 once=0x01,
		 quit=0x02,
		 mode=0x04,
		 ecno=0x08
};

struct lirc_list
{
	char *string;
	struct lirc_list *next;
};

struct lirc_code
{
	char *remote;
	char *button;
	struct lirc_code *next;
};

struct lirc_config
{
	char *current_mode;
	struct lirc_config_entry *next;
	struct lirc_config_entry *first;
};

struct lirc_config_entry
{
	char *prog;
	struct lirc_code *code;
	unsigned int rep;
	struct lirc_list *config;
	char *change_mode;
	unsigned int flags;
	
	char *mode;
	struct lirc_list *next_config;
	struct lirc_code *next_code;

	struct lirc_config_entry *next;
};

int lirc_init(char *prog);
int lirc_deinit();

int lirc_readconfig(char *file,
		    struct lirc_config **config,
		    int (check)(char *s));
void lirc_freeconfig(struct lirc_config *config);
void lirc_freeconfigentries(struct lirc_config_entry *first);

void lirc_clearmode(struct lirc_config *config);
char *lirc_execute(struct lirc_config *config,struct lirc_config_entry *scan);
int lirc_iscode(struct lirc_config_entry *scan,char *remote,char *button,int rep);
char *lirc_nextir();
char *lirc_ir2char(struct lirc_config *config,char *string);

extern char *progname;

#endif

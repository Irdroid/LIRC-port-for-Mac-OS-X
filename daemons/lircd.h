/*      $Id: lircd.h,v 5.3 1999/08/02 19:56:49 columbus Exp $      */

/****************************************************************************
 ** lircd.h *****************************************************************
 ****************************************************************************
 *
 */

#ifndef _LIRCD_H
#define _LIRCD_H

#include "ir_remote.h"

#define PACKET_SIZE (256)
#define WHITE_SPACE " \t"

void sigterm(int sig);
void sighup(int sig);
void config(void);
void nolinger(int sock);
void remove_client(int fd);
void add_client(void);
void start_server(void);
void logprintf(int level,char *format_str, ...);
void logperror(int level,const char *s);
void daemonize(void);
void sigalrm(int sig);
int parse_rc(int fd,char *message,char *arguments,struct ir_remote **remote,
	     struct ir_ncode **code,int n);
int send_success(int fd,char *message);
int send_error(int fd,char *message,char *format_str, ...);
int send_remote_list(int fd,char *message);
int send_remote(int fd,char *message,struct ir_remote *remote);
int send_name(int fd,char *message,struct ir_ncode *code);
int list(int fd,char *message,char *arguments);
int send_once(int fd,char *message,char *arguments);
int send_start(int fd,char *message,char *arguments);
int send_stop(int fd,char *message,char *arguments);
int version(int fd,char *message,char *arguments);
int get_pid(int fd,char *message,char *arguments);
int get_command(int fd);
int waitfordata(unsigned long maxusec);
void loop(void);


struct protocol_directive
{
	char *name;
	int (*function)(int fd,char *message,char *arguments);
};

#endif _LIRCD_H

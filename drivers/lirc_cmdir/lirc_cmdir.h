/*      $Id: lirc_cmdir.h,v 1.1 2005/07/10 08:34:13 lirc Exp $      */

/*
 *   lirc_cmdir.h
 */

#ifndef LIRC_CMDIR_H
#define LIRC_CMDIR_H

#define ON          1
#define OFF         0

/* transmitter channel control */
#define MAX_CHANNELS     32

/* CommandIR control codes */
#define MCU_CTRL_SIZE   3
#define FREQ_HEADER     2
#define TX_HEADER       7
 

extern int cmdir_write(unsigned char *buffer, int count, void *callback_fct);
extern ssize_t cmdir_read (unsigned char *buffer, size_t count);
extern int set_tx_channels (unsigned int next_tx);

#endif

#ifndef REMOTE_H
#define REMOTE_H
/*
   $Id: lirc_fly98.h,v 1.1 1999/07/26 09:53:58 jochym Exp $
*/
/*
 * This driver is for a FlyVideo'98 (FlyVideoII/ConferenceTV ?) 
 * card IR Remote Control
 * (L) Pawel T. Jochym <jochym@ifj.edu.pl>
 *     http://wolf.ifj.edu.pl/~jochym/FlyVideo98
 */

#define BT848_RMTCTL_MASK 0x1F8
#define BT848_RMTCTL_RELEASED_MASK 0x100
#define BT848_RMTCTL_SHIFT 3

/* We are using experimental range for now */
#define IRCTL_DEV_MAJOR 61 
#define MAX_IRCTL_DEVICES 4
#define IRCTL_DEV_NAME "RemoteCtl"
#define BUFLEN 512

struct irctl 
{
  /* Buffer for key codes */
  unsigned char buffer[BUFLEN];
  /* Consumer/Producer pointers */ 
  int head, tail, open;
  spinlock_t lock;
  struct wait_queue *wait;
};

extern void remote_queue_key(int nr, unsigned long code);

#endif /* REMOTE_H */








#ifndef REMOTE_H
#define REMOTE_H
/*
   $Id: lirc_fly98.h,v 1.4 1999/08/03 11:53:37 jochym Exp $
*/
/*
 * This driver is for a FlyVideo'98/FlyVideoII/ConferenceTV 
 * card IR Remote Control
 * (L) Pawel T. Jochym <jochym@ifj.edu.pl>
 *     http://wolf.ifj.edu.pl/~jochym/
 */

#include <linux/version.h>

#if LINUX_VERSION_CODE < 0x020200
/* We need the latest bttv.c in the kernel */
#error "--- A 2.2.x kernel is required to use this module ---"
#endif

#include <linux/ioctl.h>
#include "../lirc.h"

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
typedef void (*bttv_gpio_monitor_t)(int nr, unsigned long data); 
extern bttv_gpio_monitor_t bttv_set_gpio_monitor(bttv_gpio_monitor_t callback);

#endif /* REMOTE_H */








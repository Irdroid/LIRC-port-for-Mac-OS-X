#ifndef REMOTE_H
#define REMOTE_H
/*
   $Id: lirc_fly98.h,v 1.2 1999/07/27 09:17:26 jochym Exp $
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

#define BT848_RMTCTL_MASK 0x1F8
#define BT848_RMTCTL_RELEASED_MASK 0x100
#define BT848_RMTCTL_SHIFT 3

/* lirc.h constants */
#define LIRC_MODE_RAW                  0x00000001
#define LIRC_MODE_PULSE                0x00000002
#define LIRC_MODE_MODE2                0x00000004
#define LIRC_MODE_CODE                 0x00000008
#define LIRC_MODE_LIRCCODE             0x00000010
#define LIRC_MODE_STRING               0x00000020

#define LIRC_MODE2REC(x) ((x) << 16)
#define LIRC_CAN_REC_CODE              LIRC_MODE2REC(LIRC_MODE_CODE)

#define LIRC_GET_FEATURES              _IOR('i', 0x00000000, __u32)
#define LIRC_GET_REC_MODE              _IOR('i', 0x00000002, __u32)
#define LIRC_SET_REC_MODE              _IOW('i', 0x00000012, __u32)


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








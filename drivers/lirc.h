#ifndef LIRC_AVER_H
#define LIRC_AVER_H

#include <linux/ioctl.h>

#define DEVICE_NAME "remote"
#define TRUE  1
#define FALSE 0
#define LIRC_MODE_DECODED     _IO('l', 0)
#define LIRC_MODE_PULSE_SPACE _IO('l', 1)

#endif

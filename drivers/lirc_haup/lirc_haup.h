#ifndef _LIRC_HAUP_H_
#define _LIRC_HAUP_H_

/*
 * Header file for hauppauge 
 */

/* Scanning range for I2C bus ports */

#define IR_READ_LOW  0x31
#define IR_READ_HIGH 0x35

/*
 * The polling rate in jiffies of the i2c status.
 *
 * i.e. HZ jiffies are one second, or HZ/POLLING is the rate in seconds
 */
#define POLLING    (HZ / 20)
#define I2C_DRIVERID_REMOTE 4

/*
 * If this key changes, a new key was pressed.
 */
#define REPEAT_TOGGLE_0      192
#define REPEAT_TOGGLE_1      224

/*
 * code length
 */
#define CODE_LENGTH 13

/* return values */

#define SUCCESS 0

/*
 * KEY SEQUENCES.
 * BYTE 2 contains the last key pressed. The mapping is
 */
#define REMOTE_0           0x00
#define REMOTE_1           0x04
#define REMOTE_2           0x08
#define REMOTE_3           0x0c
#define REMOTE_4           0x10
#define REMOTE_5           0x14
#define REMOTE_6           0x18
#define REMOTE_7           0x1c
#define REMOTE_8           0x20
#define REMOTE_9           0x24
#define REMOTE_RADIO       0x30
#define REMOTE_MUTE        0x34
#define REMOTE_TV          0x3c
#define REMOTE_VOL_PLUS    0x40
#define REMOTE_VOL_MINUS   0x44
#define REMOTE_RESERVED    0x78
#define REMOTE_CHAN_PLUS   0x80
#define REMOTE_CHAN_MINUS  0x84
#define REMOTE_SOURCE      0x88
#define REMOTE_MINIMIZE    0x98
#define REMOTE_FULL_SCREEN 0xb8

#endif /* _LIRC_HAUP_H_ */



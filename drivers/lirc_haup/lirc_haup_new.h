#ifndef _LIRC_HAUP_H_
#define _LIRC_HAUP_H_

/*
 * Header file for hauppauge 
 */

/*
 * The polling rate in jiffies of the i2c status.
 *
 * i.e. HZ jiffies are one second, or HZ/POLLING is the rate in seconds
 */
#define POLLING    (HZ / 20)
#define I2C_DRIVERID_REMOTE 42

#define BUFLEN 256  /* read buffer length */

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
 * Hauppauge uses these definitions internally in transmitting keypresses.
 * BYTE 2 contains the last key pressed.
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

/* soft irq */
static void         lirc_haup_do_timer(unsigned long data);

/* fops hooks */
static int          lirc_haup_open(struct inode *inode, struct file *file);
static int          lirc_haup_close(struct inode *inode, struct file *file);
static ssize_t      lirc_haup_read(struct file * file, char * buffer,
				   size_t count, loff_t *ppos);
static ssize_t      lirc_haup_write(struct file * file, const char * buffer,
				    size_t count, loff_t *ppos);
static unsigned int lirc_haup_poll(struct file *file,
				   struct poll_table_struct * wait);
static int          lirc_haup_ioctl(struct inode *, struct file *,
				    unsigned int cmd, unsigned long arg);

/* i2c stuff */
static int remote_attach (struct i2c_adapter *adap, int addr,
	unsigned short flags, int kind);
static int remote_probe (struct i2c_adapter *adap);
static int remote_detach (struct i2c_client *client);
static int remote_command (struct i2c_client *client,
	unsigned int cmd, void *arg);

#endif /* _LIRC_HAUP_H_ */


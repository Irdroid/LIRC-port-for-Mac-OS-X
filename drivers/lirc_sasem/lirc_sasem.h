/*      $Id: lirc_sasem.h,v 1.2 2005/03/28 17:05:41 lirc Exp $      */

#ifndef LIRC_SASEM_H
#define LIRC_SASEM_H

#include "drivers/kcompat.h"

/*
 * Version Information
 */
#define DRIVER_VERSION  "v0.3"
#define DATE            "Nov 2004"
#define DRIVER_AUTHOR 		"Oliver Stabel <oliver.stabel@gmx.de>"
#define DRIVER_DESC 		"USB Driver for Sasem Remote Controller V1.1"
#define DRIVER_SHORTDESC 	"Sasem"
#define DRIVER_NAME		"lirc_sasem"

#define BANNER \
  KERN_INFO DRIVER_SHORTDESC " " DRIVER_VERSION " (" DATE ")\n" \
  KERN_INFO "   by " DRIVER_AUTHOR "\n"

static const char longbanner[] = {
	DRIVER_DESC ", " DRIVER_VERSION " (" DATE "), by " DRIVER_AUTHOR
};

#define MAX_INTERRUPT_DATA 8
#define SASEM_MINOR 144

#include <linux/version.h>
#include <linux/time.h>

static const char sc_cSasemCode[MAX_INTERRUPT_DATA] =
	{ 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

#ifdef KERNEL_2_5
typedef struct usb_class_driver t_usb_class_driver, *tp_usb_class_driver;
#endif
typedef struct usb_driver t_usb_driver, *tp_usb_driver;
typedef struct usb_device t_usb_device, *tp_usb_device;
typedef struct usb_interface t_usb_interface, *tp_usb_interface;
typedef struct usb_device_id t_usb_device_id, *tp_usb_device_id;
typedef struct usb_host_interface t_usb_host_interface,
	*tp_usb_host_interface;
typedef struct usb_interface_descriptor t_usb_interface_descriptor,
	*tp_usb_interface_descriptor;
typedef struct usb_endpoint_descriptor t_usb_endpoint_descriptor,
	*tp_usb_endpoint_descriptor;
typedef struct urb t_urb, *tp_urb;

typedef struct semaphore t_semaphore, *tp_semaphore;

typedef struct lirc_plugin t_lirc_plugin, *tp_lirc_plugin;
typedef struct lirc_buffer t_lirc_buffer;

struct sasemDevice {
	t_usb_device *m_psDevice;
	t_usb_endpoint_descriptor *m_psDescriptorIn;
	t_usb_endpoint_descriptor *m_psDescriptorOut;
	t_urb *m_psUrbIn;
	t_urb *m_psUrbOut;
	struct timeval m_sPressTime;
	unsigned int m_uiInterfaceNum;
	int	m_iDevnum;
	unsigned char m_aucBufferIn[MAX_INTERRUPT_DATA];
	unsigned char m_aucBufferOut[MAX_INTERRUPT_DATA];
	t_semaphore m_sSemLock;

	char m_acLastCode[MAX_INTERRUPT_DATA];
	int m_iCodeSaved;
	
	/* lirc */
	t_lirc_plugin *m_psLircPlugin;
	int m_iConnected;

	/* LCDProc */
	int m_iOpen;
	wait_queue_head_t m_sQueueWrite;
	wait_queue_head_t m_sQueueOpen;
};

typedef struct sasemDevice t_sasemDevice, *tp_sasemDevice;

#ifndef KERNEL_2_5
static void * s_SasemProbe(t_usb_device *p_psDevice, unsigned p_uInterfaceNum,
		const t_usb_device_id *p_psID);
static void s_SasemDisconnect(t_usb_device *p_psDevice, void *p_pPtr);
static void s_SasemCallbackIn(t_urb *p_psUrb);
static void s_SasemCallbackOut(t_urb *p_psUrb);
#else
static int s_SasemProbe(t_usb_interface *p_psInt, const t_usb_device_id *p_psID);
static void s_SasemDisconnect(t_usb_interface *p_psInt); 
static void s_SasemCallbackIn(t_urb *p_psUrb, struct pt_regs *regs);
static void s_SasemCallbackOut(t_urb *p_psUrb, struct pt_regs *regs);
#endif

/* lirc */
static int s_iUnregisterFromLirc(t_sasemDevice *p_psSasemDevice);
static int s_iLircSetUseInc(void *p_pData);
static void s_iLircSetUseDec(void *p_pData);

/* fs for LCDProc */
typedef struct file_operations t_file_operations, *tp_file_operations;
typedef struct inode t_inode, *tp_inode;
typedef struct file t_file, *tp_file;

#define IOCTL_GET_HARD_VERSION  1
#define IOCTL_GET_DRV_VERSION   2

static int s_iSasemFSOpen(t_inode *p_psInode, t_file *p_psFile);
static int s_iSasemFSRelease(t_inode *p_psInode, t_file *p_psFile);
static ssize_t s_sSasemFSWrite(t_file *p_psFile, const char *p_pcBuffer,
			size_t p_psCount, loff_t *p_psPos);
static ssize_t s_sSasemFSRead(t_file *p_psFile, char *p_pcBuffer,
			size_t p_psCount, loff_t *p_psUnused_pos);
static int s_iSasemFSIoctl(t_inode *p_psInode, t_file *p_psFile,
			unsigned p_uCmd, unsigned long p_ulArg);
static unsigned s_uSasemFSPoll(t_file *p_psFile, poll_table *p_psWait);

#endif

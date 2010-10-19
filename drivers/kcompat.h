/*      $Id: kcompat.h,v 5.50 2010/07/27 05:43:21 jarodwilson Exp $      */

#ifndef _KCOMPAT_H
#define _KCOMPAT_H

#include <linux/version.h>

#ifndef __func__
#define __func__ __FUNCTION__
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34)
#define usb_alloc_coherent usb_buffer_alloc
#define usb_free_coherent usb_buffer_free
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 16)
#define LIRC_THIS_MODULE(x) x,
#else /* >= 2.6.16 */
#define LIRC_THIS_MODULE(x)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)

#include <linux/device.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 18)
#define LIRC_HAVE_DEVFS
#define LIRC_HAVE_DEVFS_26
#endif

#define LIRC_HAVE_SYSFS

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 13)

typedef struct class_simple lirc_class_t;

static inline lirc_class_t *class_create(struct module *owner, char *name)
{
	return class_simple_create(owner, name);
}

static inline void class_destroy(lirc_class_t *cls)
{
	class_simple_destroy(cls);
}

#define lirc_device_create(cs, parent, dev, drvdata, fmt, args...) \
	class_simple_device_add(cs, dev, parent, fmt, ## args)

static inline void lirc_device_destroy(lirc_class_t *cls, dev_t devt)
{
	class_simple_device_remove(devt);
}

#else /* >= 2.6.13 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 15)

#define lirc_device_create(cs, parent, dev, drvdata, fmt, args...) \
	class_device_create(cs, dev, parent, fmt, ## args)

#else /* >= 2.6.15 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 26)

#define lirc_device_create(cs, parent, dev, drvdata, fmt, args...) \
	class_device_create(cs, NULL, dev, parent, fmt, ## args)

#else /* >= 2.6.26 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 27)

#define lirc_device_create(cs, parent, dev, drvdata, fmt, args...) \
	device_create(cs, parent, dev, fmt, ## args)

#else /* >= 2.6.27 */

#define lirc_device_create device_create

#endif /* >= 2.6.27 */

#endif /* >= 2.6.26 */

#define LIRC_DEVFS_PREFIX

#endif /* >= 2.6.15 */

typedef struct class lirc_class_t;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 26)

#define lirc_device_destroy class_device_destroy

#else

#define lirc_device_destroy device_destroy

#endif

#endif /* >= 2.6.13 */

#endif

#ifndef LIRC_DEVFS_PREFIX
#define LIRC_DEVFS_PREFIX "usb/"
#endif

#include <linux/moduleparam.h>

/* DevFS header */
#ifdef LIRC_HAVE_DEVFS
#include <linux/devfs_fs_kernel.h>
#endif

#ifndef LIRC_HAVE_SYSFS
#define class_destroy(x) do { } while (0)
#define class_create(x, y) NULL
#define lirc_device_destroy(x, y) do { } while (0)
#define lirc_device_create(x, y, z, xx, yy, zz) 0
#define IS_ERR(x) 0
typedef struct class_simple
{
	int notused;
} lirc_class_t;
#endif /* No SYSFS */

#include <linux/interrupt.h>
#ifndef IRQ_RETVAL
typedef void irqreturn_t;
#define IRQ_NONE
#define IRQ_HANDLED
#define IRQ_RETVAL(x)
#endif

#ifndef MOD_IN_USE
#ifdef CONFIG_MODULE_UNLOAD
#define MOD_IN_USE module_refcount(THIS_MODULE)
#else
#error "LIRC modules currently require"
#error "  'Loadable module support  --->  Module unloading'"
#error "to be enabled in the kernel"
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 18)
#if !defined(local_irq_save)
#define local_irq_save(flags) do { save_flags(flags); cli(); } while (0)
#endif
#if !defined(local_irq_restore)
#define local_irq_restore(flags) do { restore_flags(flags); } while (0)
#endif
#endif

#if KERNEL_VERSION(2, 4, 0) <= LINUX_VERSION_CODE
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 22)
#include <linux/pci.h>
static inline char *pci_name(struct pci_dev *pdev)
{
	return pdev->slot_name;
}
#endif /* kernel < 2.4.22 */
#endif /* kernel >= 2.4.0 */

/*************************** I2C specific *****************************/
#include <linux/i2c.h>

/* removed in 2.6.14 */
#ifndef I2C_ALGO_BIT
#   define I2C_ALGO_BIT 0
#endif

/* removed in 2.6.16 */
#ifndef I2C_DRIVERID_EXP3
#  define I2C_DRIVERID_EXP3 0xf003
#endif

/*************************** USB specific *****************************/
#include <linux/usb.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 8)
static inline int usb_kill_urb(struct urb *urb)
{
	return usb_unlink_urb(urb);
}
#endif

/* removed in 2.6.14 */
#ifndef URB_ASYNC_UNLINK
#define URB_ASYNC_UNLINK 0
#endif

/******************************* pm.h *********************************/

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 11)
typedef u32 pm_message_t;
#endif /* kernel < 2.6.11 */
#endif /* kernel >= 2.6.0 */

/*************************** pm_wakeup.h ******************************/
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 27)
static inline void device_set_wakeup_capable(struct device *dev, int val) {}
#endif /* kernel < 2.6.27 */

/*************************** interrupt.h ******************************/
/* added in 2.6.18, old defines removed in 2.6.24 */
#ifndef IRQF_DISABLED
#define IRQF_DISABLED SA_INTERRUPT
#endif
#ifndef IRQF_SHARED
#define IRQF_SHARED SA_SHIRQ
#endif

/*************************** spinlock.h *******************************/
/* added in 2.6.11 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 11)
#define DEFINE_SPINLOCK(x) spinlock_t x = SPIN_LOCK_UNLOCKED
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 18)
#define __SPIN_LOCK_UNLOCKED(x) SPIN_LOCK_UNLOCKED;
#endif

/***************************** slab.h *********************************/
/* added in 2.6.14 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 14)
static inline void *kzalloc(size_t size, gfp_t flags)
{
        void *ret = kmalloc(size, flags);
        if (ret)
                memset(ret, 0, size);
        return ret;
}
#endif

/****************************** bitops.h **********************************/
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
#define BIT_MASK(nr)            (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)            ((nr) / BITS_PER_LONG)
#endif

/****************************** kernel.h **********************************/
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 29)
#define DIV_ROUND_CLOSEST(x, divisor)(                  \
{                                                       \
        typeof(divisor) __divisor = divisor;            \
        (((x) + ((__divisor) / 2)) / (__divisor));      \
}                                                       \
)
#endif

#endif /* _KCOMPAT_H */

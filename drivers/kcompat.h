/*      $Id: kcompat.h,v 5.7 2004/04/27 18:52:33 lirc Exp $      */

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 3, 0)
#include <linux/timer.h>
#include <linux/interrupt.h>
static inline void del_timer_sync(struct timer_list * timerlist)
{
	start_bh_atomic();
	del_timer(timerlist);
	end_bh_atomic();
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
#undef daemonize
#define daemonize(name) do {                                           \
                                                                       \
	lock_kernel();                                                 \
	                                                               \
	exit_mm(current);                                              \
	exit_files(current);                                           \
	exit_fs(current);                                              \
	current->session = 1;                                          \
	current->pgrp = 1;                                             \
	current->euid = 0;                                             \
	current->tty = NULL;                                           \
	sigfillset(&current->blocked);                                 \
	                                                               \
	strcpy(current->comm, name);                                   \
	                                                               \
	unlock_kernel();                                               \
                                                                       \
} while (0)

#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 5, 0)
#define KERNEL_2_5

#undef MOD_INC_USE_COUNT
#define MOD_INC_USE_COUNT try_module_get(THIS_MODULE)
#undef MOD_DEC_USE_COUNT
#define MOD_DEC_USE_COUNT module_put(THIS_MODULE)

#endif

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

#if !defined(local_irq_save)
#define local_irq_save(flags) do{ save_flags(flags);cli(); } while(0)
#endif
#if !defined(local_irq_restore)
#define local_irq_restore(flags) do{ restore_flags(flags); } while(0)
#endif

#if !defined(pci_pretty_name)
#define pci_pretty_name(dev) ((dev)->name)
#endif

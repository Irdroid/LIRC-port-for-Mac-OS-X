/*      $Id: kcompat.h,v 5.2 2004/03/28 15:20:54 lirc Exp $      */

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
#define daemonize(name)                                                \
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
	unlock_kernel();

#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 5, 0)
#define KERNEL_2_5
#endif

#ifndef IRQ_RETVAL
typedef void irqreturn_t;
#define IRQ_NONE
#define IRQ_HANDLED
#define IRQ_RETVAL(x)
#endif

#ifndef MOD_IN_USE
#define MOD_IN_USE module_refcount(THIS_MODULE)
#endif

#if !defined(local_irq_save)
#define local_irq_save(flags) save_flags(flags);cli()
#endif
#if !defined(local_irq_restore)
#define local_irq_restore(flags) restore_flags(flags)
#endif

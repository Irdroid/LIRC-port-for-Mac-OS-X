/*      $Id: kcompat.h,v 5.1 2002/10/12 15:31:47 ranty Exp $      */

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 3, 0)
#include <linux/timer.h>
static inline void del_timer_sync(struct timer_list * timerlist)
{
	start_bh_atomic();
	del_timer(timerlist);
	end_bh_atomic();
}
#endif


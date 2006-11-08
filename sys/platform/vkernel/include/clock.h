/*
 * Kernel interface to machine-dependent clock driver.
 * Garrett Wollman, September 1994.
 * This file is in the public domain.
 *
 * $FreeBSD: src/sys/i386/include/clock.h,v 1.38.2.1 2002/11/02 04:41:50 iwasaki Exp $
 * $DragonFly: src/sys/platform/vkernel/include/clock.h,v 1.1 2006/11/08 16:40:00 dillon Exp $
 */

#ifndef _MACHINE_CLOCK_H_
#define	_MACHINE_CLOCK_H_

#ifdef _KERNEL

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif

/*
 * i386 to clock driver interface.
 * XXX large parts of the driver and its interface are misplaced.
 */
extern int	adjkerntz;
extern int	disable_rtc_set;
extern int	statclock_disable;
extern u_int	timer_freq;
extern int	timer0_max_count;
extern int	tsc_present;
extern u_int	tsc_freq;
extern int	tsc_is_broken;
extern int	wall_cmos_clock;
#ifdef APIC_IO
extern int	apic_8254_intr;
#endif

/*
 * Driver to clock driver interface.
 */

int	rtcin (int val);
int	acquire_timer2 (int mode);
int	release_timer2 (void);
int	sysbeep (int pitch, int period);
void	timer_restore (void);

#endif /* _KERNEL */

#endif /* !_MACHINE_CLOCK_H_ */

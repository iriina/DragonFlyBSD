/*
 * Copyright (c) 2003,2004,2008 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * Copyright (c) 1997, by Steve Passe,  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. The name of the developer may NOT be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/i386/isa/apic_ipl.s,v 1.27.2.2 2000/09/30 02:49:35 ps Exp $
 * $DragonFly: src/sys/platform/pc64/apic/apic_ipl.s,v 1.1 2008/08/29 17:07:12 dillon Exp $
 */

#include "use_npx.h"

#include <machine/asmacros.h>
#include <machine/segments.h>
#include <machine/lock.h>
#include <machine/psl.h>
#include <machine/trap.h>

#include "apicreg.h"
#include "apic_ipl.h"
#include "assym.s"

#ifdef APIC_IO

	.data
	ALIGN_DATA

	/*
	 * Interrupt mask for APIC interrupts, defaults to all hardware
	 * interrupts turned off.
	 */

	.p2align 2				/* MUST be 32bit aligned */

	.globl apic_imen
apic_imen:
	.long	APIC_HWI_MASK

	.text
	SUPERALIGN_TEXT

	/*
	 * Functions to enable and disable a hardware interrupt.  The
	 * IRQ number is passed as an argument.
	 */
ENTRY(APIC_INTRDIS)
	APIC_IMASK_LOCK			/* enter critical reg */
	movl	4(%esp),%eax
1:
	btsl	%eax, apic_imen
	shll	$4, %eax
	movl	CNAME(int_to_apicintpin) + 8(%eax), %edx
	movl	CNAME(int_to_apicintpin) + 12(%eax), %ecx
	testl	%edx, %edx
	jz	2f
	movl	%ecx, (%edx)		/* target register index */
	orl	$IOART_INTMASK,16(%edx)	/* set intmask in target apic reg */
2:
	APIC_IMASK_UNLOCK		/* exit critical reg */
	ret

ENTRY(APIC_INTREN)
	APIC_IMASK_LOCK			/* enter critical reg */
	movl	4(%esp), %eax		/* mask into %eax */
1:
	btrl	%eax, apic_imen		/* update apic_imen */
	shll	$4, %eax
	movl	CNAME(int_to_apicintpin) + 8(%eax), %edx
	movl	CNAME(int_to_apicintpin) + 12(%eax), %ecx
	testl	%edx, %edx
	jz	2f
	movl	%ecx, (%edx)		/* write the target register index */
	andl	$~IOART_INTMASK, 16(%edx) /* clear mask bit */
2:	
	APIC_IMASK_UNLOCK		/* exit critical reg */
	ret

/******************************************************************************
 * 
 */

/*
 * u_int io_apic_write(int apic, int select);
 */
ENTRY(io_apic_read)
	movl	4(%esp), %ecx		/* APIC # */
	movl	ioapic, %eax
	movl	(%eax,%ecx,4), %edx	/* APIC base register address */
	movl	8(%esp), %eax		/* target register index */
	movl	%eax, (%edx)		/* write the target register index */
	movl	16(%edx), %eax		/* read the APIC register data */
	ret				/* %eax = register value */

/*
 * void io_apic_write(int apic, int select, int value);
 */
ENTRY(io_apic_write)
	movl	4(%esp), %ecx		/* APIC # */
	movl	ioapic, %eax
	movl	(%eax,%ecx,4), %edx	/* APIC base register address */
	movl	8(%esp), %eax		/* target register index */
	movl	%eax, (%edx)		/* write the target register index */
	movl	12(%esp), %eax		/* target register value */
	movl	%eax, 16(%edx)		/* write the APIC register data */
	ret				/* %eax = void */

/*
 * Send an EOI to the local APIC.
 */
ENTRY(apic_eoi)
	movl	$0, lapic+0xb0
	ret

#endif
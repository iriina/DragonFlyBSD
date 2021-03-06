/*
 * Written by J.T. Conklin <jtc@NetBSD.org>.
 * Public domain.
 *
 * $NetBSD: s_scalbn.S,v 1.8 2006/03/21 11:35:21 drochner Exp $
 * $DragonFly: src/lib/libm/arch/i386/s_scalbn.S,v 1.2 2007/06/16 23:53:00 pavalos Exp $
 */

#include <machine/asm.h>

ENTRY(scalbn)
#ifdef __x86_64__
	movl %edi,-12(%rsp)
	fildl -12(%rsp)
	movsd %xmm0,-8(%rsp)
	fldl -8(%rsp)
	fscale
	fstpl -8(%rsp)
	movsd -8(%rsp),%xmm0
	fstp %st(0)
#else
	fildl 12(%esp)
	fldl 4(%esp)
	fscale
	fstp %st(1)		/* clean up stack */
#endif
	ret

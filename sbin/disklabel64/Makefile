#	@(#)Makefile	8.2 (Berkeley) 3/17/94
# $FreeBSD: src/sbin/disklabel/Makefile,v 1.3.14.3 2001/04/25 10:58:15 ru Exp $
# $DragonFly: src/sbin/disklabel64/Makefile,v 1.1 2007/06/19 02:53:55 dillon Exp $

PROG=	disklabel64
SRCS=	disklabel64.c crc32.c
MAN=	disklabel64.5 disklabel64.8

LINKS=	${BINDIR}/disklabel64 ${BINDIR}/disklabel
MLINKS+= disklabel64.8 disklabel.8
MLINKS+= disklabel64.5 disklabel.5

.include <bsd.prog.mk>

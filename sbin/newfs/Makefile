#	@(#)Makefile	8.2 (Berkeley) 3/27/94
# $FreeBSD: src/sbin/newfs/Makefile,v 1.6.6.2 2001/04/25 10:58:41 ru Exp $

PROG=	newfs
SRCS=	getmntopts.c newfs.c mkfs.c fscopy.c
MAN=	newfs.8

MOUNT=	${.CURDIR}/../mount
CFLAGS+=-DMFS -DFSIRAND -I${MOUNT}
.PATH:	${MOUNT}

LINKS=	${BINDIR}/newfs ${BINDIR}/mount_mfs
MLINKS=	newfs.8 mount_mfs.8 newfs.8 mfs.8

.include <bsd.prog.mk>

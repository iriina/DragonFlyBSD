#	$NetBSD: Makefile,v 1.4 2008/12/22 18:13:30 agc Exp $

LVM2_DIR=	${.CURDIR}/../../../contrib/lvm2

LVM2_DISTDIR=       ${LVM2_DIR}/dist
LIBDM_DISTDIR=        ${LVM2_DIR}/dist/libdm
LIBDM_SRCDIR= ${LVM2_DIR}/lib/libdevmapper
.PATH:  ${LVM2_DISTDIR}/man

PROG=		dmsetup

MAN=		dmsetup.8

CSTD?=		gnu89

CFLAGS+=	-D_XOPEN_SOURCE=600
CFLAGS+=      -I${LIBDM_DISTDIR}/ -I${LIBDM_DISTDIR}/misc \
		-I. -I${LIBDM_DISTDIR}/ioctl -I${LVM2_DISTDIR}/include

CFLAGS+=	-D__LIB_DEVMAPPER__ -DDM_DEVICE_UID=0 \
		-DDM_DEVICE_GID=0 -DDM_DEVICE_MODE=0600

LDADD+=		-L${.CURDIR}/../../lib/libdevmapper -ldevmapper
DPADD+=		${.CURDIR}/../../lib/libdevmapper/libdevmapper.a

LDADD+=		-lprop

.PATH:	${LVM2_DISTDIR}/tools

.include <bsd.prog.mk>

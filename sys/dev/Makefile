# $DragonFly: src/sys/dev/Makefile,v 1.12 2007/01/30 14:50:10 corecode Exp $
#

.include "../platform/${MACHINE_PLATFORM}/Makefile.inc"

SUBDIR=	\
	acpica5 \
	agp \
	bridge \
	crypto \
	disk \
	drm \
	est \
	misc \
	netif \
	pccard \
	powermng \
	raid \
	serial \
	sound \
	usbmisc \
	video \
	virtual

.for dir in ${SUBDIR}
.if empty(DEV_SUPPORT:M${dir}) && empty(DEV_SUPPORT:M${dir}/*)
SUBDIR:= ${SUBDIR:N${dir}}
.endif
.endfor

.include <bsd.subdir.mk>

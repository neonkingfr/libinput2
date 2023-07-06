#	$OpenBSD$

INSTALL_DATA=	/usr/bin/install -c -m 644

PACKAGE_VERSION=	0.21.0

NOPROFILE=

LIB=		input

INCSDIR=	/usr/local/include/
LIBDIR=		/usr/local/lib

CPPFLAGS+=	-I${.CURDIR} \
		-I${.CURDIR}/include \


INCS= 		libinput.h
SRCS=		libinput.c libinput-util.c wscons.c
PKGCONFIG=	libinput.pc

LINUX_INCS=	input.h \
		freebsd/input-event-codes.h \
		freebsd/input.h

includes: _SUBDIRUSE
	cd ${.CURDIR}; for i in ${INCS}; do \
	    j="cmp -s $$i ${DESTDIR}${INCSDIR}/$$i || \
		${INSTALL_DATA} $$i ${DESTDIR}${INCSDIR}/"; \
		echo "\tinstalling $$i"; \
		eval "$$j"; \
	done
	@test -d ${DESTDIR}${INCSDIR}/linux || \
	    mkdir ${DESTDIR}${INCSDIR}/linux
	cd ${.CURDIR}/include; pax -rwv linux ${DESTDIR}${INCSDIR}/

.include <bsd.obj.mk>
.include <bsd.lib.mk>

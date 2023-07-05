#	$OpenBSD$

.include "${.CURDIR}/Makefile.inc"

LIB=		input

INCSDIR=	/usr/local/include/
LIBDIR=		/usr/local/lib

CPPFLAGS+=	-I${.CURDIR} \
		-I${.CURDIR}/include \


INCS= 		libinput.h
SRCS=		libinput.c libinput-util.c wscons.c
PKGCONFIG=	libinput.pc

LINUX_INCS=	input.h


.include <bsd.obj.mk>
.include <bsd.lib.mk>

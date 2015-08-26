#	$OpenBSD$
.include <bsd.xconf.mk>
.include "${.CURDIR}/Makefile.inc"

LIB=		input

INCSDIR=	${X11BASE}/include/

CPPFLAGS+=	-I${.CURDIR} \
		-I${X11BASE}/include

INCS= 		libinput.h
SRCS=		libinput.c libinput-util.c wscons.c
PKGCONFIG=	libinput.pc

LINUX_INCS=	input.h

obj: _xenocara_obj

.include <bsd.lib.mk>
.include <bsd.xorg.mk>

#	$OpenBSD$

INSTALL_DATA=	/usr/bin/install -c -m 644

PACKAGE_VERSION=	0.21.0

NOPROFILE=

LIB=		input

PREFIX=		/usr/local
INCSDIR=	${PREFIX}/include/
LIBDIR=		${PREFIX}/lib

CPPFLAGS+=	-I${.CURDIR} \
		-I${.CURDIR}/include \


INCS= 		libinput.h
SRCS=		libinput.c libinput-util.c wscons.c
PKGCONFIG=	libinput.pc

LINUX_INCS=	input.h \
		input-event-codes.h \
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
	cd ${.CURDIR}/include/linux; for i in ${LINUX_INCS}; do \
	    j="cmp -s $$i ${DESTDIR}${INCSDIR}/linux/$$i || \
		${INSTALL_DATA} $$i ${DESTDIR}${INCSDIR}/linux/"; \
		echo "\tinstalling $$i"; \
		eval "$$j"; \
	done


# pkgconfig
PKGCONFIG = libinput.pc

.SUFFIXES: .in .pc

all: ${PKGCONFIG}

CLEANFILES += ${PKGCONFIG}

${PKGCONFIG}: ${PKGCONFIG}.in
	@sed -e 's#@prefix@#${PREFIX}#g' \
	    -e 's#@datarootdir@#$${prefix}/share#g' \
	    -e 's#@datadir@#$${datarootdir}#g' \
	    -e 's#@exec_prefix@#$${prefix}#g' \
	    -e 's#@libdir@#$${exec_prefix}/lib#g' \
	    -e 's#@includedir@#$${prefix}/include#g' \
	    -e 's#@PACKAGE_VERSION@#'${PACKAGE_VERSION}'#g' \
	    ${EXTRA_PKGCONFIG_SUBST} \
	< $? > $@

install-pc: ${PKGCONFIG}
	${INSTALL_DATA} ${PKGCONFIG} ${DESTDIR}${LIBDIR}/pkgconfig

realinstall: install-pc

.include <bsd.obj.mk>
.include <bsd.lib.mk>

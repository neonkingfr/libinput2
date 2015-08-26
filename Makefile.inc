#	$OpenBSD$

PACKAGE_VERSION=	0.21.0

NOPROFILE=

includes: _SUBDIRUSE
	cd ${.CURDIR}; for i in ${INCS}; do \
	    j="cmp -s $$i ${DESTDIR}${INCSDIR}/$$i || \
		${INSTALL_DATA} $$i ${DESTDIR}${INCSDIR}/"; \
		echo "\tinstalling $$i"; \
		eval "$$j"; \
	done
	@test -d ${DESTDIR}${INCSDIR}/linux || \
	    mkdir ${DESTDIR}${INCSDIR}/linux
	cd ${.CURDIR}; for i in ${LINUX_INCS}; do \
	    j="cmp -s $$i ${DESTDIR}${INCSDIR}/linux/$$i || \
		${INSTALL_DATA} $$i ${DESTDIR}${INCSDIR}/linux"; \
		echo "\tinstalling $$i"; \
		eval "$$j"; \
	done

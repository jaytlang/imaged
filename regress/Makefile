PYTHON!=	which python3 || true
RCDAEMON=	bundled
FLAGS=		-v
SUDO=		doas

REGRESS_SETUP_ONCE= daemon_start
REGRESS_CLEANUP= daemon_stop

TESTFILES!= ls t_*.py | sed -E 's/(.*)\.py/\1/'

.for TESTFILE in ${TESTFILES}
${TESTFILE}:
	[ -n "${PYTHON}" ] && {			\
		${PYTHON} ${TESTFILE}.py;	\
		pgrep ${RCDAEMON} >/dev/null;	\
	} || echo "SKIPPED"

REGRESS_TARGETS+= ${TESTFILE}
.endfor

.PHONY: daemon_start daemon_stop depend

daemon_start:
	if [ ! -r "/etc/rc.d/${RCDAEMON}" ]; then		\
		echo "SKIPPED";					\
		exit 0;						\
	fi
	doas rcctl enable ${RCDAEMON}
	doas rcctl set ${RCDAEMON} flags -v
	doas rcctl start ${RCDAEMON}

daemon_stop:
	doas rcctl stop ${RCDAEMON}
	doas rcctl disable ${RCDAEMON}
	doas rm -rf __pycache__

.include <bsd.regress.mk>

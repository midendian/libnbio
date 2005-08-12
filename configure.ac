AC_INIT(src/libnbio.c)
AM_INIT_AUTOMAKE(libnbio, 0.20)
AM_CONFIG_HEADER(include/config.h)

AM_MAINTAINER_MODE

AC_PROG_CC
AC_ISC_POSIX
AC_HEADER_STDC
AM_PROG_LIBTOOL
AC_CHECK_HEADERS(errno.h fcntl.h netdb.h stdio.h stdlib.h string.h sys/poll.h sys/socket.h sys/types.h unistd.h)

case "$ac_cv_host" in
	*-*-darwin*)
		[macosx="yes"]
		;;
esac

NB_LDADD=

CFLAGS="$CFLAGS -Wall -g"

AC_ARG_ENABLE(kqueue,   [  --enable-kqeue   use kqueue/kevent instead of poll], enable_kqueue=yes, enable_kqueue=no)

if test "$macosx" = "yes"; then
	dnl pre-Tiger doesn't have poll(), and Tiger's poll is broken as hell.
	AC_DEFINE(NBIO_USE_SELECT, 1, [Define if select should be used instead of poll on UNIX])
elif test "x$enable_kqueue" = "xyes" ; then
	AC_DEFINE(NBIO_USE_KQUEUE, 1, [Define if kqueue should be used instead of poll on UNIX])
fi

dnl for systems that can't decide if they need libdl or not
AC_CHECK_LIB(dl, dlopen, AP_LDADD="-ldl $NB_LDADD")
AC_SUBST(NB_LDADD)

AC_SUBST(CFLAGS)

dnl LIBTOOL="$LIBTOOL --silent"

AC_OUTPUT([
	Makefile
	include/Makefile
	src/Makefile
	nbmsnp/Makefile
])

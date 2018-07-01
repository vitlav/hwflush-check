
AC_PREREQ([2.69])
AC_INIT([hwflush-check], [0.1], [lav@etersoft.ru])

AM_INIT_AUTOMAKE

#AC_CONFIG_HEADERS([config.h])

AC_PROG_CC
AM_PROG_CC_C_O

# for O_DIRECT
#AC_USE_SYSTEM_EXTENSIONS
AC_GNU_SOURCE

AC_C_CONST

AC_CHECK_HEADERS([fcntl.h getopt.h netdb.h stdio.h stdlib.h string.h])
AC_CHECK_HEADERS([sys/ioctl.h sys/socket.h sys/stat.h sys/types.h stdint.h signal.h])
AC_CHECK_HEADERS([netinet/tcp.h unised.h malloc.h stdarg.h sys/time.h])
AC_CHECK_HEADERS(pthread.h,, [AC_MSG_ERROR([pthread.h required])])

# required libraries
AC_CHECK_LIB(pthread, pthread_create, [LIBS="$LIBS -lpthread"])

AC_CONFIG_FILES([
	Makefile
])
AC_OUTPUT


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

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <signal.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <unistd.h>
#include <malloc.h>
#include <stdarg.h>
#include <sys/time.h>


AC_CHECK_HEADERS([fcntl.h getopt.h netdb.h stdio.h stdlib.h string.h])
AC_CHECK_HEADERS([sys/ioctl.h sys/socket.h sys/stat.h sys/types.h stdint.h signal.h])
AC_CHECK_HEADERS([netinet/tcp.h unised.h malloc.h stdarg.h sys/time.h])
AC_CHECK_HEADERS(pthread.h,, [AC_MSG_ERROR([pthread.h required])])

# required functions
#AC_CHECK_FUNC([snprintf],, [AC_MSG_ERROR($function_not_found)])

# required libraries
# pthreads
AC_CHECK_LIB(pthread, pthread_create, [LIBS="$LIBS -lpthread"])

AC_CONFIG_FILES([
	Makefile
])
AC_OUTPUT

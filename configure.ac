AC_PREREQ([2.71])
AC_INIT([bwchat], [0.0.0], [defanor@thunix.net])
AM_INIT_AUTOMAKE([-Werror -Wall])
AC_CONFIG_SRCDIR([bwchat_server.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
AC_ARG_WITH([fcgi],
  [AS_HELP_STRING([--without-fcgi], [disable FastCGI support])])

AS_IF([test "x$with_fcgi" != xno],
  [AC_SEARCH_LIBS([FCGI_Accept], [fcgi],
    [AC_SUBST([LIBFCGI], ["-lfcgi"])
     AC_DEFINE([HAVE_FCGI], [1], [libfcgi is available])])])

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h sys/socket.h syslog.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_CHECK_FUNCS([select socket strerror])

AC_OUTPUT

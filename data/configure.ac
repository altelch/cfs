#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([cfs], [1.0], [hotline@rhrk.uni-kl.de]) 
AM_INIT_AUTOMAKE([-Wall -Werror -Wno-portability foreign])
AM_MAINTAINER_MODE

AC_CONFIG_SRCDIR([src/memdb.c])

AC_CONFIG_HEADER([config.h])

AC_PREFIX_DEFAULT([/usr])

# Checks for programs.
AC_PROG_CC
AC_PROG_RANLIB

m4_pattern_allow([AM_PROG_AR])
AM_PROG_AR
    
# Checks for libraries.

PKG_CHECK_MODULES([CHECK], [check >= 0.9.4],
HAVE_CHECK="yes", AC_MSG_ERROR([check not found])) 

PKG_CHECK_MODULES(GLIB, 
glib-2.0 >= 2.42.1 gthread-2.0 >= 2.42.1, 
HAVE_GLIB="yes", AC_MSG_ERROR([glib not found]))
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

PKG_CHECK_MODULES(FUSE, 
fuse >= 2.7.4, 
HAVE_FUSE="yes", AC_MSG_ERROR([fuse not found]))
AC_SUBST(FUSE_CFLAGS)
AC_SUBST(FUSE_LIBS)

PKG_CHECK_MODULES(COROSYNC, 
libquorum >= 2.3.4 libcpg >= 2.3.4 libcmap >= 2.3.4,  
HAVE_COROSYNC="yes", AC_MSG_ERROR([corosync not found]))
AC_SUBST(COROSYNC_CFLAGS)
AC_SUBST(COROSYNC_LIBS)

PKG_CHECK_MODULES(QB, 
libqb, 
HAVE_QB="yes", AC_MSG_ERROR([libqb not found]))
AC_SUBST(QB_CFLAGS)
AC_SUBST(QB_LIBS)

PKG_CHECK_MODULES(SQLITE3, 
sqlite3, 
HAVE_SQLITE3="yes", AC_MSG_ERROR([sqlite3 not found]))
AC_SUBST(SQLITE3_CFLAGS)
AC_SUBST(SQLITE3_LIBS)

AC_PATH_PROG([PERL], [perl], [no])
if test "x${PERL}" = xno; then
  AC_MSG_ERROR([perl not found])
fi
AC_SUBST(PERL)

PERL_INC="$(perl -MExtUtils::Embed -e perl_inc)"
PERL_VENDORLIB="$(perl -MConfig -e 'print $Config{vendorlibexp}')"
PERL_INSTALLARCHLIB="$(perl -MConfig -e 'print $Config{installarchlib}')"
AC_SUBST(PERL_INC)
AC_SUBST(PERL_VENDORLIB)
AC_SUBST(PERL_INSTALLARCHLIB)

# Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_CHECK_HEADERS([fcntl.h stdlib.h string.h sys/file.h sys/mount.h sys/statvfs.h syslog.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_MODE_T
AC_TYPE_OFF_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_STRUCT_ST_BLOCKS
AC_HEADER_TIME
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T

# Checks for library functions.
AC_FUNC_CLOSEDIR_VOID
AC_FUNC_LSTAT
AC_FUNC_LSTAT_FOLLOWS_SLASHED_SYMLINK
AC_FUNC_MEMCMP
AC_FUNC_SELECT_ARGTYPES
AC_CHECK_FUNCS([memset mkdir rmdir select strdup strerror])

AC_CONFIG_FILES([
Makefile
src/Makefile
])
AC_OUTPUT


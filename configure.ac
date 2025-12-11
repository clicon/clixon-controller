#
# ***** BEGIN LICENSE BLOCK *****
# 
# Copyright (C) 2023,2024 Olof Hagsand
#
# This file is part of CLIXON
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# ***** END LICENSE BLOCK *****
#

# To rebuild the `configure' script from this, execute the command
# 	autoconf
# in the directory containing this script.
#

AC_INIT([controller],[1.1])

# Default CFLAGS and INSTALLFLAGS unless set by environment
: ${CFLAGS="-O2 -Wall -Werror"}
: ${INSTALLFLAGS="-s"}

# where autotool scripts are: install-sh, config.sub, config.guess
AC_CONFIG_AUX_DIR(aux)

CONTROLLER_VERSION="$(./aux/version.sh)"
CONTROLLER_VERSION2=$(echo ${CONTROLLER_VERSION} | awk -F- '{print $1}')
CONTROLLER_VERSION_MAJOR=$(echo ${CONTROLLER_VERSION2} | awk -F. '{print $1}')
CONTROLLER_VERSION_MINOR=$(echo ${CONTROLLER_VERSION2} | awk -F. '{print $2}')
CONTROLLER_VERSION_PATCH=$(echo ${CONTROLLER_VERSION2} | awk -F. '{print $3}')

AC_SUBST(CONTROLLER_VERSION)

AC_MSG_RESULT(CONTROLLER version is ${CONTROLLER_VERSION})

# Check CLIgen
if test "$prefix" = "NONE"; then
	CLIGEN_PREFIX="$ac_default_prefix"
	CLIXON_PREFIX="$ac_default_prefix"
else
	CLIGEN_PREFIX="$prefix"
	CLIXON_PREFIX="$prefix"
fi

AC_CHECK_LIB(m, main)

# AC_SUBST(var) makes @var@ appear in makefiles.
AC_SUBST(CLIXON_PREFIX)
AC_SUBST(CLIGEN_PREFIX)

AC_CANONICAL_TARGET

AC_SUBST(CC)
AC_SUBST(CFLAGS)
AC_SUBST(LDFLAGS)
AC_SUBST(INCLUDES)
AC_SUBST(CPPFLAGS)
AC_PROG_INSTALL
AC_SUBST(INSTALL)
AC_SUBST(INSTALL_DATA)
AC_SUBST(INSTALL_PROGRAM)
AC_SUBST(INSTALLFLAGS)
AC_SUBST(LIBS)
AC_SUBST(SH_SUFFIX)
# Used in expansion in, e.g., example/main/example.xml.in
AC_SUBST(BINDIR)
AC_SUBST(DATADIR)
AC_SUBST(INCLUDEDIR)
AC_SUBST(LIBDIR)
AC_SUBST(LIBEXECDIR)
AC_SUBST(LOCALSTATEDIR)
AC_SUBST(RUNSTATEDIR)
AC_SUBST(SBINDIR)
AC_SUBST(SYSCONFDIR)
AC_SUBST(HAVE_SYSTEMD,false)

#
AC_PROG_CC()
AC_PROG_CPP

AC_MSG_RESULT(compiler is $CC)	
# Debug flag
AC_ARG_ENABLE(debug, AS_HELP_STRING([--enable-debug],[Build with debug symbols, default: no]),[
	  if test "$enableval" = no; then
	      ac_enable_debug=no
	  else	      
	      ac_enable_debug=yes
          fi
        ],
	[ ac_enable_debug=no])

AC_MSG_RESULT(debug is $ac_enable_debug)	
if test "$ac_enable_debug" = "yes"; then
   CFLAGS="-g -Wall -Werror"
   INSTALLFLAGS=""	
fi

AC_MSG_RESULT(CPPFLAGS is $CPPFLAGS)	
AC_MSG_RESULT(CFLAGS is $CFLAGS)
AC_MSG_RESULT(INSTALLFLAGS is $INSTALLFLAGS)	

# SSH binary path (see controller/src/Makefile and clixon_client.c
AC_PATH_PROG(SSH_BIN, ssh)

if test "$prefix" = "NONE"; then
     prefix=${ac_default_prefix}
fi
if test "$exec_prefix" = "NONE"; then
     exec_prefix=${prefix}
fi

# Expand for easy replacement in example/main/example.xml.in
# Special case is $libdir, which is composed from $exec_prefix
BINDIR=`eval echo $bindir`
SBINDIR=`eval echo $sbindir`
SYSCONFDIR=`eval echo $sysconfdir`
DATADIR=`eval echo \`eval echo $datadir\``
INCLUDEDIR=`eval echo $includedir`
LIBDIR=`eval echo \`eval echo $libdir\``
LIBEXECDIR=`eval echo $libexecdir`
LOCALSTATEDIR=`eval echo $localstatedir`
RUNSTATEDIR=`eval echo \`eval echo $runstatedir\``

# This is for cligen
AC_ARG_WITH(cligen,  [  --with-cligen=dir       Use CLIGEN here ] )
if test "${with_cligen}"; then
  echo "Using CLIgen here: ${with_cligen}"
  CPPFLAGS="-I${with_cligen}/include ${CPPFLAGS}"
  LDFLAGS="-L${with_cligen}/lib ${LDFLAGS}"
  test -d "$with_cligen" && CLIGEN_PREFIX="$with_cligen"
fi

# This is for clixon
AC_ARG_WITH(clixon,  [  --with-clixon=dir       Use Clixon here ] )
if test "${with_clixon}"; then
  echo "Using Clixon here: ${with_clixon}"
  CPPFLAGS="-I${with_clixon}/include ${CPPFLAGS}"
  LDFLAGS="-L${with_clixon}/lib ${LDFLAGS}"
  test -d "$with_clixon" && CLIXON_PREFIX="$with_clixon"
fi

AC_CHECK_HEADERS(cligen/cligen.h,, AC_MSG_ERROR(cligen missing. Try: git clone https://github.com/clicon/cligen.git))

AC_CHECK_LIB(cligen, cligen_init,, AC_MSG_ERROR([CLIgen missing. Try: git clone https://github.com/clicon/cligen.git]))

AC_CHECK_HEADERS(clixon/clixon.h,, AC_MSG_ERROR(Clixon missing. Try: git clone https://github.com/clicon/clixon.git), [[#include <signal.h>
#include <cligen/cligen.h>]])
AC_CHECK_LIB(clixon, clixon_log_init,, AC_MSG_ERROR([Clixon missing. Try: git clone https://github.com/clicon/clixon.git]),)

# Dummy to disable native language support (nls) to remove warnings in buildroot
AC_ARG_ENABLE(nls)

# who we run as in our examples and tests
CLICON_USER="clicon"
AC_ARG_WITH([clicon-user], [AS_HELP_STRING([--with-clicon-user=user], [Run as this user in configuration files])], [
  CLICON_USER="$withval"
])
AC_SUBST(CLICON_USER)
if test -n "${CLICON_USER}"; then
    echo "Using CLICON_USER here: ${CLICON_USER}"
fi

CLICON_GROUP="clicon"
AC_ARG_WITH([clicon-group], [AS_HELP_STRING([--with-clicon-group=group], [Run as this group in configuration files])], [
  CLICON_GROUP="$withval"
])
AC_SUBST(CLICON_GROUP)
if test -n "${CLICON_GROUP}"; then
    echo "Using CLICON_GROUP here: ${CLICON_GROUP}"
fi

# handle systemd service
PKG_PROG_PKG_CONFIG()
AC_ARG_WITH([systemdsystemunitdir],
     [AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [Directory for systemd service files])],,
     [with_systemdsystemunitdir=auto])
AS_IF([test "x$with_systemdsystemunitdir" = "xyes" -o "x$with_systemdsystemunitdir" = "xauto"], [
     def_systemdsystemunitdir=$($PKG_CONFIG --variable=systemdsystemunitdir systemd)

     AS_IF([test "x$def_systemdsystemunitdir" = "x"],
   [AS_IF([test "x$with_systemdsystemunitdir" = "xyes"],
    [AC_MSG_ERROR([systemd support requested but pkg-config unable to query systemd package])])
    with_systemdsystemunitdir=no],
   [with_systemdsystemunitdir="$def_systemdsystemunitdir"])])
AS_IF([test "x$with_systemdsystemunitdir" != "xno"],
      [AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])])
if test "x$with_systemdsystemunitdir" != "xno" ; then
      AC_DEFINE(HAVE_SYSTEMD, true)
      HAVE_SYSTEMD=true
fi

AC_CONFIG_FILES([Makefile
                 clixon-controller.service
                 src/Makefile
                 src/controller.xml
                 yang/Makefile
                 util/Makefile
                 docker/Makefile
                 test/config.sh
                 doc/Makefile
		 extensions/Makefile
                 extensions/plugins/Makefile
 		 extensions/plugins/junos-native/Makefile
 		 extensions/plugins/cli-command/Makefile		 
		 extensions/yang/Makefile
		 extensions/yang/junos-bgp/Makefile
		 extensions/yang/junos-macsec/Makefile
		 extensions/yang/junos-users/Makefile
])
AC_OUTPUT

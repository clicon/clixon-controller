#
# ***** BEGIN LICENSE BLOCK *****
# 
# Copyright (C) 2023 Olof Hagsand
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

AC_INIT(controller, 0.1)

# Default CFLAGS and INSTALLFLAGS unless set by environment
: ${CFLAGS="-O2 -Wall"}
: ${INSTALLFLAGS="-s"}

# where autotool scripts are: install-sh, config.sub, config.guess
AC_CONFIG_AUX_DIR(aux)

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
   CFLAGS="-g -Wall"
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

AC_CHECK_HEADERS(cligen/cligen.h,, AC_MSG_ERROR(cligen missing. Try: git clone https://github.com/olofhagsand/cligen.git))

AC_CHECK_LIB(cligen, cligen_init,, AC_MSG_ERROR([CLIgen missing. Try: git clone https://github.com/olofhagsand/cligen.git]))

AC_CHECK_HEADERS(clixon/clixon.h,, AC_MSG_ERROR(Clixon missing. Try: git clone https://github.com/clicon/clixon.git), [[#include <signal.h>
#include <cligen/cligen.h>]])
AC_CHECK_LIB(clixon, clicon_log_init,, AC_MSG_ERROR([Clixon missing. Try: git clone https://github.com/clicon/clixon.git]),)

# This is where local yangs are installed
AC_ARG_WITH(yang-installdir,
            [  --with-yang-installdir=DIR  Install yang files here (default: ${prefix}/share/clixon) ],
	    [YANG_INSTALLDIR="$withval"],
    	    [YANG_INSTALLDIR="${prefix}/share/clixon"]		
	    )
AC_SUBST(YANG_INSTALLDIR)
AC_MSG_RESULT(Clixon yang files are installed in ${YANG_INSTALLDIR})	

# Dummy to disable native language support (nls) to remove warnings in buildroot
AC_ARG_ENABLE(nls)

AC_OUTPUT([Makefile
	  controller/Makefile
  	  controller/src/Makefile
	  controller/yang/Makefile
    	  controller/docker/Makefile
          util/Makefile
])

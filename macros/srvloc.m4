dnl Check for optional server location protocol support (used by MacOS X)

dnl $Id: srvloc.m4,v 1.2.2.3 2002-02-08 16:43:26 jmarcus Exp $

AC_DEFUN([NETATALK_SRVLOC], [

	SLP_LIBS=""
	SLP_CFLAGS=""

	AC_ARG_ENABLE(srvloc,
		[  --enable-srvloc[=DIR]   turn on Server Location Protocol support],
		[srvloc=$enableval],
		[srvloc=no]
	)

	if test "x$srvloc" != "xno"; then

		savedcppflags="$CPPFLAGS"
		savedldflags="$LDFLAGS"
		if test "x$srvloc" != "xyes"; then
			CPPFLAGS="$CPPFLAGS -I$srvloc/include"
			LDFLAGS="$LDFLAGS -L$srvloc/lib"
		fi
		AC_MSG_CHECKING([for slp.h])
		AC_TRY_CPP([#include <slp.h>],
			[AC_MSG_RESULT([yes])],
			[
				AC_MSG_RESULT([no])
				AC_MSG_ERROR([SLP installation not found])
			]
		)
		AC_CHECK_LIB(slp, SLPOpen, [
			SLP_LIBS="-L$srvloc/lib -lslp"
			SLP_CFLAGS="-I$srvloc/include"
		], AC_MSG_ERROR([SLP installation not found]))

		AC_DEFINE(USE_SRVLOC, 1)

		CPPFLAGS="$savedcppflags"
		LDFLAGS="$savedldflags"
	fi

	AC_SUBST(SLP_LIBS)
	AC_SUBST(SLP_CFLAGS)
])

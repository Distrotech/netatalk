dnl $Id: db3-check.m4,v 1.7.4.3 2003-06-06 19:51:15 srittau Exp $
dnl Autoconf macro to check for the Berkeley DB library

AC_DEFUN([AC_PATH_BDB], [
	trybdbdir=""
	AC_ARG_WITH(bdb,
		[  --with-bdb=PATH         specify path to Berkeley DB installation],
		if test "x$withval" != "xno"; then
			trybdbdir="$withval"
		fi
	)

	bdbfound=no
	for bdbdir in "" "$trybdbdir" "$trybdbdir/include" "$trybdbdir/include/db3" "/usr/local/BerkeleyDB.3.3/include" "/usr/local/include/db3" "/usr/local/include" "/usr/include/db3" "/usr/include" ; do
		if test -f "$bdbdir/db.h" ; then
			bdblibdir="`echo $bdbdir | sed 's/include\/db3$/lib/'`"
			bdblibdir="`echo $bdblibdir | sed 's/include$/lib/'`"
			bdbbindir="`echo $bdbdir | sed 's/include\/db3$/bin/'`"
			bdbbindir="`echo $bdbbindir | sed 's/include$/bin/'`"

			savedcflags="$CFLAGS"
			savedldflags="$LDFLAGS"
			CFLAGS="$CFLAGS -I$bdbdir"
			LDFLAGS="-L$bdblibdir $LDFLAGS"
			AC_CHECK_LIB(db, main, [
				bdbfound=yes
				if test "$bdbdir" != "/usr/include"; then
				    BDB_CFLAGS="-I$bdbdir"
				fi
				if test "$bdblibdir" != "/usr/lib"; then
				    BDB_LIBS="-L$bdblibdir"
				fi
				BDB_LIBS="$BDB_LIBS -ldb"
				BDB_BIN=$bdbbindir
				BDB_PATH="`echo $bdbdir | sed 's,include/db3$,,'`"
				BDB_PATH="`echo $BDB_PATH | sed 's,include$,,'`"
			])
			CFLAGS="$savedcflags"
			LDFLAGS="$savedldflags"
			break;
		fi
	done

	if test "x$bdbfound" = "xyes"; then
		ifelse([$1], , :, [$1])
	else
		ifelse([$2], , :, [$2])     
	fi

	AC_SUBST(BDB_CFLAGS)
	AC_SUBST(BDB_LIBS)
	AC_SUBST(BDB_BIN)
	AC_SUBST(BDB_PATH)
])

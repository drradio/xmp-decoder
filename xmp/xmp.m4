dnl libxmp

AC_ARG_WITH(xmp, AS_HELP_STRING([--without-xmp],
                                    [Compile without libxmp]))

if test "x$with_xmp" != "xno"
then
	PKG_CHECK_MODULES(xmp, libxmp >= 4.4,
			   [AC_SUBST(xmp_LIBS)
			   AC_SUBST(xmp_CFLAGS)
			   want_xmp="yes"
			   DECODER_PLUGINS="$DECODER_PLUGINS xmp"],
			   [true])
fi

AM_CONDITIONAL([BUILD_xmp], [test "$want_xmp"])
AC_CONFIG_FILES([decoder_plugins/xmp/Makefile])

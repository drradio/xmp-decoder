lib_LTLIBRARIES = libxmp_decoder.la
libdir = $(plugindir)/$(DECODER_PLUGIN_DIR)
libxmp_decoder_la_LDFLAGS = @PLUGIN_LDFLAGS@
libxmp_decoder_la_LIBADD = $(xmp_LIBS)
libxmp_decoder_la_CFLAGS = $(xmp_CFLAGS) -I$(top_srcdir)
libxmp_decoder_la_SOURCES = xmp.c

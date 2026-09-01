AM_CPPFLAGS = \
	-I$(top_srcdir)/include

AM_CFLAGS = \
	$(GLOBAL_CFLAGS) \
	$(libimobiledevice_CFLAGS) \
	$(libimobiledevice_glue_CFLAGS) \
	$(libplist_CFLAGS)

AM_LDFLAGS = \
	$(GLOBAL_LIBS) \
	$(libimobiledevice_LIBS) \
	$(libimobiledevice_glue_LIBS) \
	$(libplist_LIBS)

bin_PROGRAMS = ideviceactivation

ideviceactivation_SOURCES = ideviceactivation.c
ideviceactivation_CFLAGS = $(AM_CFLAGS)
ideviceactivation_LDFLAGS = $(AM_LDFLAGS)
ideviceactivation_LDADD = $(top_builddir)/src/libideviceactivation-1.0.la

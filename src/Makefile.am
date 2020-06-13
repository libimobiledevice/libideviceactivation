AM_CPPFLAGS = \
	-I$(top_srcdir)/include

AM_CFLAGS = \
	$(GLOBAL_CFLAGS) \
	$(libimobiledevice_CFLAGS) \
	$(libplist_CFLAGS) \
	$(libcurl_CFLAGS) \
	$(libxml2_CFLAGS)

AM_LDFLAGS = \
	$(GLOBAL_LIBS) \
	$(libimobiledevice_LIBS) \
	$(libplist_LIBS) \
	$(libcurl_LIBS) \
	$(libxml2_LIBS)

lib_LTLIBRARIES = libideviceactivation-1.0.la
libideviceactivation_1_0_la_LDFLAGS = $(AM_LDFLAGS) -version-info $(LIBIDEVICEACTIVATION_SO_VERSION) -no-undefined
libideviceactivation_1_0_la_SOURCES = \
		activation.c

if WIN32
libideviceactivation_1_0_la_LDFLAGS += -avoid-version
endif

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libideviceactivation-1.0.pc

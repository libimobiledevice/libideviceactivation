AUTOMAKE_OPTIONS = foreign
ACLOCAL_AMFLAGS = -I m4
SUBDIRS = src include tools man

DISTCHECK_CONFIGURE_FLAGS =

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libideviceactivation-1.0.pc

indent:
	indent -kr -ut -ts4 -l120 src/*.c src/*.h dev/*.c


AUTOMAKE_OPTIONS = foreign
ACLOCAL_AMFLAGS = -I m4
SUBDIRS = src include tools man

EXTRA_DIST = \
	README.md

DISTCHECK_CONFIGURE_FLAGS =

indent:
	indent -kr -ut -ts4 -l120 src/*.c src/*.h dev/*.c

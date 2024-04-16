AUTOMAKE_OPTIONS = foreign
ACLOCAL_AMFLAGS = -I m4
SUBDIRS = src include tools man

EXTRA_DIST = \
	README.md \
	git-version-gen

DISTCHECK_CONFIGURE_FLAGS =

dist-hook:
	@if ! git diff --quiet; then echo "Uncommitted changes present; not releasing"; exit 1; fi
	echo $(VERSION) > $(distdir)/.tarball-version

indent:
	indent -kr -ut -ts4 -l120 src/*.c src/*.h dev/*.c

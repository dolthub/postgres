# src/test/modules/test_doltgres/Makefile
MODULE_big = test_doltgres
MODULES = storage_resource
OBJS = $(WIN32RES) test_doltgres.o catalog_view.o
PGFILEDESC = "test_doltgres - branch catalog projection experiment"
REGRESS = branch_catalog
EXTENSION = server_test_ext
DATA = server_test_ext--1.0.sql
TAP_TESTS = 1
# 026_native_binding counts the internal statements binding runs; 036 uses
# extensions' functions, types and operators.
EXTRA_INSTALL = contrib/pg_stat_statements contrib/pgcrypto contrib/hstore

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_doltgres
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

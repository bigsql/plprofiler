MODULE_big = plprofiler
OBJS = plprofiler.o

EXTENSION = plprofiler
DATA =	plprofiler--1.0--2.0.sql	\
		plprofiler--2.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/plprofiler
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk

plprofiler.o: CFLAGS += -I$(top_builddir)/src/pl/plpgsql/src

endif


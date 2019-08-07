# Major Change History:
# 2012 - Removed from PostgreSQL plDebugger Extension
# 2015 - Resurrected as standalone plProfiler by OpenSCG
# 2016 - Rewritten as v2 to use shared hash tables, have lower overhead
#			- v3 Major performance improvements, flame graph UI

MODULE_big = plprofiler
OBJS = plprofiler.o

EXTENSION = plprofiler
DATA =	plprofiler--1.0--2.0.sql \
		plprofiler--2.0--3.0.sql \
		plprofiler--3.0--3.5.sql \
		plprofiler--3.5--4.0.sql \
		plprofiler--4.0--4.1.sql \
		plprofiler--4.1.sql

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

plprofiler.o: plprofiler.c plprofiler.h

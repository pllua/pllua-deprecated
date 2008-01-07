# Makefile for PL/Lua
# $Id: Makefile,v 1.5 2008/01/07 23:41:37 carvalho Exp $

MODULES = pllua plluau
DATA_built = pllua.sql
PG_CPPFLAGS = -I/usr/include/lua5.1
EXTRA_CLEAN = plluaapi.o plluaspi.o

PGXS := $(shell pg_config --pgxs)
include $(PGXS)

%.sql: %.source
	rm -f $@; \
	C=`pg_config --pkglibdir`; \
	sed -e "s:_OBJWD_:$$C:g" < $< > $@

pllua.so : pllua.o plluaapi.o plluaspi.o
	$(CC) -shared -o $@ $^ -llua5.1

plluau.so : plluau.o plluaapi.o plluaspi.o
	$(CC) -shared -o $@ $^ -llua5.1


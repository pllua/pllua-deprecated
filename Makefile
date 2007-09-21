# Makefile for PL/Lua
# $Id: Makefile,v 1.4 2007/09/21 03:20:52 carvalho Exp $

MODULES = pllua plluau
DATA_built = pllua.sql
PG_CPPFLAGS = -I/usr/include/lua5.1

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


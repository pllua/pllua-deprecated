# Makefile for PL/Lua
# $Id: Makefile,v 1.3 2007/09/18 22:03:17 carvalho Exp $

MODULES = pllua
DATA_built = pllua.sql
PG_CPPFLAGS = -I/usr/include/lua5.1

PGXS := $(shell pg_config --pgxs)
include $(PGXS)

%.sql: %.source
	rm -f $@; \
	C=`pg_config --pkglibdir`; \
	sed -e "s:_OBJWD_:$$C:g" < $< > $@

pllua.so : pllua.o plluaspi.o
	$(CC) -shared -o $@ $^ -llua5.1


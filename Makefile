# Makefile for PL/Lua
# $Id: Makefile,v 1.6 2008/01/09 17:56:18 carvalho Exp $

# Lua specific
PG_CPPFLAGS = -I/usr/include/lua5.1
LUALIB = lua5.1 # lua

# no need to edit below here

MODULES = pllua plluau
DATA_built = pllua.sql
EXTRA_CLEAN = plluaapi.o plluaspi.o

PGXS := $(shell pg_config --pgxs)
include $(PGXS)

pllua.so : pllua.o plluaapi.o plluaspi.o
	$(CC) -shared -o $@ $^ -l$(LUALIB)

plluau.so : plluau.o plluaapi.o plluaspi.o
	$(CC) -shared -o $@ $^ -l$(LUALIB)


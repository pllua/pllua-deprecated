# Makefile for PL/Lua
# $Id: Makefile,v 1.7 2008/01/14 16:29:48 carvalho Exp $

# Lua specific
LUAINC = -I/usr/include/lua5.1
LUALIB = -llua5.1

# no need to edit below here
MODULE_big = pllua
DATA_built = pllua.sql
OBJS = pllua.o plluaapi.o plluaspi.o
PG_CPPFLAGS = $(LUAINC)
SHLIB_LINK = $(LUALIB)

PGXS := $(shell pg_config --pgxs)
include $(PGXS)


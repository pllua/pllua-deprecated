# Makefile for PL/Lua
# $Id: Makefile,v 1.11 2009/09/19 16:20:45 carvalho Exp $

# Lua specific

# General
LUAINC =
LUALIB = -llua

# Debian/Ubuntu
#LUAINC = -I/usr/include/lua5.1
#LUALIB = -llua5.1

# Fink
#LUAINC = -I/sw/include -I/sw/include/postgresql
#LUALIB = -L/sw/lib -llua

# no need to edit below here
MODULE_big = pllua
DATA_built = pllua.sql
REGRESS = plluatest
OBJS = pllua.o plluaapi.o plluaspi.o
PG_CPPFLAGS = $(LUAINC)
SHLIB_LINK = $(LUALIB)

#PG_CONFIG = /usr/local/pgsql/bin/pg_config
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)


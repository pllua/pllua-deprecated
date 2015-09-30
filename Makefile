# Makefile for PL/Lua
# $Id: Makefile,v 1.12 2009/09/20 14:20:48 carvalho Exp $
PG_CONFIG ?= pg_config
PKG_LIBDIR := $(shell $(PG_CONFIG) --pkglibdir)
# Lua specific

# General
LUA_INCDIR ?= /usr/include/lua5.1
LUALIB ?= -L/usr/local/lib -llua

# LuaJIT
#LUAINC = -I/usr/local/include/luajit-2.0
#LUALIB = -L/usr/local/lib -lluajit-5.1

# Debian/Ubuntu
#LUAINC = -I/usr/include/lua5.1
#LUALIB = -llua5.1

# Fink
#LUAINC = -I/sw/include -I/sw/include/postgresql
#LUALIB = -L/sw/lib -llua

# Lua for Windows
#LUAINC = -IC:/PROGRA~1/Lua/5.1/include
#LUALIB = -LC:/PROGRA~1/Lua/5.1/lib -llua5.1

# no need to edit below here
MODULE_big = pllua
EXTENSION = pllua
DATA = pllua--1.0.sql
#DATA_built = pllua.sql

REGRESS = \
plluatest \
biginttest

OBJS = \
pllua.o \
pllua_debug.o \
plluaapi.o \
plluaspi.o \
lua_int64.o \
rtupdesc.o \
rtupdescstk.o

PG_CPPFLAGS = -I$(LUA_INCDIR) #-DPLLUA_DEBUG
SHLIB_LINK = $(LUALIB)


PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)



/**
Copyright (c) 2012-2013 codingow.com

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software,
and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


source:https://github.com/idning/lua-int64.git
*/

#include "lua_int64.h"


#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#ifndef _MSC_VER
#include <stdbool.h>
#include <inttypes.h>
#endif

#include "pllua.h"
static const char int64_type_name[] = "int64";

static int64_t check_int64(lua_State* L, int idx) {\
    int64_t* p;
    luaL_checktype(L,idx,LUA_TUSERDATA);
    p = (int64_t*)luaL_checkudata(L, idx, int64_type_name);
    return p ? *p : 0;
}

#define get_ab_values  \
    int64_t a;\
    int64_t b;\
    if(lua_isnil(L,1) || lua_isnil(L,2)) \
    return luaL_error(L, "attempt to perform arithmetic on a nil value"); \
    a = get_int64(L,1); \
    b = get_int64(L,2)

static int64_t
get_int64(lua_State *L, int index) {
    int type = lua_type(L,index);
    int64_t value = 0;

    switch(type) {
    case LUA_TNUMBER: {
        return (int64_t)(luaL_checknumber(L,index));
    }
    case LUA_TSTRING: {

#ifdef  _MSC_VER
            return(int64_t)_strtoi64(lua_tostring(L, index), NULL, 0);
#else
            return(int64_t)strtoll(lua_tostring(L, index), NULL, 0);
#endif
    }
    case LUA_TUSERDATA:
        value = check_int64(L, index);
        break;

    default:
        return luaL_error(L, "argument %d error type %s", index, lua_typename(L,type));
    }
    return value;
}

static inline void
_pushint64(lua_State *L, int64_t n) {
    int64_t * udata = (int64_t *)lua_newuserdata(L, sizeof(int64_t ));
    *udata = n;
    luaL_getmetatable(L, int64_type_name);
    lua_setmetatable(L, -2);
}

static int
int64_add(lua_State *L) {
    get_ab_values;
    _pushint64(L, a+b);

    return 1;
}


static int
int64_new(lua_State *L) {
    int top = lua_gettop(L);

    int64_t n;
    switch(top) {
    case 0 :
        _pushint64(L,0);
        break;
    case 1 :
        n = get_int64(L,1);
        lua_pop(L, 1);
        _pushint64(L,n);
        break;
    default: {
        const char * str;
        int base = luaL_checkinteger(L,2);
        if (base < 2) {
            luaL_error(L, "base must be >= 2");
        }
        str = luaL_checkstring(L, 1);
#ifdef  _MSC_VER
            n = _strtoi64(str, NULL, base);
#else
            n = strtoll(str, NULL, base);
#endif

        _pushint64(L,n);
        break;
    }
    }
    return 1;
}


static int
int64_sub(lua_State *L) {
    get_ab_values;
    _pushint64(L, a-b);
    return 1;
}

static int
int64_mul(lua_State *L) {
    get_ab_values;
    _pushint64(L, a * b);
    return 1;
}

static int
int64_div(lua_State *L) {
    get_ab_values;
    if (b == 0) {
        return luaL_error(L, "div by zero");
    }
    _pushint64(L, a / b);

    return 1;
}

static int
int64_mod(lua_State *L) {
    get_ab_values;
    if (b == 0) {
        return luaL_error(L, "mod by zero");
    }
    _pushint64(L, a % b);

    return 1;
}

static int64_t
_pow64(int64_t a, int64_t b) {
    int64_t a2;
    if (b == 1) {
        return a;
    }
    a2 = a * a;
    if (b % 2 == 1) {
        return _pow64(a2, b/2) * a;
    } else {
        return _pow64(a2, b/2);
    }
}

static int
int64_pow(lua_State *L) {
    int64_t p;
    get_ab_values;
    if (b > 0) {
        p = _pow64(a,b);
    } else if (b == 0) {
        p = 1;
    } else {
        return luaL_error(L, "pow by nagtive number %d",(int)b);
    }
    _pushint64(L, p);

    return 1;
}

static int
int64_unm(lua_State *L) {
    int64_t a = get_int64(L,1);
    _pushint64(L, -a);
    return 1;
}

static int
int64_eq(lua_State *L) {
    get_ab_values;
    lua_pushboolean(L,a == b);
    return 1;
}

static int
int64_lt(lua_State *L) {
    get_ab_values;
    lua_pushboolean(L,a < b);
    return 1;
}

static int
int64_le(lua_State *L) {
    get_ab_values;
    lua_pushboolean(L,a <= b);
    return 1;
}

static int
int64_len(lua_State *L) {
    int64_t a = get_int64(L,1);
    lua_pushnumber(L,(lua_Number)a);
    return 1;
}


static int
tostring(lua_State *L) {
    static char hex[16] = "0123456789ABCDEF";
    int64_t n = check_int64(L,1);
    if (lua_gettop(L) == 1) {
        char str[24];
#ifndef _MSC_VER
        sprintf(str, "%" PRId64, n);
#else
        sprintf(str,"%10lld", (long long)n);
#endif
        lua_pushstring(L,str);
    } else {
        int i;
        char buffer[64];
        int base = luaL_checkinteger(L,2);
        int shift = 1;
        int mask = 2;
        switch(base) {
        case 0: {
            unsigned char buffer[8];
            int i;
            for (i=0;i<8;i++) {
                buffer[i] = (n >> (i*8)) & 0xff;
            }
            lua_pushlstring(L,(const char *)buffer, 8);
            return 1;
        }
        case 10: {
            int buffer[32], i;

            int64_t dec = (int64_t)n;
            luaL_Buffer b;
#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM < 502
            luaL_buffinit(L, &b);
#else
            luaL_buffinitsize(L , &b , 28);
#endif
            if (dec<0) {
                luaL_addchar(&b, '-');
                dec = -dec;
            }

            for (i=0;i<32;i++) {
                buffer[i] = dec%10;
                dec /= 10;
                if (dec == 0)
                    break;
            }
            while (i>=0) {
                luaL_addchar(&b, hex[buffer[i]]);
                --i;
            }
            luaL_pushresult(&b);
            return 1;
        }
        case 2:
            shift = 1;
            mask = 1;
            break;
        case 8:
            shift = 3;
            mask = 7;
            break;
        case 16:
            shift = 4;
            mask = 0xf;
            break;
        default:
            luaL_error(L, "Unsupport base %d",base);
            break;
        }

        for (i=0;i<64;i+=shift) {
            buffer[i/shift] = hex[(n>>(64-shift-i)) & mask];
        }
        lua_pushlstring(L, buffer, 64 / shift);
    }
    return 1;
}


#undef get_ab_values


void register_int64(lua_State *L)
{
    if (sizeof(long long int)==sizeof(int64_t)) {
        luaL_Reg regs[] =
        {
            { "new", int64_new },
            { "tostring", tostring },
            { "__add", int64_add },
            { "__sub", int64_sub },
            { "__mul", int64_mul },
            { "__div", int64_div },
            { "__mod", int64_mod },
            { "__unm", int64_unm },
            { "__pow", int64_pow },
            { "__eq", int64_eq },
            { "__lt", int64_lt },
            { "__le", int64_le },
            { "__len", int64_len },
            { "__tostring", tostring },

            { NULL, NULL }
        };


        luaL_newmetatable(L, int64_type_name);
        luaL_setfuncs(L, regs, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -1, "__index");
        lua_setglobal(L, int64_type_name);

    }
}

int64 get64lua(lua_State *L, int index)
{
    return get_int64(L,index);
}


void setInt64lua(lua_State *L, int64 value)
{
    _pushint64(L,value);
}

CREATE OR REPLACE FUNCTION i_void(internal)
  RETURNS internal AS
$BODY$
-- mymodule.lua
local M = {} -- public interface

-- private
local x = 1
local function baz() print 'test' end

function M.foo() print("foo", x) end

function M.bar()
  M.foo()
  baz()
  print "bar"
end

function M.getAnswer()
  return 42
end

return M

$BODY$ LANGUAGE pllua;

CREATE OR REPLACE FUNCTION pg_temp.pgfunc_test()
RETURNS SETOF text AS $$
  local quote_ident = pgfunc("quote_ident(text)")
  coroutine.yield(quote_ident("int"))
  local right = pgfunc("right(text,int)")
  coroutine.yield(right('abcde', 2))
  local factorial = pgfunc("factorial(int8)")
  coroutine.yield(tostring(factorial(50)))
  local i_void = pgfunc("i_void(internal)")
  coroutine.yield(i_void.getAnswer())
$$ LANGUAGE pllua;
select pg_temp.pgfunc_test();

do $$
print(pgfunc('quote_nullable(text)')(nil))
$$ language pllua;

create or replace function pg_temp.throw_error(text) returns void as $$
begin
raise exception '%', $1;
end
$$ language plpgsql;

do $$
pgfunc('pg_temp.throw_error(text)',{only_internal=false})("exception test")
$$ language pllua;

do $$
local f = pgfunc('pg_temp.throw_error(text)',{only_internal=false})
print(pcall(f, "exception test"))
$$ language pllua;

create or replace function pg_temp.no_throw() returns json as $$
select '{"a":5, "b":10}'::json
$$ language sql;

do $$
local f = pgfunc('pg_temp.no_throw()',{only_internal=false, throwable=false})
print(f())
$$ language pllua;
CREATE or replace FUNCTION pg_temp.arg_count(a1 integer,a2 integer,a3 integer,a4 integer,a5 integer
,a6 integer,a7 integer,a8 integer,a9 integer,a10 integer
,a11 integer,a12 integer,a13 integer,a14 integer,a15 integer ) returns integer AS
$$
begin
return a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14+a15;
end
$$
LANGUAGE plpgsql;
do $$
local f = pgfunc([[pg_temp.arg_count(integer, integer, integer, integer, integer,
 integer, integer, integer, integer, integer, 
 integer, integer, integer, integer, integer ) ]],{only_internal=false});
print(f(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15))
$$ language pllua;
CREATE or replace FUNCTION pg_temp.inoutf(a integer, INOUT b text, INOUT c text)  AS
$$
begin
c = a||'c:'||c;
b = 'b:'||b;
end
$$
LANGUAGE plpgsql;
do $$
local f = pgfunc('pg_temp.inoutf(integer,text,text)',{only_internal=false});
local r = f(5, 'ABC', 'd')
print(r.b)
print(r.c)
$$ language pllua;
do $$
local f = pgfunc('generate_series(int,int)')
print('4-6')
for rr in f(4,6) do
	print(rr)
end

print('1-3')
for rr in f(1,3) do
	print(rr)
end

$$ language pllua;
do $$
local f = pgfunc('generate_series(int,int)')
for rr in f(1,3) do

	for rr in f(41,43) do
		print(rr)
	end
	print(rr)
end
$$ language pllua;

-- Type wrapper

create extension hstore;
do $$
local hstore = {
	fromstring = function(text)
		return fromstring('hstore',text)
	end,
	akeys = pgfunc('akeys(hstore)',{only_internal = false}),
	each = pgfunc('each(hstore)',{only_internal = false}) --orig:each(IN hs hstore, OUT key text, OUT value text)
}
 
local v = hstore.fromstring[[
	"paperback" => "542",
	"publisher" => "postgresql.org",
	"language"  => "English",
	"ISBN-13"   => "978-0000000000",
	"weight"    => "24.1 ounces"
]]

print(v)

for _,v in ipairs(hstore.akeys(v)) do
	print (v)
end

for hv in hstore.each(v) do
	print ("key = " .. hv.key .. "    value = "..hv.value)
end
 $$ language pllua;

create or replace function getnull() returns text as $$
begin
return null;
end
$$ language plpgsql;

do $$
local a = pgfunc('getnull()',{only_internal = false})
print(a())
$$ language pllua;


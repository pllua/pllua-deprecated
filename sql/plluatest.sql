\set ECHO none
--\i pllua.sql
CREATE EXTENSION pllua;
\set ECHO all

-- minimal function
CREATE FUNCTION hello(name text)
RETURNS text AS $$
  return string.format("Hello, %s!", name)
$$ LANGUAGE pllua;
SELECT hello('PostgreSQL');

-- null handling
CREATE FUNCTION max(a integer, b integer) RETURNS integer AS $$
  if a == nil then return b end -- first arg is NULL?
  if b == nil then return a end -- second arg is NULL?
  return a > b and a or b -- return max(a, b)
$$ LANGUAGE pllua;
SELECT max(1,2), max(2,1), max(2,null), max(null, 2), max(null, null);

-- plain recursive
CREATE FUNCTION fib(n int) RETURNS int AS $$
  if n < 3 then
    return n
  else
    return fib(n - 1) + fib(n - 2)
  end
$$ LANGUAGE pllua;
SELECT fib(4);

-- memoized
CREATE FUNCTION fibm(n integer) RETURNS integer AS $$
  if n < 3 then return n
  else
    local v = _U[n]
    if not v then
      v = fibm(n - 1) + fibm(n - 2)
      _U[n] = v
    end
    return v
  end
end
do _U = {}
$$ LANGUAGE pllua;
SELECT fibm(4);

-- tail recursive
CREATE FUNCTION fibt(n integer) RETURNS integer AS $$
  return _U(n, 0, 1)
end
_U = function(n, a, b)
  if n < 1 then return b
  else return _U(n - 1, b, a + b) end
$$ LANGUAGE pllua;
SELECT fibt(4);

-- iterator
CREATE FUNCTION fibi() RETURNS integer AS $$
  while true do
    _U.curr, _U.next = _U.next, _U.curr + _U.next
    coroutine.yield(_U.curr)
  end
end
do
  _U = {curr = 0, next = 1}
  fibi = coroutine.wrap(fibi)
$$ LANGUAGE pllua;
SELECT fibi(), fibi(), fibi(), fibi(), fibi();
SELECT fibi(), fibi(), fibi(), fibi(), fibi();

-- upvalue
CREATE FUNCTION counter() RETURNS int AS $$
  while true do
    _U = _U + 1
    coroutine.yield(_U)
  end
end
do
  _U = 0 -- counter
  counter = coroutine.wrap(counter)
$$ LANGUAGE pllua;
SELECT counter();
SELECT counter();
SELECT counter();

-- record input
CREATE TYPE greeting AS (how text, who text);
CREATE FUNCTION makegreeting (g greeting, f text) RETURNS text AS $$
  return string.format(f, g.how, g.who)
$$ LANGUAGE pllua;
SELECT makegreeting(('how', 'who'), '%s, %s!');

-- array, record output
CREATE FUNCTION greetingset (how text, who text[])
RETURNS SETOF greeting AS $$
  for _, name in ipairs(who) do
    coroutine.yield{how=how, who=name}
  end
$$ LANGUAGE pllua;
SELECT makegreeting(greetingset, '%s, %s!') FROM
  (SELECT greetingset('Hello', ARRAY['foo', 'bar', 'psql'])) AS q;

-- more array, upvalue
CREATE FUNCTION perm (a text[]) RETURNS SETOF text[] AS $$
  _U(a, #a)
end
do
  _U = function (a, n) -- permgen in PiL
    if n == 0 then
      coroutine.yield(a) -- return next SRF row
    else
      for i = 1, n do
        a[n], a[i] = a[i], a[n] -- i-th element as last one
        _U(a, n - 1) -- recurse on head
        a[n], a[i] = a[i], a[n] -- restore i-th element
      end
    end
  end
$$ LANGUAGE pllua;
SELECT * FROM perm(array['1', '2', '3']);

-- shared variables
CREATE FUNCTION getcounter() RETURNS integer AS $$
  if shared.counter == nil then -- not cached?
    setshared("counter", 0)
  end
  return counter -- _G.counter == shared.counter
$$ LANGUAGE pllua;
CREATE FUNCTION setcounter(c integer) RETURNS void AS $$
  if shared.counter == nil then -- not cached?
    setshared("counter", c)
  else
    counter = c -- _G.counter == shared.counter
  end
$$ LANGUAGE pllua;
SELECT getcounter();
SELECT setcounter(5);
SELECT getcounter();

-- SPI usage

CREATE TABLE sometable ( sid int4, sname text, sdata text);
INSERT INTO sometable VALUES (1, 'name', 'data');

CREATE FUNCTION get_rows (i_name text) RETURNS SETOF sometable AS $$
  if _U == nil then -- plan not cached?
    local cmd = "SELECT sid, sname, sdata FROM sometable WHERE sname = $1"
    _U = server.prepare(cmd, {"text"}):save()
  end
  local c = _U:getcursor({i_name}, true) -- read-only
  while true do
    local r = c:fetch(1)
    if r == nil then break end
    r = r[1]
    coroutine.yield{sid=r.sid, sname=r.sname, sdata=r.sdata}
  end
  c:close()
$$ LANGUAGE pllua;

SELECT * FROM get_rows('name');


CREATE TABLE tree (id INT PRIMARY KEY, lchild INT, rchild INT);

CREATE FUNCTION filltree (t text, n int) RETURNS void AS $$
  local p = server.prepare("insert into " .. t .. " values($1, $2, $3)",
    {"int4", "int4", "int4"})
  for i = 1, n do
    local lchild, rchild = 2 * i, 2 * i + 1 -- siblings
    p:execute{i, lchild, rchild} -- insert values
  end
$$ LANGUAGE pllua;
SELECT filltree('tree', 10);

CREATE FUNCTION preorder (t text, s int) RETURNS SETOF int AS $$
  coroutine.yield(s)
  local q = server.execute("select * from " .. t .. " where id=" .. s,
      true, 1) -- read-only, only 1 result
  if q ~= nil then
    local lchild, rchild = q[1].lchild, q[1].rchild -- store before next query
    if lchild ~= nil then preorder(t, lchild) end
    if rchild ~= nil then preorder(t, rchild) end
  end
$$ LANGUAGE pllua;
SELECT * from preorder('tree', 1);

CREATE FUNCTION postorder (t text, s int) RETURNS SETOF int AS $$
  local p = _U[t]
  if p == nil then -- plan not cached?
    p = server.prepare("select * from " .. t .. " where id=$1", {"int4"})
    _U[t] = p:save()
  end
  local c = p:getcursor({s}, true) -- read-only
  local q = c:fetch(1) -- one row
  if q ~= nil then
    local lchild, rchild = q[1].lchild, q[1].rchild -- store before next query
    c:close()
    if lchild ~= nil then postorder(t, lchild) end
    if rchild ~= nil then postorder(t, rchild) end
  end
  coroutine.yield(s)
end
do _U = {} -- plan cache
$$ LANGUAGE pllua;
SELECT * FROM postorder('tree', 1);


-- trigger
CREATE FUNCTION treetrigger() RETURNS trigger AS $$
  local row, operation = trigger.row, trigger.operation
  if operation == "update" then
    trigger.row = nil -- updates not allowed
  elseif operation == "insert" then
    local id, lchild, rchild = row.id, row.lchild, row.rchild
    if lchild == rchild or id == lchild or id == rchild -- avoid loops
        or (lchild ~= nil and _U.intree(lchild)) -- avoid cycles
        or (rchild ~= nil and _U.intree(rchild))
        or (_U.nonemptytree() and not _U.isleaf(id)) -- not leaf?
        then
      trigger.row = nil -- skip operation
    end
  else -- operation == "delete"
    if not _U.isleafparent(row.id) then -- not both leaf parent?
      trigger.row = nil
    end
  end
end
do
  local getter = function(cmd, ...)
    local plan = server.prepare(cmd, {...}):save()
    return function(...)
      return plan:execute({...}, true) ~= nil
    end
  end
  _U = { -- plan closures
    nonemptytree = getter("select * from tree"),
    intree = getter("select node from (select id as node from tree "
      .. "union select lchild from tree union select rchild from tree) as q "
      .. "where node=$1", "int4"),
    isleaf = getter("select leaf from (select lchild as leaf from tree "
      .. "union select rchild from tree except select id from tree) as q "
      .. "where leaf=$1", "int4"),
    isleafparent = getter("select lp from (select id as lp from tree "
      .. "except select ti.id from tree ti join tree tl on ti.lchild=tl.id "
      .. "join tree tr on ti.rchild=tr.id) as q where lp=$1", "int4")
  }
$$ LANGUAGE pllua;

CREATE TRIGGER tree_trigger BEFORE INSERT OR UPDATE OR DELETE ON tree
  FOR EACH ROW EXECUTE PROCEDURE treetrigger();

SELECT * FROM tree WHERE id = 1;
UPDATE tree SET rchild = 1 WHERE id = 1;
SELECT * FROM tree WHERE id = 10;
DELETE FROM tree where id = 10;
DELETE FROM tree where id = 1;

-- passthru types
CREATE FUNCTION echo_int2(arg int2) RETURNS int2 AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_int2('12345');
CREATE FUNCTION echo_int4(arg int4) RETURNS int4 AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_int4('1234567890');
CREATE FUNCTION echo_int8(arg int8) RETURNS int8 AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_int8('1234567890');
SELECT echo_int8('12345678901236789');
SELECT echo_int8('1234567890123456789');
CREATE FUNCTION echo_text(arg text) RETURNS text AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_text('qwe''qwe');
CREATE FUNCTION echo_bytea(arg bytea) RETURNS bytea AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_bytea('qwe''qwe');
SELECT echo_bytea(E'q\\000w\\001e''q\\\\we');
CREATE FUNCTION echo_timestamptz(arg timestamptz) RETURNS timestamptz AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_timestamptz('2007-01-06 11:11 UTC') AT TIME ZONE 'UTC';
CREATE FUNCTION echo_timestamp(arg timestamp) RETURNS timestamp AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_timestamp('2007-01-06 11:11');
CREATE FUNCTION echo_date(arg date) RETURNS date AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_date('2007-01-06');
CREATE FUNCTION echo_time(arg time) RETURNS time AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_time('11:11');
CREATE FUNCTION echo_arr(arg text[]) RETURNS text[] AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_arr(array['a', 'b', 'c']);

CREATE DOMAIN mynum AS numeric(6,3);
CREATE FUNCTION echo_mynum(arg mynum) RETURNS mynum AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_mynum(666.777);

CREATE TYPE mytype AS (id int2, val mynum, val_list numeric[]);
CREATE FUNCTION echo_mytype(arg mytype) RETURNS mytype AS $$ return arg $$ LANGUAGE pllua;
SELECT echo_mytype((1::int2, 666.777, array[1.0, 2.0]) );

-- body reload
SELECT hello('PostgreSQL');
CREATE OR REPLACE FUNCTION hello(name text)
RETURNS text AS $$
  return string.format("Bye, %s!", name)
$$ LANGUAGE pllua;
SELECT hello('PostgreSQL');


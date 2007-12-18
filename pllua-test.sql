-- debugging
create function init() returns void as
  $$ setshared("debug") $$ language pllua;

create function lua(s text) returns void as
  $$ assert(loadstring(s))() $$ language pllua;


create function rpower (alpha int) returns int as $$
  local n = 100 -- weight vector size
  local w = upvalue[alpha]
  if w == nil then -- not cached?
    w = {}
    local s = 0
    for i = 1, n do
      local v = i^(-alpha)
      s = s + v
      w[i] = v
    end
    for i = 1, n do -- normalize
      w[i] = w[i] / s
    end
    upvalue[alpha] = w
  end
  -- sample from w
  local u = math.random()
  local s = 0
  for i = 1, n do
    s = s + w[i]
    if u <= s then return i end
  end
  return n -- failsafe
end
do upvalue = {} -- cache
$$ language pllua;

create function hpower (alpha int, n int) returns text as $$
  local p = server.prepare(("select rpower(%d) as sample"):format(alpha))
  local c = {} -- counts
  for i = 1, n do
    local t = p:execute(nil, true, 1) -- execute read-only, 1 row
    local k = t[1].sample
    c[k] = (c[k] or 0) + 1
  end
  local s = {}
  for i = 1, table.maxn(c) do
    s[i] = string.format("%d   %d", i, (c[i] or 0))
  end
  return table.concat(s, "\n")
$$ language pllua;

-- g is an adjacency table with fields f (from) and t (to), s is source,
-- and d is destination
create function dist (g text, f text, t text, s int, d int) returns int as $$
  -- initialize
  local dist = {[s] = 0}
  local notvisited = {}
  local c = server.prepare(string.format("select distinct v from "
      .. "(select %s as v from %s union select %s as v from %s) as q",
      f, g, t, g)):getcursor(nil, true) -- no args, read-only
  repeat -- for each vertex in table g
    local t = c:fetch(1)
    if t ~= nil then -- cursor active?
      notvisited[t[1].v] = true
    end
  until t == nil
  assert(notvisited[s], "source not in table '" .. g .. "'")
  assert(notvisited[d], "destination not in table '" .. g .. "'")
  -- shortest path
  local plan = server.prepare(string.format("select distinct v from "
        .. "(select %s as v from %s where %s=$1 union "
        .. "select %s as v from %s where %s=$1) as q", f, g, t, t, g, f),
        {"int4"})
  while next(notvisited) do
    -- choose min
    local v, s = next(dist)
    while not notvisited[v] do v, s = next(dist, v) end
    for e, u in next, dist, v do
      if notvisited[e] and u < s then v, s = e, u end
    end
    if v == d then return dist[d] end
    notvisited[v] = nil
    local c = plan:getcursor({v}, true) -- read-only
    repeat -- for each neighbor of v
      local t = c:fetch(1)
      if t ~= nil then -- cursor active?
        local w = t[1].v -- neighbor
        local dv = dist[v] + 1 -- cost == 1
        local dw = dist[w]
        if not dw or dw > dv then
          dist[w] = dv -- relax
        end
      end
    until t == nil
  end
  return dist[d] -- failsafe
$$ language pllua;


-- Note: global environment should be used only for shared values!
-- Note: triggers don't return, but read trigger.row for returned value

-- tree traversal
create table tree (id int primary key, lchild int, rchild int);

-- operations only on leaves
create function treetrigger() returns trigger as $$
  local row, operation = trigger.row, trigger.operation
  if operation == "update" then
    trigger.row = nil -- updates not allowed
  else
    local id, lchild, rchild = row.id, row.lchild, row.rchild
    -- check input consistency
    if lchild == rchild or id == lchild or id == rchild -- avoid loops
        or (lchild ~= nil and upvalue.intree(lchild)) -- avoid cycles
        or (rchild ~= nil and upvalue.intree(rchild))
        -- operate on leaves only:
        or (operation == "insert" and not upvalue.emptytree()
          and not upvalue.isleaf(id)) -- not leaf?
        or (operation == "delete"
          and not upvalue.isleafparent(id)) then -- not both leaf parent?
      trigger.row = nil
    end
  end
end
do upvalue = { -- cache plans
  emptytree = function()
    local p = server.prepare("select * from tree"):save()
    return p:execute(nil, true) == nil
  end,
  intree = function(node)
    local p = server.prepare("select node from (select id as node from tree "
      .. "union select lchild from tree union select rchild from tree) as q "
      .. "where node=$1", {"int4"}):save()
    return p:execute({node}, true) ~= nil
  end,
  isleaf = function(node)
    local p = server.prepare("select leaf from (select lchild as leaf from tree "
      .. "union select rchild from tree except select id from tree) as q "
      .. "where leaf=$1", {"int4"}):save()
    return p:execute({node}, true) ~= nil
  end,
  isleafparent = function(node)
    local p = server.prepare("select lp from (select id as lp from tree "
      .. "except select ti.id from tree ti join tree tl on ti.lchild=tl.id "
      .. "join tree tr on ti.rchild=tr.id) as q where lp=$1", {"int4"}):save()
    return p:execute({node}, true) ~= nil
  end
}
$$ language pllua;

create trigger tree_trigger before insert or update or delete on tree
  for each row execute procedure treetrigger();

create function filltree (t text, n int) returns void as $$
  local p = server.prepare("insert into " .. t .. " values($1, $2, $3)",
    {"int4", "int4", "int4"})
  for i = 1, n do
    local lchild, rchild = 2 * i, 2 * i + 1 -- siblings
    p:execute{i, lchild, rchild}
  end
$$ language pllua;


-- preorder without plan caching
create function preorder (t text, s int) returns setof int as $$
  coroutine.yield(s)
  local q = server.execute("select * from " .. t .. " where id=" .. s,
      true, 1)
  if q ~= nil then
    local lchild, rchild = q[1].lchild, q[1].rchild -- store before next query
    if lchild ~= nil then preorder(t, lchild) end
    if rchild ~= nil then preorder(t, rchild) end
  end
$$ language pllua;

-- postorder with plan caching
create function postorder (t text, s int) returns setof int as $$
  local p = upvalue[t]
  if p == nil then -- not saved?
    p = server.prepare("select * from " .. t .. " where id=$1", {"int4"})
    upvalue[t] = p:save()
  end
  local q = p:execute({s}, true, 1) -- read-only, one row
  if q ~= nil then
    local lchild, rchild = q[1].lchild, q[1].rchild -- store before next query
    if lchild ~= nil then postorder(t, lchild) end
    if rchild ~= nil then postorder(t, rchild) end
  end
  coroutine.yield(s)
end
do upvalue = {}
$$ language pllua;


-- tests

create type greeting as (how text, who text);

create function greetingset (how text, who text[])
    returns setof greeting as $$
  for _, name in ipairs(who) do
    coroutine.yield{how=how, who=name}
  end
$$ language pllua;

create function makegreeting (g greeting, f text) returns text as $$
  return string.format(f, g.how, g.who)
$$ language pllua;

select makegreeting(greetingset, '%s, %s!') from
  (select greetingset('Hello', ARRAY['foo', 'bar', 'psql'])) as q;


-- permutation
create function perm (a text[]) returns setof text[] as $$
  upvalue(a, #a)
end
do
  upvalue = function (a, n)
    if n == 0 then
      coroutine.yield(a)
    else
      for i = 1, n do
        a[n], a[i] = a[i], a[n]
        upvalue(a, n - 1)
        a[n], a[i] = a[i], a[n]
      end
    end
  end
  --perm = coroutine.wrap(perm)
$$ language pllua;

-- simple counter
create function counter() returns int as $$
  while true do
    upvalue = upvalue + 1
    coroutine.yield(upvalue)
  end
end
do
  upvalue = 0
  counter = coroutine.wrap(counter)
$$ language pllua;

-- Fibonacci
-- plain recursive
create function fib(n integer) returns integer as $$
  if n < 3 then return n
  else return fib(n - 1) + fib(n - 2) end
$$ language pllua;

-- memoized
create function fibm(n integer) returns integer as $$
  if n < 3 then return n
  else
    local v = upvalue[n]
    if not v then
      v = fibm(n - 1) + fibm(n - 2)
      upvalue[n] = v
    end
    return v
  end
end
do upvalue = {}
$$ language pllua;

-- tail recursive
create function fibt(n integer) returns integer as $$
  return upvalue(n, 0, 1)
end
upvalue = function(n, a, b)
  if n < 1 then return b
  else return upvalue(n - 1, b, a + b) end
$$ language pllua;

-- iterator
create function fibi() returns integer as $$
  while true do
    upvalue.curr, upvalue.next = upvalue.next, upvalue.curr + upvalue.next
    coroutine.yield(upvalue.curr)
  end
end
do
  upvalue = {curr = 0, next = 1}
  fibi = coroutine.wrap(fibi)
$$ language pllua;

-- factorial
create function lfact(n double precision) returns double precision as $$
  return n < 2 and 0 or lfact(n - 1) + math.log(n)
$$ language pllua;

create function lfactt(n double precision) returns double precision as $$
  return upvalue(n, 0)
end
upvalue = function(n, a)
  if n < 2 then
    return a
  else
    return upvalue(n - 1, a + math.log(n))
  end
$$ language pllua;


-- test suite
create function hello () returns text as $$
  return "Hello!"
$$ language pllua;

create function counter () returns int as $$
  upvalue = upvalue + 1
  return upvalue
end
do upvalue = 0
$$ language pllua;

create function global1 () returns text as $$
  if shared.test == nil then
    setshared("test", "set by global1")
  end
  return "shared.test = " .. shared.test
$$ language pllua;

create function global2 () returns text as $$
  if shared.test == nil then
    setshared("test", "set by global2")
  end
  return "shared.test = " .. shared.test
$$ language pllua;

create function lua_require (m text) returns void as $$
  require(m)
$$ language plluau;

create function lua_md5 (t text) returns text as $$
  upvalue:reset()
  upvalue:update(t)
  return upvalue:digest()
end
do upvalue = require"md5".new()
$$ language plluau;

create table people (name text, lname text);
insert into people values ('Roberto', 'Ierusalimschy');
insert into people values ('Luiz', 'de Figueiredo');

create function md5people (p people) returns text as $$
  local s = p.name .. " " .. p.lname
  upvalue:reset()
  upvalue:update(s)
  return s .. " => " .. upvalue:digest()
end
do upvalue = require"md5".new()
$$ language plluau;

create function argpeople (p people, f1 text, f2 text) returns text as $$
  local r = {}
  local t = {f1, f2}
  for k, v in pairs(p) do
    r[#r + 1] = string.format("%s: %s", k, v)
  end
  t[3] = "{ " .. table.concat(r, ", ") .. " }"
  return table.concat(t, " | ")
$$ language pllua;

create function nested1 (a text) returns text as $$
  local r = server.execute(string.format("select nested2('%s') as name", a))
  return r[1].name
$$ language pllua;

create function nested2 (a text) returns text as $$
  local r = server.execute(string.format("select nested3('%s') as name", a))
  return r[1].name
$$ language pllua;

create function nested3 (a text) returns text as $$
  return a
$$ language pllua;


-- int_interval

create table features (feat int_interval);

create function insert_ii(m int, n int) returns void as $$
  for i=1,n do 
    local x,y = math.random(1, m), math.random(1, m)
    if x>y then x,y = y,x end
    server.execute(string.format("insert into features values('(%d, %d)')",
        x, y))
  end
$$ language pllua;


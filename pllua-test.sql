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
do upvalue = upvalue or {} -- cache
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


create function preorder (t text, s int) returns setof int as $$
  local r = server.execute("select * from "..t.." where id="..s, true, 1)
  if r ~= nil then
    local r = r[1]
    if r.l ~= nil then preorder(t, r.l) end
    if r.r ~= nil then preorder(t, r.r) end
  end
  coroutine.yield(s)
$$ language pllua;

create function postorder (t text, s int) returns setof int as $$
  coroutine.yield(s)
  local r = server.execute("select * from "..t.." where id="..s, true, 1)
  if r ~= nil then
    local r = r[1]
    if r.l ~= nil then postorder(t, r.l) end
    if r.r ~= nil then postorder(t, r.r) end
  end
$$ language pllua;

-- tests

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


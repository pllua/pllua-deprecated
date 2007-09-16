-- debugging
create function init() returns void as
  $$ setshared("debug") $$ language pllua;

create function lua(s text) returns void as
  $$ assert(loadstring(s))() $$ language pllua;


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


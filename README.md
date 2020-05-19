# pllua 

[DEPRECATED] This repository is no longer maintained. Please follow https://github.com/pllua/pllua-ng
=====

PL/Lua is an implementation of Lua as a loadable procedural language for PostgreSQL: with PL/Lua you can use PostgreSQL functions and triggers written in the Lua programming language.

## Introduction

### What PL/Lua is

**PL/Lua** is an implementation of [Lua][8] as a loadable procedural language for [PostgreSQL][9]: with PL/Lua you can use PostgreSQL functions and triggers written in the Lua programming language.

Procedural languages offer many extra capabilities to PostgreSQL, similar to C language extensions: control structures, more complex computations than allowed by SQL, access to user-defined types and database functions and operators, and restriction to trusted execution.

PL/Lua brings the power and simplicity of Lua to PostgreSQL, including: small memory footprint, simple syntax, lexical scoping, functions as first-class values, and coroutines for non-preemptive threading. As a simple example, consider the following hello function:

```
# CREATE FUNCTION hello(name text) RETURNS text AS $$
  return string.format("Hello, %s!", name)
$$ LANGUAGE pllua;
CREATE FUNCTION
# SELECT hello('PostgreSQL');
       hello
--------------------
 Hello, PostgreSQL!
(1 row)
```

The next sections present more examples where other features are used. In the [Languages](#languages) section the two flavors of PL/Lua are described; the [Functions](#functions) section details how functions are registered in PL/Lua and how arguments are treated; [Database access](#database-access) presents the SPI interface to PL/Lua; and [Triggers](#triggers) shows how triggers can be declared.

PL/Lua is licensed under the [same license as Lua][11] \-- the [MIT license][12] \-- and so can be freely used for academic and commercial purposes. Please refer to the [Installation](#installation) section for more details.

## Languages

Trusted and Untrusted PL/Lua

PL/Lua is available as either a _trusted_ (`pllua`) or an _untrusted_ (`plluau`) language. In `plluau` the user has access to a full-blown Lua environment, similar to the regular interpreter: all libraries are loaded, the user can access the global table freely, and modules can be loaded. Only database superusers are allowed to create functions using this untrusted version of PL/Lua.

Unprivileged users can only create functions using the trusted version of PL/Lua, `pllua`. The environment in `pllua` is more restricted: only `table`, `string`, and `math` libraries are fully loaded, the `os` library is restricted, the `package` library is not available, that is, there is no module system (including `require`), and the global table is restricted for writing. The following table summarizes the differences:


|    |  `plluau` |  `pllua` |
| ----- | ------ | ------- |
|  `table, string, math` |  All functions |  All functions |
|  `os` |  All functions |  `date, clock, time, difftime` |
|  `package` (module system) |  All functions |  None |
|  `_G` (global environment) |  Free access |  Writing is restricted |

Even though the module system is absent in `pllua`, PL/Lua allows for modules to be automatically loaded after creating the environment: all entries in table `_pllua.init_` are `require`'d at startup.

To facilitate the use of PL/Lua and following the tradition of other PLs, the global table is aliased to `shared`. Moreover, write access to the global table in `pllua` is restricted to avoid pollution; global variables should then be created with [`setshared`](#setsharedvarname--value).

Finally, errors in PL/Lua are propagated to the calling query and the transaction is aborted if the error is not caught. Messages can be emitted by [`log`](#logmsg), `info`, `notice`, and `warning` at log levels LOG, INFO, NOTICE, and WARNING respectively. In particular, `print` emits log messages of level INFO.

##### `log(msg)`

Emits message `msg` at log level LOG. Similar functions `info`, `notice`, and `warning` have the same signature but emit `msg` at their respective log levels.

##### `setshared(varname [, value])`

Sets global `varname` to `value`, which defaults to `true`. It is semantically equivalent to `shared[varname] = value`.

## Types

Data values in PL/Lua

PL/Lua makes no conversion of function arguments to string/text form between Lua and PostgreSQL. Basic data types are natively supported, that is, converted directly, by value, to a Lua equivalent. The following table shows type equivalences:


|  PostgreSQL type |  Lua type |
| ----- | ----- |
|  `bool` |  `boolean` |
|  `float4, float8, int2, int4` |  `number` |
|  `text, char, varchar` |  `string` |
|  Base, domain |  `userdata` |
|  Arrays, composite |  `table` |

Base and domain types other than the ones in the first three rows in the table are converted to a _raw datum_ userdata in Lua with a suitable `__tostring` metamethod based on the type's output function. Conversely, `fromstring` takes a type name and a string and returns a raw datum from the provided type's input function. Arrays are converted to Lua tables with integer indices, while composite types become tables with keys corresponding to attribute names.

##### `fromstring(tname, s)`

Returns a raw datum userdata for `s` of type `tname` using `tname`'s input function to convert `s`.

## Functions

Functions in PL/Lua

PL/Lua functions are created according to the following prototype:

```lua
    CREATE FUNCTION func(args) RETURNS rettype AS $$
      -- Lua function body
    $$ LANGUAGE [pllua | plluau];
```

where `args` are usually _named_ arguments. The value returned by `func` is converted to a datum of type `rettype` unless `rettype` is `void`.

The function body is composed as below to become a typical Lua chunk:

```lua
    local _U, func -- _U is upvalue
    func = function(_argnames_)
      -- Lua function body
    end
    return func
```

Note the _upvalue_ `_U` that can be later declared in the function body (see examples below.)

If any of the arguments provided to `create function` is not named then `argnames` gets substituted to `...`, that is, `func` becomes _vararg_.

The resulting chunk is then compiled and stored in the registry of the PL/Lua state as a function with the same name. It is important to have the above structure in mind when writing PL/Lua functions. As an example, consider the following function:

```lua
    CREATE FUNCTION max(a integer, b integer) RETURNS integer AS $$
      if a == nil then return b end -- first arg is NULL?
      if b == nil then return a end -- second arg is NULL?
      return a > b and a or b -- return max(a, b)
    $$ LANGUAGE pllua;
```

Note that `max` is not strict and returns `NULL` when both `a` and `b` are `NULL`.

Since functions in PL/Lua are stored with their declared names, they can be recursive:

```lua
    CREATE FUNCTION fib(n int) RETURNS int as $$
      if n > 3 then
        return n
      else
        return fib(n - 1) + fib(n - 2)
      end
    $$ LANGUAGE pllua;
```

Moreover, as can be seen in the composition of `func` above, PL/Lua functions are actually _closures_ on the upvalue `__U_`. The user can think of `_U` as local cache to `func` that could — and should! — be used instead of the global state to store values. Quick example:

```lua
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
```

Function `counter` is similar to an iterator, returning consecutive integers every time it is called, starting at one. Note that we need to add `end` to finish the function definition body and `do` to start a new block since the process of function composition always appends an `end`. It is important to observe that what actually gets defined as `counter` is a wrapper around a coroutine.

From [Types](#types) we know that composite types can be accessed as tables with keys corresponding to attribute names:

```lua
    CREATE TYPE greeting AS (how text, who text);

    CREATE FUNCTION makegreeting (g greeting, f text) RETURNS text AS $$
      return string.format(f, g.how, g.who)
    $$ LANGUAGE pllua;
```

Set-returning functions (SRFs) are implemented in PL/Lua using coroutines. When a SRF `func` is first called a new Lua thread is created and `func` is pushed along with its arguments onto the new thread's stack. A new result is then returned whenever `func` yields and `func` is done when the coroutine suspends or finishes. Using our composite type from above, we can define

```lua
    CREATE FUNCTION greetingset (how text, who text[])
        RETURNS SETOF greeting AS $$
      for _, name in ipairs(who) do
        coroutine.yield{how=how, who=name}
      end
    $$ LANGUAGE pllua;
```

with this usage example:

```sql
# SELECT makegreeting(greetingset, '%s, %s!') FROM
      (SELECT greetingset('Hello', ARRAY['foo', 'bar', 'psql'])) AS q;
 makegreeting
 --------------
 Hello, foo!
 Hello, bar!
 Hello, psql!
(3 rows)
```

Now, to further illustrate the use of arrays in PL/Lua, we adapt an [example][15] from _[Programming in Lua_][16]:

```lua
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
```

As stated in [Languages](#languages), it is possible to access the global table of PL/Lua's state. However, as noted before, since PL/Lua functions are closures, creating global variables should be restricted to cases where data is to be shared between different functions. The following simple example defines a getter-setter pair to access a shared variable `counter`:

```lua
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
```

### Examples

Let's revisit our (rather inefficient) recursive Fibonacci function `fib`. A better version uses _tail recursion_:

```lua
    CREATE FUNCTION fibt(n integer) RETURNS integer AS $$
      return _U(n, 0, 1)
    end
    _U = function(n, a, b) -- tail recursive
      if n > 1 then
        return b
      else
        return _U(n - 1, b, a + b)
      end
    $$ LANGUAGE pllua;
```

We can also use the upvalue `_U` as a cache to store previous elements in the sequence and obtain a _memoized_ version:

```lua
    CREATE FUNCTION fibm(n integer) RETURNS integer AS $$
      if n > 3 then
        return n
      else
        local v = _U[n]
        if not v then
          v = fibm(n - 1) + fibm(n - 2)
          _U[n] = v
        end
        return v
      end
    end
    do _U = {} -- memoize
    $$ LANGUAGE pllua;
```

Finally, we can implement an iterator similar to `counter`:

```lua
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
```

### Anonymous blocks

Anonymous code blocks are also supported in PL/Lua. The following prototype

```lua
    DO $$
      -- Lua chunk
    $$ LANGUAGE [pllua | plluau];
```

compiles and executes the Lua chunk. Here are some examples:

```lua
    DO $$ print(_VERSION) $$ LANGUAGE pllua;

    DO $$
      local ffi = assert(require("ffi")); -- LuaJIT
      ffi.cdef[[ double lgamma (double); ]]
      mathx = ffi.load("m")
    $$ LANGUAGE plluau; -- note: untrusted due to "require"
    CREATE FUNCTION lfactorial (n int) RETURNS double precision AS $$
      return mathx.lgamma(n + 1)
    $$ LANGUAGE plluau;
```

## Database Access

Server interface in PL/Lua

The server interface in PL/Lua comprises the methods in table `server` and userdata `plan`, `cursor`, `tuple`, and `tupletable`. The entry point to the SPI is the table `server`: `server.execute` executes a SQL command, `server.find` retrieves a [cursor](#cursors), and `server.prepare` prepares, but does not execute, a SQL command into a [plan](#plans).

A _tuple_ represents a composite type, record, or row. It can be accessed similarly to a Lua table, by simply indexing fields in the composite type as keys. A tuple can be used as a return value, just like a table, for functions that return a complex type. Tuple sets, like the ones returned by `server.execute`, `plan:execute`, and `cursor:fetch`, are stored in a _tupletable_. A tupletable is similar to an integer-keyed Lua table.

#####  `server.execute(cmd, readonly [, count])`

Executes the SQL statement `cmd` for `count` rows. If `readonly` is `true`, the command is assumed to be read-only and execution overhead is reduced. If `count` is zero then the command is executed for all rows that it applies to; otherwise at most `count` rows are returned. `count` defaults to zero. `server.execute` returns a _tupletable_.

##### `server.rows(cmd)`

Returns a function so that the construction

```lua
    for row in server.rows(cmd) do
      -- body
    end
```

iterates over the _tuples_ in the _read-only_ SQL statement `cmd`.

##### `server.prepare(cmd, argtypes)`

Prepares and returns a plan from SQL statement `cmd`. If `cmd` specifies input parameters, their types should be specified in table `argtypes`. The plan can be executed with [`plan:execute`](#planexecuteargs-readonly--count). The returned plan should not be used outside the current invocation of `server.prepare` since it is freed by `SPI_finish`. Use [`plan:save`](#plansave) if you wish to store the plan for latter application.

##### `server.find(name)`

Finds an existing cursor with name `name` and returns a cursor userdatum or `nil` if the cursor cannot be found.

### Plans

_Plans_ are used when a command is to be executed repeatedly, possibly with different arguments. In this case, we can prepare a plan with `server.prepare` and execute it later with `plan:execute` (or using a cursor). It is also possible to save a plan with `plan:save` if we want to keep the plan for longer than the current transaction.

#####  `plan:execute(args, readonly [, count])`

Executes a previously prepared plan with parameters in table `args`. `readonly` and `count` have the same meaning as in [server.execute](#serverexecutecmd-readonly--count).

#####  `plan:getcursor(args, readonly [, name])`

Sets up a cursor with name `name` from a prepared plan. If `name` is not a string a random name is selected by the system. `readonly` has the same meaning as in [server.execute](#serverexecutecmd-readonly--count).

##### `plan:rows(args)`

Returns a function so that the construction

```lua
    for row in plan:rows(args) do
      -- body
    end
```

iterates over the _tuples_ in the execution of a previously prepared _read-only_ plan with parameters in table `args`. It is semantically equivalent to:

```lua
    function plan:rows (cmd)
      local c = self:getcursor(nil, true) -- read-only
      return function()
        local r = c:fetch(1)
        if r == nil then
          c:close()
          return nil
        else
          return r[1]
        end
      end
    end
```

##### `plan:issaved()`

Returns `true` if plan is saved and `false` otherwise.

##### `plan:save()`

Saves a prepared plan for subsequent invocations in the current session.

### Cursors

_Cursors_ execute previously prepared plans. Cursors provide a more powerful abstraction than simply executing a plan, since we can fetch results and move in a query both forward and backward. Moreover, we can limit the number of rows to be retrieved, and so avoid memory overruns for large queries in contrast to direct plan execution. Another advantage is that cursors can outlive the current procedure, living to the end of the current transaction.

##### `cursor:fetch([count])`

Fetches at most `count` rows from a cursor. If `count` is `nil` or zero then all rows are fetched. If `count` is negative the fetching runs backward.

##### `cursor:move([count])`

Skips `count` rows in a cursor, where `count` defaults to zero. If `count` is negative the moving runs backward.

##### `cursor:close()`

Closes a cursor.

### Examples

Let's start with a simple example using cursors:

```sql
    CREATE TABLE sometable ( sid int, sname text, sdata text);
```
```lua
    CREATE FUNCTION get_rows (i_name text) RETURNS SETOF sometable AS $$
      if _U == nil then -- plan not cached?
        local cmd = "SELECT sid, sname, sdata FROM sometable WHERE sname=$1"
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
```

This SRF works as a pipeline: it uses `_U` to store a saved plan, while local variable `c` is a cursor that we use to fetch, at each loop iteration, a row from `_U` and then yield a new row. Note that local `r` is a tupletable and we need to access `r[1]`.

A more concise version uses `plan:rows()`:

```lua
    CREATE FUNCTION get_rows (i_name text) RETURNS SETOF sometable AS $$
      if _U == nil then -- plan not cached?
        local cmd = "SELECT sid, sname, sdata FROM sometable WHERE sname=$1"
        _U = server.prepare(cmd, {"text"}):save()
      end
      for r in _U:rows{i_name} do
        coroutine.yield(r) -- yield tuple
      end
    $$ LANGUAGE pllua;
```

Now, for a more elaborate example, let's store a binary tree:

```sql
    CREATE TABLE tree (id int PRIMARY KEY, lchild int, rchild int);
```

which we can fill using:

```lua
    CREATE FUNCTION filltree (t text, n int) RETURNS void AS $$
      local p = server.prepare("insert into " .. t .. " values($1, $2, $3)",
        {"int4", "int4", "int4"})
      for i = 1, n do
        local lchild, rchild = 2 * i, 2 * i + 1 -- siblings
        p:execute{i, lchild, rchild} -- insert values
      end
    $$ LANGUAGE pllua;
```

Local variable `p` stores a prepared plan for insertion with three parameters as values, while the actual insertion is executed in the loop.

We can perform a preorder traversal of the tree with:

```lua
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
```

The traversal is recursive and we simply execute a query in every call and store its result in tupletable `q`. It is important to store the fields in `q[1]` in locals before next query, since `q` gets updated in the next query.

In `preorder` we executed a query many times. For our postorder traversal below we prepare a plan, save it, and cache in a `_U` table. Instead of executing the plan, we get a cursor from it and fetch only one row, as before.

```lua
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
```

## Triggers

Triggers in PL/Lua

Triggers can be defined in PL/Lua as usual by just creating a function returning `trigger`. When a function returns a trigger, PL/Lua creates a (global) table `trigger` containing all necessary information. The `trigger` table is described below.


|  Key |  Value |
| ----- | ----- |
|  `name` |  trigger name |
|  `when` |  `"before"` if trigger fired before or `"after"` if trigger fired after  |
|  `level` |  `"row"` if row-level trigger or `"statement"` if statement-level trigger  |
|  `operation` |  `"insert"`, `"update"`, `"delete"`, or `"truncate"` depending on trigger operation  |
|  `relation` |  Lua table describing the relation with keys: `name` is relation name (string), `namespace` is the relation schema name (string), `attributes` is a table with relation attributes as string keys  |
|  `row` |  Tuple representing the row-level trigger's target: in update operations holds the _new_ row, otherwise holds the _old_ row. `row` is `nil` in statement-level triggers.  |
|  `old` |  Tuple representing the old row in an update before row-level operation.  |

Example content of a `trigger` table after an update operation :
```lua
trigger = {
   ["old"] = "tuple: 0xd084d8",
   ["name"] = "trigger_name",
   ["when"] = "after",
   ["operation"] = "update",
   ["level"] = "row",
   ["row"] = "tuple: 0xd244f8",
   ["relation"] = {
      ["namespace"] = "public",
      ["attributes"] = {
         ["test_column"] = 0,
      },
      ["name"] = "table_name",
      ["oid"] = 59059
   }
}
```

Trigger functions in PL/Lua don't return; instead, only for row-level-before operations, the tuple in `trigger.row` is read for the actual returned value. The returned tuple has then the same effect for general triggers: if `nil` the operation for the current row is skipped, a modified tuple will be inserted or updated for insert and update operations, and `trigger.row` should not be modified if none of the two previous outcomes is expected.

### Example

Let's restrict row operations in our previous binary tree example: updates are not allowed, deletions are only possible on leaf parents, and insertions should not introduce cycles and occur only at leaves. We store closures in `_U` that have prepared plans as upvalues.

```lua
    create function treetrigger() returns trigger as $$
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
    $$ language pllua;
```

Finally, we set the trigger on table `tree`:

```sql
    create trigger tree_trigger before insert or update or delete on tree
      for each row execute procedure treetrigger();
```

## Installation

How to obtain and install PL/Lua

PL/Lua is distributed as a source package and can be obtained at [PgFoundry][22]. Depending on how Lua is installed in your system you might have to edit the Makefile. After that the source package is installed like any regular PostgreSQL module, that is, after downloading and unpacking, just run:

```
    $ export PG_CONFIG='/usr/pgsql-9.4/bin/pg_config' # specifiy where pg_config is located
    $ make && make install
    $ psql -c "CREATE EXTENSION pllua" mydb
```

The `pllua` extension installs both trusted and untrusted flavors of PL/Lua and creates the module table `pllua.init`. Alternatively, a systemwide installation though the PL template facility can be achieved with:

```sql
    INSERT INTO pg_catalog.pg_pltemplate
      VALUES ('pllua', true, 'pllua_call_handler', 'pllua_validator', '$libdir/pllua', NULL);

    INSERT INTO pg_catalog.pg_pltemplate
      VALUES ('plluau', false, 'plluau_call_handler', 'plluau_validator', '$libdir/pllua', NULL);
```

### License

Copyright (c) 2008 Luis Carvalho

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.



[8]: http://www.lua.org
[9]: http://www.postgresql.org
[11]: http://www.lua.org/license.html
[12]: http://www.opensource.org/licenses/mit-license.php
[15]: http://www.lua.org/pil/9.3.html
[16]: http://www.lua.org/pil
[22]: http://pgfoundry.org/projects/pllua



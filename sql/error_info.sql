do $$ 
local testfunc = function () error("my error") end
local f = function()
	local status, err = pcall(testfunc)
	if (err) then
		error(err)
	end
end
f()
$$language pllua;

create or replace function pg_temp.function_with_error() returns integer as $$
	local testfunc = function () error("my error") end
	local f = function()
		local status, err = pcall(testfunc)
		if (err) then
			error(err)
		end
	end
	f()
$$language plluau;

create or replace function pg_temp.second_function() returns void as $$
	for k in server.rows('select pg_temp.function_with_error()') do
	end
$$language plluau;

do $$ 
	server.execute('select pg_temp.second_function()') 
$$language pllua;

do $$
local status, err = subtransaction(function() assert(1==2) end)
if (err) then
    error(err)
end
$$language pllua;

do $$
info({message="info message", hint="info hint", detail="info detail"})
$$language pllua;

do $$
info("info message")
$$language pllua;

do $$
warning({message="warning message", hint="warning hint", detail="warning detail"})
$$language pllua;

do $$
warning("warning message")
$$language pllua;

do $$
error({message="error message", hint="error hint", detail="error detail"})
$$language pllua;

do $$
error("error message")
$$language pllua;

do $$
info()
$$language pllua;

do $$
warning()
$$language pllua;

do $$
error()
$$language pllua;


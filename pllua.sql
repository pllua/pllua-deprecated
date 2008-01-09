-- $Id: pllua.sql,v 1.1 2008/01/09 17:56:19 carvalho Exp $

CREATE FUNCTION pllua_call_handler()
  RETURNS language_handler AS '$libdir/pllua'
  LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION pllua_validator(oid)
  RETURNS VOID AS '$libdir/pllua'
  LANGUAGE C IMMUTABLE STRICT;

CREATE TRUSTED LANGUAGE pllua
  HANDLER pllua_call_handler
  VALIDATOR pllua_validator;

CREATE FUNCTION plluau_call_handler()
  RETURNS language_handler AS '$libdir/plluau'
  LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION plluau_validator(oid)
  RETURNS VOID AS '$libdir/plluau'
  LANGUAGE C IMMUTABLE STRICT;

CREATE LANGUAGE plluau
  HANDLER plluau_call_handler
  VALIDATOR plluau_validator;

CREATE SCHEMA pllua
  CREATE TABLE init (module text);

-- PL template installation:

--INSERT INTO pg_catalog.pg_pltemplate
--  VALUES ('pllua', true, 'pllua_call_handler', 'pllua_validator',
--    '$libdir/pllua', NULL);

--INSERT INTO pg_catalog.pg_pltemplate
--  VALUES ('plluau', false, 'plluau_call_handler', 'plluau_validator',
--    '$libdir/pllua', NULL);

-- vim: set syn=sql:

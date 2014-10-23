CREATE FUNCTION pllua_call_handler()
  RETURNS language_handler AS '$libdir/pllua'
  LANGUAGE C IMMUTABLE STRICT;

-- comment out if VERSION < 9.0
CREATE FUNCTION pllua_inline_handler(internal)
  RETURNS VOID AS '$libdir/pllua'
  LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION pllua_validator(oid)
  RETURNS VOID AS '$libdir/pllua'
  LANGUAGE C IMMUTABLE STRICT;

CREATE TRUSTED LANGUAGE pllua
  HANDLER pllua_call_handler
  INLINE pllua_inline_handler -- comment out if VERSION < 9.0
  VALIDATOR pllua_validator;

CREATE FUNCTION plluau_call_handler()
  RETURNS language_handler AS '$libdir/pllua'
  LANGUAGE C IMMUTABLE STRICT;

-- comment out if VERSION < 9.0
CREATE FUNCTION plluau_inline_handler(internal)
  RETURNS VOID AS '$libdir/pllua'
  LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION plluau_validator(oid)
  RETURNS VOID AS '$libdir/pllua'
  LANGUAGE C IMMUTABLE STRICT;

CREATE LANGUAGE plluau
  HANDLER plluau_call_handler
  INLINE plluau_inline_handler -- comment out if VERSION < 9.0
  VALIDATOR plluau_validator;

-- Optional:

--CREATE SCHEMA pllua
--  CREATE TABLE init (module text);

-- PL template installation:

--INSERT INTO pg_catalog.pg_pltemplate
--  VALUES ('pllua', true, true, 'pllua_call_handler',
--  'pllua_inline_handler', 'pllua_validator', '$libdir/pllua', NULL);

--INSERT INTO pg_catalog.pg_pltemplate
--  VALUES ('plluau', false, true, 'plluau_call_handler',
--  'plluau_inline_handler', 'plluau_validator', '$libdir/pllua', NULL);

-- vim: set syn=sql:

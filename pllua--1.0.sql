\echo Use "CREATE EXTENSION pllua" to load this file. \quit

CREATE FUNCTION pllua_call_handler()
  RETURNS language_handler AS 'MODULE_PATHNAME'
  LANGUAGE C IMMUTABLE STRICT;

-- comment out if VERSION < 9.0
CREATE FUNCTION pllua_inline_handler(internal)
  RETURNS VOID AS 'MODULE_PATHNAME'
  LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION pllua_validator(oid)
  RETURNS VOID AS 'MODULE_PATHNAME'
  LANGUAGE C IMMUTABLE STRICT;

CREATE TRUSTED LANGUAGE pllua
  HANDLER pllua_call_handler
  INLINE pllua_inline_handler -- comment out if VERSION < 9.0
  VALIDATOR pllua_validator;

CREATE FUNCTION plluau_call_handler()
  RETURNS language_handler AS 'MODULE_PATHNAME'
  LANGUAGE C IMMUTABLE STRICT;

-- comment out if VERSION < 9.0
CREATE FUNCTION plluau_inline_handler(internal)
  RETURNS VOID AS 'MODULE_PATHNAME'
  LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION plluau_validator(oid)
  RETURNS VOID AS 'MODULE_PATHNAME'
  LANGUAGE C IMMUTABLE STRICT;

CREATE LANGUAGE plluau
  HANDLER plluau_call_handler
  INLINE plluau_inline_handler -- comment out if VERSION < 9.0
  VALIDATOR plluau_validator;

--within 'pllua' schema
CREATE TABLE init (module text);

-- PL template installation:
INSERT INTO pg_catalog.pg_pltemplate
  SELECT 'pllua', true, true, 'pllua_call_handler',
    'pllua_inline_handler', 'pllua_validator', 'MODULE_PATHNAME', NULL
  WHERE 'pllua' NOT IN (SELECT tmplname FROM pg_catalog.pg_pltemplate);

INSERT INTO pg_catalog.pg_pltemplate
  SELECT 'plluau', false, true, 'plluau_call_handler',
    'plluau_inline_handler', 'plluau_validator', 'MODULE_PATHNAME', NULL
  WHERE 'plluau' NOT IN (SELECT tmplname FROM pg_catalog.pg_pltemplate);

-- vim: set syn=sql:

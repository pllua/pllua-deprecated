CREATE FUNCTION int64_minus_one(value bigint)
RETURNS bigint AS $$
  return value - 1;
$$ LANGUAGE pllua;
select int64_minus_one(9223372036854775807);

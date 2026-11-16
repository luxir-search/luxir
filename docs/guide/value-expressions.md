# Value expressions: functions and scripting

A value expression computes one number per document from columns, the query
score, and constants. The same language is used in sort clauses, in
[`rescore`](query-reference.md#rescore) and its `min_score` cut-off, in the
per-document input of aggregates (`sum(year(published_dt))`), and in LTR
features. Its arithmetic, `def`, conditionals, array reducers, `random`, and
decay functions are described in
[Searching](searching.md#sort-expressions-in-detail). This page covers the rest
of the language: operators, math, date parts, geo, hashing, local variables,
and how to rewrite Lucene expressions and Painless scoring scripts.

Everything here shares the rules of the base language:

- A document with no value for a column has no value for any expression that
  uses it, unless `def(value, fallback)` supplies one. Missing is not zero.
- A result that is NaN or infinite, or an integer that overflows, is an error
  that names the function. Provably invalid expressions fail before any document
  is read.
- Integers stay exact `int64`; a `double` operand promotes the operation.
- Arrays (multi-valued columns) are reduced with `min`, `max`, `avg`, `sum`, or
  `count`. One-argument math functions, `geo_lat`, `geo_lon`, and the date
  parts map element-wise over an array, which a reducer then consumes:
  `max(geo_lat(places))`.

## Operators

From the loosest to the tightest:

| Operators | Notes |
|---|---|
| `c ? a : b` | Only the chosen branch is evaluated. Right associative. |
| `\|\|` | Short-circuits. |
| `&&` | Short-circuits. |
| `\|` | Bitwise or. |
| `^` | Bitwise xor. |
| `&` | Bitwise and. |
| `==` `!=` `<` `<=` `>` `>=` | Return 1 or 0. They do not chain. |
| `<<` `>>` `>>>` | Shift left, arithmetic shift right, logical shift right. |
| `+` `-` | |
| `*` `/` `%` | `/` always returns a double. `%` is Java's remainder. |
| unary `-` `+` `!` `~` | `!` is logical not, `~` is bitwise not. |

This is Java's precedence. Where Java rejects a mix of types, such as
`2 & 1 == 0` (an int and a boolean), Luxir reads comparisons as numbers, so the
expression means `2 & (1 == 0)`, which is 0. Add parentheses when in doubt.

The bitwise operators and shifts take integers; convert a double with
`to_int(x)`. A shift distance uses its low six bits, so `1 << 65` is `1 << 1`,
as in Java and Painless. `%` returns an integer for two integers and a double
otherwise; the sign follows the dividend (`-7 % 3` is `-1`), and a zero divisor
makes the value missing, as for `/`.

Every operator has a function spelling: `mod`, `bit_and`, `bit_or`, `bit_xor`,
`bit_not`, `shl`, `shr`, `ushr`.

## Field paths and variables

A field path starts with a bare column name or `col("name")`. A dot denotes a
child of that field and must be adjacent to both path names (no whitespace). DATE and GEO_POINT columns define these components:

| Field type | Components | Equivalent function |
|---|---|---|
| DATE | `year`, `month`, `day_of_month`, `day_of_week`, `day_of_year`, `week_of_year`, `quarter`, `hour`, `minute`, `second`, `millis`, `millis_of_day`, `epoch_millis` | The function with the same name: `ts.year` is `year(ts)`. |
| GEO_POINT | `lat`, `lon` | `loc.lat` is `geo_lat(loc)`; `loc.lon` is `geo_lon(loc)`. |

Each field accepts at most one component: `col("ts").day_of_week` is valid;
`loc.lat.x`, `price.lat`, and `ts.date.year` are errors. Other field types
have no components. Components map over multi-valued columns, so use
`max(places.lat)` or `min(times.year)` to obtain a scalar.

DATE components use UTC. For a fixed offset use the function spelling,
for example `hour(ts, 120)`. Components belong to field paths; they do not
apply to arbitrary expressions or let-bound names. Use `year(t)` for a
let-bound date.

Request variables use `$name` in both per-document and bucket expressions.
`score` and `_score` denote the query score.

## Math

All of these return a double unless noted. Arguments may be any numeric
expression.

| Function | Meaning |
|---|---|
| `sin(x)` `cos(x)` `tan(x)` | Trigonometry, radians. |
| `asin(x)` `acos(x)` `atan(x)` | Inverse trigonometry. `asin` and `acos` need `x` in [-1, 1]. |
| `atan2(y, x)` | The angle of the point (x, y). |
| `sinh(x)` `cosh(x)` `tanh(x)` `asinh(x)` `acosh(x)` `atanh(x)` | Hyperbolic functions. |
| `ln(x)`, `log2(x)`, `log10(x)` | Logarithms. `ln` is an alias of the natural logarithm `log`, with identical behavior. |
| `expm1(x)` | `exp(x) - 1`, accurate for small `x`. `exp`, `pow`, `sqrt`, and `log1p` are in the base language. |
| `cbrt(x)` | Cube root. |
| `hypot(a, b)` | `sqrt(a*a + b*b)` without intermediate overflow. |
| `floor(x)` `ceil(x)` `trunc(x)` | Round down, up, toward zero. They return doubles. |
| `round(x)` | Round half up, like Java's `Math.round`: `round(2.5)` is 3 and `round(-2.5)` is -2. |
| `rint(x)` | Round half to even: `rint(2.5)` is 2. |
| `signum(x)` | -1, 0, or 1. |
| `to_radians(x)` `to_degrees(x)` | Angle conversion. |
| `pi()` `e()` | The constants. |
| `clamp(x, low, high)` | `min(max(x, low), high)`. Integers stay integers. |
| `mod(a, b)` | The `%` operator. |
| `to_int(x)` | Truncates toward zero to an int64. It is an error when the value does not fit. |
| `to_double(x)` | An integer as a double. |

`round`, `ceil`, `trunc`, and `rint` return doubles, like `floor`; use `to_int`
where an integer is needed, for example before a shift.

Bucket (aggregate) expressions such as `sum(x_i) % 4` or `sin(sum(x_i))` accept
these math functions, operators, `pi()`, `e()`, `clamp`, `to_int`, and
`to_double`. Date, geo, hash, `let` functions work per document, so
inside an aggregate they belong in its input: `sum(year(when_dt))`.

## Date parts

A date value carries its unit (milliseconds or nanoseconds), and the functions
below read the calendar parts of it in UTC. Each takes one DATE value, either
unit, and returns an integer. Fractional ticks round toward earlier time
before calendar extraction. Multi-valued dates map element-wise.

| Function | Result |
|---|---|
| `year(d)` | The year, with year 0 for 1 BC and negative years before it. |
| `month(d)` | 1 to 12. |
| `day_of_month(d)` | 1 to 31. |
| `day_of_week(d)` | ISO weekday: 1 is Monday, 7 is Sunday. |
| `day_of_year(d)` | 1 to 366. |
| `week_of_year(d)` | ISO 8601 week number, 1 to 53. The first week of the year holds its first Thursday, so 31 December can be in week 1. |
| `quarter(d)` | 1 to 4. |
| `hour(d)` `minute(d)` `second(d)` | Clock parts, 0 to 23 and 0 to 59. |
| `millis(d)` | Milliseconds within the second, 0 to 999. |
| `millis_of_day(d)` | Milliseconds since midnight. |
| `epoch_millis(d)` | Milliseconds since the epoch, whatever the field's unit. |

Every function except `epoch_millis` accepts a second argument, a constant
offset from UTC in minutes between -1439 and 1439, for a civil day at a fixed
offset: `hour(published_dt, 120)` is the hour in UTC+02:00. Named time zones are
not available in expressions; for calendar buckets in a named zone use a
[date histogram](dates.md#time-zones).

```
day_of_week(published_dt) >= 6 ? 0.8 : 1
```

scores weekend documents at 0.8.

## Geo

A GEO_POINT column cannot be used as a number, but its coordinates can:

| Function | Result |
|---|---|
| `geo_lat(point)`, `point.lat` | Latitude in degrees. |
| `geo_lon(point)`, `point.lon` | Longitude in degrees. |
| `geo_distance(point, lat, lon)` | Meters from a point column to a constant place (the base language). |
| `arc_distance(lat1, lon1, lat2, lon2)` | Great-circle meters between any two places, each a number expression. |
| `haversin(lat1, lon1, lat2, lon2)` | The same distance in kilometers, as in Lucene expressions. |
| `plane_distance(lat1, lon1, lat2, lon2)` | Meters on a flat approximation. Cheaper, and close to the arc for short distances. |
| `geo_bearing(lat1, lon1, lat2, lon2)` | The initial compass bearing from the first place to the second, in degrees from north, in [0, 360). |
| `geo_in_box(point, min_lat, min_lon, max_lat, max_lon)` | 1 when the point is in the box, else 0. The box is constant, is tested exactly as the `geo_box` query tests it, and wraps the date line when `min_lon` is greater than `max_lon`. A multi-valued point is inside when any value is. |
| `geohash(lat, lon, precision)` | The standard base32 geohash of 1 to 12 characters as an int64: 5 bits per character, the first character highest. Keys of one precision sort like the strings do. |
| `geotile(lat, lon, zoom)` | The web mercator tile at zoom 0 to 29, packed like Elasticsearch: `(zoom << 58) \| (x << 29) \| y`. |

`geohash` and `geotile` take latitude and longitude as numbers, so they combine
with field components: `geohash(place.lat, place.lon, 5)`. A coordinate outside
[-90, 90] and [-180, 180] is an error.

Constant coordinates in distance and bearing functions are checked when the
expression is parsed. Box endpoints are quantized to the same inclusive encoded
intervals as the query; a box containing no representable point is empty,
including singleton +90 latitude and singleton +180 longitude.

Where each geo operation lives:

| You want | Use |
|---|---|
| Documents in a box, within a radius, or in a polygon | The `geo_box`, `geo_distance`, and `geo_polygon` [queries](geo-search.md). They use the points index, and are the way to filter. |
| Sort by distance | A sort clause with the expression `geo_distance(point, lat, lon)`. |
| Boost documents near a place | `gauss_decay(geo_distance(point, lat, lon), 0, 2000)` in a rescore. |
| Boost documents in a box | `geo_in_box(...)` in a conditional: `geo_in_box(point, ...) ? 1.5 : 1`. |
| Distance between two computed places | `arc_distance`, `haversin`, or `plane_distance`. |
| Direction | `geo_bearing`. |
| Sort or bucket by a grid cell | `geohash` or `geotile`; the cell is an integer. Hexagonal cells are written with the language itself, below. |
| A polygon test inside a score | Not available. Use a `geo_polygon` query as a clause. |

## Local variables

`let` names a value so it is computed once:

```
let(name, value, body)
```

`name` stands for `value` inside `body`, and the result is `body`. Both
`value` and `body` must be scalars (not arrays). Integers stay integers; dates
and geo points keep their nature. A geo point must be consumed by a geo
function rather than returned as the expression's numeric result. Decimal and
boolean values become numbers. A name shadows a column of the same name,
cannot shadow another binding or a reserved name, and is not visible outside
the `body`. The reserved binding names are `score`, `_score`, `_score_`,
`_docid_`, and `col`.

A used value is evaluated at most once per document (and again if its score
changes and the value depends on score). An unused value is not evaluated,
including one used only in an untaken `if` or `def` branch. A constant binding
remains a constant, so `let(z, 5, geotile(place.lat, place.lon, z))` is valid.
Expression nesting and dependency depth are bounded by the parser's nesting
budget; there is no separate limit on the number of bindings.

`let` is not available inside aggregate (bucket) expressions.

## Hashing

These functions give a document a stable pseudo-random number, for consistent
sampling and bucketing (an A/B split, a percentage rollout, a spread of a
tie). The argument is a number, hashed by its 64-bit pattern: integer-lane values (including dates)
use their integer bits; double-lane values (including fractional dates) use
their IEEE 754 bits, so `-0.0` and `0.0` differ.

| Function | Result |
|---|---|
| `xxhash64(x)`, `xxhash64(x, seed)` | The 64-bit xxHash (XXH64) of the 8 bytes of `x`, as an int64. |
| `murmur64(x)`, `murmur64(x, seed)` | MurmurHash64A of the 8 bytes of `x`, as an int64. |
| `hash_bucket(x, n)`, `hash_bucket(x, n, seed)` | An integer in [0, n), evenly spread by `x`. `n` is a positive constant. |
| `hash_combine(a, b)` | Mixes two integers, in order: `hash_combine(a, b)` differs from `hash_combine(b, a)`. |

`seed` is an integer constant and defaults to 0. Integer seeds and bucket
counts preserve all 64 bits; they do not pass through floating point. Results of `xxhash64` and
`murmur64` use all 64 bits and can be negative; take a bucket with
`hash_bucket`. Hashing a string column is not available.

```
hash_bucket(account_id_l, 100, 7) < 10 ? 1.2 : 1
```

boosts a fixed 10% of accounts.

## Porting Lucene expressions and Painless scripts

Scoring scripts need rewriting to use Luxir expressions. The following
functions and operators have direct equivalents:

| Script | Luxir |
|---|---|
| `_score` | `_score` or `score` |
| `Math.log(x)`, `Math.log10(x)`, `Math.pow(a, b)`, `Math.sqrt(x)` | `log(x)`, `log10(x)`, `pow(a, b)`, `sqrt(x)` |
| `Math.round(x)`, `Math.floor(x)`, `Math.ceil(x)`, `Math.hypot(a, b)` | `round(x)`, `floor(x)`, `ceil(x)`, `hypot(a, b)` |
| `Math.sin(x)` ... `Math.toRadians(x)` | `sin(x)` ... `to_radians(x)` |
| `(int) x`, `(long) x` | `to_int(x)` |
| Lucene `haversin(lat1, lon1, lat2, lon2)` (kilometers) | `haversin(lat1, lon1, lat2, lon2)` |
| `x & 3`, `x >> 2`, `~x`, `x ? a : b`, `a && b` | the same |

Rewrite field reads as bare column names or `col("name")`, variables as `$name`,
array reductions as function calls (`min(f)`, `sum(f)`, `count(f)`), and missing
checks as `count(f) == 0`. Replace getter methods and camelCase date parts
with the canonical functions or the field components listed above. The
`doc[...]`, `params...`, `.value`, `.empty`, `.size()`, reducer properties,
and `.date` forms are not accepted.

Differences to know about when porting:

- **Missing values.** Lucene expressions read a missing field as 0, and Painless
  throws unless the script checks `size() == 0`. Luxir carries "missing"
  through the arithmetic and gives the document no value. Write
  `def(f, 0)` to read a missing field as zero.
- **Types.** Painless has statements, `if`/`else`, loops, strings, lists, maps,
  and user functions; Luxir has expressions only. `if` becomes `?:`, a
  loop needs a built-in function or an expression without iteration. A script
  that builds a string must produce an integer key instead.
- **`/` is always floating point**, as in Lucene expressions; for integer
  division shift or truncate: `to_int(x / 4)` or `x >> 2`.
- **No `emit`, no `_source`, no stored fields, no strings.**

## Porting Painless grid scripts

A script that computes a grid cell as a string becomes an expression that
computes it as an integer. The cell then sorts and groups like any number, and
`geohash` and `geotile` are built in.

**Geohash**, 5 characters. The Painless version bisects the range with a
`while` loop and emits a string. `geohash(lat, lon, 5)` returns the same cell as
an integer:

```
geohash(location.lat, location.lon, 5)
```

**Geotile**, zoom 5. `geotile(lat, lon, 5)`, or in expression form:

```
let(lat, clamp(location.lat, -85.0511287798066, 85.0511287798066),
let(lon, location.lon, let(tiles, 1 << 5,
let(x, clamp(to_int(floor((lon + 180.0) / 360.0 * tiles)), 0, tiles - 1),
let(s, sin(to_radians(lat)),
let(y, clamp(to_int(floor((0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * pi())) * tiles)),
             0, tiles - 1),
(5 << 58) | (x << 29) | y))))))
```

The value is `(zoom << 58) | (x << 29) | y`. Take the tile back apart with
`(key >> 29) & 536870911` for `x` and `key & 536870911` for `y`.

**Hexagonal cells**, an axial grid at resolution 5 as in the original script.
The cell `rx/rz` becomes `rx << 32 | rz`:

```
let(lon, location.lon, let(lat, location.lat,
let(size, 180.0 / pow(sqrt(7), 5),
let(q, (2.0 / 3.0 * lon) / size,
let(r, (-1.0 / 3.0 * lon + sqrt(3.0) / 3.0 * lat) / size,
let(s, -q - r, let(rx, round(q), let(ry, round(s), let(rz, round(r),
let(xd, abs(rx - q), let(yd, abs(ry - s), let(zd, abs(rz - r),
let(first, xd > yd && xd > zd,
let(fx, first ? -ry - rz : rx,
let(fz, first || yd > zd ? rz : -rx - ry,
(to_int(fx) << 32) | (to_int(fz) & 4294967295))))))))))))))))
```

Use any of them as a sort clause, or as the input of a `sum` or `cardinality`
aggregate to count cells. A document with no point has no cell and sorts last.

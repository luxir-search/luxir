# The Luxir query language (`expr`)

`expr` is the string form of a query: what you type into a curl body, a
dashboard, or a filter. The structured JSON or protobuf form builds the same
tree, and anywhere a request takes a query, either form works. The syntax is
close to Lucene's but without its warts: `AND`/`OR`/`NOT` have real
precedence, special characters only act in the position where they mean
something, most structured query types are callable as functions, and
`$vars` substitute values without being parsed as syntax. The grammar is strict and reports parse errors with a byte offset. It
is not meant for raw end-user input; a search box should send its text through
[`simple_query`](quickstart.md#forgiving-end-user-search), which never fails to
parse.

Anywhere the JSON API takes a query object, a bare string is an expression:

```http
POST /collections/main/_search

{"query": "status_s:active AND year_i:>=1960", "fields": ["id"]}
```

The forms at a glance:

```
title_t:dune                          term, analyzed like the field
title_t:"dune messiah"                phrase
title_t:"dune messiah"~2              phrase with slop
status_s:=live                        exact whole value, no analysis
title_t:dune AND NOT status_s:draft   AND, OR, NOT with real precedence
+title_t:dune -tag_s:beta             search-box style: required, prohibited
title_t:(dune OR kings)               field group
year_i:[1960 TO 1970]                 range; { } excludes an endpoint
year_i:>=1965                         comparison
created_dt:>=NOW/DAY-30DAYS           date math
title_t:mess*                         prefix
author_s:Herbet~1                     fuzzy, one edit
year_i:*                              field has a value
title_t:dune^2                        boost
fuzzy(Herbet, field=author_s, max_edits=2)   any query type as a function
author_s:=$authors                    a value from vars, never parsed as syntax
```

Filters take expressions too, and a filter is often clearest as a string:

```
{"query": {...}, "filter": ["status_s:active AND year_i:[1960 TO 1970]"]}
```

The string form is shorthand for the `expr` arm:
`{"query": {"expr": {"q": "..."}}}`. Echo mode (`?explain=request`) prints
the request back in the structured form.

An expression is plain shorthand: it builds the same query the equivalent
structured JSON would. The syntax only supplies structure: which fields,
how clauses combine, and ranges. Your search words reach the field's
analyzer untouched, exactly as they would from a structured `match` query,
and a value's meaning is decided by its field type: `zip_s:02134` stays the
string "02134", leading zero included, because `zip_s` is a string field.

On a field with [variants](schema.md#field-variants), terms and phrases use
the field's `search` binding and `:=` and ranges use its `value` binding;
`field__label` and `field__self` select one representation explicitly. See
[default bindings for variants](schema.md#default-bindings-for-variants).

## Terms and phrases

```
title_t:dune                term match, analyzed like the field was
title_t:"dune messiah"      phrase (on analyzed text fields)
title_t:'dune messiah'      same thing; handy inside JSON
title_t:"dune messiah"~2    phrase allowing a positional spread of 2
tag_s:"in stock"            on an unanalyzed string field: one exact term
year_i:1982                 exact numeric match
```

There is no default search field: name one (`title_t:dune`), use
`match(dune, field=title_t)`, or use `simple_query` for search-box input.

Phrase slop measures the spread of the query-adjusted positions: for a
candidate occurrence of every phrase term, subtract that term's query
position, then take `max - min`. A match is accepted when that spread is no
greater than the slop. Slop zero is an exact phrase. Terms may reorder; an
adjacent transposition costs 2, so `"a b"~1` does not match `b a`, while
`"a b"~2` does. Position holes count naturally under the same rule.

Multi-valued text fields place a position gap of 100 between values. This
discourages accidental cross-value phrases but does not make values an
absolute boundary: a phrase can cross adjacent values at slop 100 or more.

## Exact values

`:=` is exact membership, equivalent to structured `any_of`. It uses the value
binding, so a whole-author filter can use the same bare field as text search:

```http
POST /collections/authors/_search

{
  "query": "author_name:=(\"Neal Asher\", \"Neal Stephenson\")",
  "fields": ["id"],
  "get_number": true,
  "sort": "id"
}
```

Using the [author example](documents.md#field-variants), this returns the five
books by Asher and Stephenson. The forms are:

| Form | Meaning |
|---|---|
| `author_name:="Neal Asher"` | One whole value through `author_name__s`. |
| `author_name:=("Neal Asher", "Neal Stephenson")` | Any listed value. Lists need commas, at least one value, and no trailing comma. |
| `author_name__self:=Neal` | Exact token membership on the primary TEXT representation. |
| `author_name:=$authors` | A scalar or list from `vars`; the contents are values, never query syntax. |

```http
POST /collections/authors/_search

{
  "query": {
    "expr": {
      "q": "author_name:=$authors",
      "vars": {
        "authors": ["Neal Asher", "Neal Stephenson"]
      }
    }
  },
  "fields": ["id"],
  "get_number": true,
  "sort": "id"
}
```

Single and double quotes both delimit exact values. Quote whitespace,
parentheses, and commas; the ordinary quoted-string escape rules apply.
Quotes after `:=` never turn the value into a phrase.

In an unquoted exact value, pattern and score suffix bytes stay literal:
`tag_s:=mess*` matches the value `mess*`, `tag_s:=x~1` matches `x~1`, and
`tag_s:=x^2` matches `x^2`. To decorate the query's score, delimit the value:
`tag_s:="x^2"^3` or `tag_s:=("x^2", "mess*")^2`.

STRING applies its whole-value normalizer. On TEXT, the single analyzed term
is what is looked up: `author_name__self:="Neal!"` looks up `neal` and matches
the same documents as `author_name__self:=Neal` or a match query for `Neal!`.
Each TEXT value must analyze to at most one term; an empty result matches nothing.
For multiple terms, use match, phrase, or a whole-value string variant.

## Special characters

A character is only special in the position where its meaning applies, so
most values need no escaping. Outside the exact-value forms above:

- `:` separates the field name at the first colon only.
  `url_s:https://x.com/a?b=1` and `time_s:12:30:00` parse as expected.
- `*` is a wildcard only at the end of a term. `mess*` is a prefix query;
  `a*b` is the literal term `a*b`.
- `~` after a quoted TEXT phrase sets phrase slop; after an unquoted term it
  is fuzzy only as a trailing `~` or `~N` (a whole number of edits).
  `"dune messiah"~2` is a sloppy phrase, `dune~1` is fuzzy, and `a~b` is
  literal.
- `^N` multiplies a clause's scores by `N`, and `^=N` replaces its scores
  with the constant `N`. Elsewhere `^` is a literal character.
- Quotes start a quoted value only where a value can begin - right after
  `field:`, whitespace, `(`, a `,` or `=` in arguments. Anywhere else a
  quote is an ordinary character, so `title_t:don't` is one word.
- `\` escapes the next character when you do need one: `status_s:a\:b`.
- Inside quotes the only escapes are `\"`, `\'`, and `\\`.

Whitespace and parentheses are always structural; when a value contains
them - or anything else you don't want the grammar to see - quote the whole
value. Inside quotes, nothing is special except the closing quote and the
three escapes above.

Coming from Lucene or Solr: `?` and mid-word `*` are **not** wildcards in a
term, and `/re/` is not a regex; all three are ordinary characters.
Wildcards and regular expressions are the named functions
`wildcard(...)` and `regex(...)` described [below](#wildcards-and-regular-expressions).

## AND, OR, NOT, and +/-

`AND`, `OR`, and `NOT` are recognized in uppercase. `NOT` binds tightest,
then `AND`, then `OR`, so

```
title_t:dune OR title_t:messiah AND status_s:live
```

means `dune OR (messiah AND live)`. `NOT x` on its own means "everything
except x":

```
status_s:live AND NOT tag_s:beta
NOT tag_s:beta
```

The other way to combine clauses is the search-box style: clauses separated
by spaces are optional, `+` makes one required, `-` excludes it. A sign must
touch its clause.

```
+status_s:live -tag_s:beta title_t:dune
```

Use one style per query level. Parentheses let the styles nest:

```
+(title_t:dune OR title_t:messiah) -tag_s:beta
```

## Field groups

Parentheses after `field:` apply the field to everything inside:

```
title_t:(dune OR messiah)        same as title_t:dune OR title_t:messiah
title_t:(dune mess* fuzz~1)      terms, prefixes, fuzzy, all against title_t
year_i:(>=1960 AND <1970)
title_t:(dune OR body_t:spice)   a field named inside the group overrides it
temp_i:(-5 OR 10)                on a numeric field, - is the value's sign
```

A group is boolean structure, not a bag of words. To match the words
together as one analyzed value, use `match(dune messiah, field=title_t)`.

In a numeric field's group, `-` in front of a number binds to the number:
`temp_i:(-5)` matches -5 rather than excluding 5. To exclude a value there,
use `NOT`: `temp_i:(NOT 5)`.

Logical field scopes distribute to each leaf before binding. In the
[author example](documents.md#field-variants), `author_name:(Stephenson AND >=O)`
searches `Stephenson` on the primary and ranges on `author_name__s`. It returns
no documents because `Neal Stephenson` sorts before `O`. Using
`author_name__self:(Stephenson AND >=O)` keeps both leaves on the primary and
returns `b3` and `b5`, whose tokens include `stephenson`. A field named inside
the group still overrides the enclosing scope.

## Ranges and comparisons

Any queryable field takes a range. Square brackets include the endpoint,
curly braces exclude it, and the two mix. `*` leaves an end open.

```
year_i:[1960 TO 1970]
year_i:{1960 TO 1970}
year_i:[1960 TO 1970}
year_i:[* TO 1970]
created_dt:[2020-01-01T10:30:00Z TO *]
created_dt:>=NOW/DAY-30DAYS      date math works wherever a date does
id:[user_100 TO user_200]        string/text fields range over their terms
year_i:>=1960                    also >, <=, <
```

Endpoints are converted exactly the way field values are at indexing time,
so querying a literal finds the documents indexed with it. On string, id,
and text fields the range runs over the indexed terms in plain byte order
(no collation) and uses the positional constant-scoring rule described
below. Text endpoints fold the way the field folds, like prefix and fuzzy
text; STRING bounds pass through the field's normalizer. Bare ranges use the
value binding, including comparisons inside a field group.

A date literal means the window it names: `created_dt:2024-06-25` matches
the whole day, `created_dt:2024-06` the whole month, and range endpoints
include the granule they name, so `[2024-01 TO 2024-06]` covers January
through June. Date math (`NOW-30DAYS`, `NOW/DAY`, `NOW-1MONTH/MONTH`) works
anywhere a date does, with one clock snapshot per request. Set `time_zone` on
the request to interpret dates and date math in another time zone.
[Dates and time zones](dates.md) explains the rules; the
[details below](#dates-in-expressions) cover the grammar corners.

Join comparisons with `AND` or `OR`, for example `year_i:(>=1960 AND <1970)`.

## Prefix, fuzzy, existence

```
title_t:mess*        terms starting with "mess"
title_t:dune~        fuzzy; edit distance chosen from the term length
title_t:dune~1       fuzzy, one edit (maximum 2; ~0 means exact)
year_i:*             documents with any value in the field
exists(year_i)       the same existence query in function form
*:*                  every document
```

`field:*` works on every indexed or column-stored field type and lowers to the
structured `{"exists":{"field":"field"}}` query. A bare name tests the primary;
`field__label:*` tests that variant. It matches documents that
supplied at least one accepted value: an empty string and text that analyzes to
zero tokens are present, while an empty multi-valued array is missing. Values
discarded during ingestion, such as a zero-norm cosine vector, are also missing.
Stored-only fields cannot be queried for existence.

Existence is a filter-shaped query. At the root or in an optional clause it
contributes `1`; as a required clause (`+field:*`) it contributes `0` and only
restricts matching. A boost multiplies the constant where the clause scores,
so `+field:*^3` still contributes `0`; use `^=N` (constant_score) when a
required clause should score, for example `+field:*^=2`.

The same positional rule applies to match-all, numeric and geo ranges, prefix,
wildcard, regex, and term-range queries: their default constant is `1` and a
required clause contributes `0`. Filter and prohibited clauses never score.

Prefix and fuzzy text is folded the way the field folds - `title_t:Runn*`
finds what "Runner" indexed - but never split into words. On unanalyzed
string fields a configured normalizer applies without splitting the value;
otherwise the text is used exactly as written, so `author_s:Herbet~1` finds
`Herbert` while `author_s:herbet~1` does not on a string field with no
normalizer.

Fuzzy matching currently requires the first byte to match exactly (the
default `prefix_length` is 1, which bounds the scan); `hte~1` will not find
"the". Pass `prefix_length=0` through the `fuzzy(...)` function to trade a
wider scan for first-position typos.

## Wildcards and regular expressions

Wildcard and regular-expression matching are functions, so their pattern
characters never collide with the rest of the grammar:

```
wildcard(du*, field=title_t)         * matches any bytes, ? matches one codepoint
wildcard(d?ne, field=title_t)
regex(dune|kings, field=title_t)     anchored: the whole indexed term must match
regex(mess.*, field=title_t)
```

Both match entire indexed terms. In a wildcard pattern `\` escapes the next
character. A regex supports `|`, concatenation, groups, repetition, `.`, and
character classes; `^` and `$` are literals because the match is already
anchored, so `regex(mess, ...)` does not match `messiah` but `regex(mess.*,
...)` does. On TEXT fields, literal characters fold the way the field folds
text while the pattern syntax, escapes, character classes, and ranges are
codepoint-exact and never fold; STRING fields normalize literal characters
through their normalizer; ID fields use literals verbatim. Both queries are
constant-scoring. Quote the pattern when it contains characters the argument
grammar would otherwise see, such as a comma or a parenthesis. See the
[structured reference](query-reference.md#wildcard) for the JSON arms.

## Per-clause scores

A score decoration applies to the clause immediately before it:

```
title_t:dune^2
title_t:"dune messiah"^1.5
(title_t:dune OR title_t:messiah)^3
status_s:active^=2
```

`^N` multiplies every matching score by `N`; on a required filter-shaped
clause the suppressed constant stays `0`, so use `^=N` there instead. Boosts
nest by multiplication.
`^=N` is shorthand for `constant_score(..., score=N)`: it keeps the match set
but replaces the child score. Only one score decoration is allowed on one
clause. The value must be a literal finite non-negative number; `$variables`
are not accepted as score-decoration values.

The structured arm is `{"boost":{"query":...,"boost":N}}`. JSON also
accepts a numeric `boost` sibling as input sugar:
`{"match":{"title_t":"dune"},"boost":2}`. An object-valued `boost` is the
arm itself, not the sibling sugar. Request echo and other encoding always use
the structured wrapper form.

An omitted boost means `1.0`. A boost of `0` is legal: matching documents
remain in the result set and their scores become zero. Boost has no effect in
filter or prohibited context because those clauses are built without scores.
Inside `constant_score`, a child boost is discarded; a boost outside
`constant_score` multiplies the constant.

For score shaping beyond a multiplier, `rescore` replaces each hit's score
with a value expression over the child score and column values:

```
rescore(title_t:dune, expr=score * log1p(popularity_i))
```

`score` is the child query's score, and any numeric column can appear
beside it. The expression must produce a value for every matched document;
`def(popularity_i, 0)` or an `exists()` clause in the child query makes it
total. `$name` variables inside the expression bind from the surrounding
expression's `vars`. The value-expression language is the one sorting uses;
see [Searching](searching.md#sorting).

## Functions

Most structured query types can be written as a function call. The function
name is the query's JSON name and the arguments are its JSON fields, so the
[structured query reference](query-reference.md) doubles as the function
reference:

```
match(dune messiah, field=title_t, operator=AND, min_match=2)
phrase(dune messiah, field=title_t)
phrase(dune messiah, field=title_t, slop=2)
fuzzy(smith, field=name_s, max_edits=2, prefix_length=0)
prefix(mess, field=title_t)
wildcard(du*, field=title_t)
regex(dune|kings, field=title_t)
exists(year_i)
range(field=year_i, gte=1960, lt=1970)
any_of(("Neal Asher", "Neal Stephenson"), field=author_name)
boost(title_t:dune, boost=2)
constant_score(status_s:active AND year_i:>=1960, score=1.0)
rescore(title_t:dune, expr=score * log1p(popularity_i))
boolean(required=[status_s:active], optional=[title_t:dune, title_t:messiah], min_match=1)
simple_query($user_input, fields=[title_t, body_t], operator=AND)
all()
```

`any_of(...)` has the same value binding and literal rules as `author_name:=(...)`.
`expr` is already the surrounding language and is written inline.

Arguments work like Python's: at most one positional argument, then
`name=value` pairs. The positional slot is the query's main value and takes
raw text up to the closing `,` or `)`; where the slot is itself a query
(`boost`, `constant_score`, `rescore`, or lists like `required=[...]`), it
takes a full expression instead. Values are typed by the argument: numbers,
`true`/`false`, `AND`/`OR`, `[lists]`, quoted strings; `rescore`'s `expr`
takes a value expression.

Quoting an argument protects it from the grammar but does not change what
it means: `match("foo bar")` and `match(foo bar)` search the same text. Use
`phrase(foo bar, field=title_t)` for a phrase. This differs
from term position, where `title_t:"foo bar"` is a phrase.

In `boolean(...)`, optional clauses only rank matches when a required or
filter clause is present; set `min_match` to make the optional group a real
constraint ("at least N of these"). With only optional clauses, at least
one must match.

Duplicate optional clauses are merged into a single weighted clause, and
`min_match` is adjusted to match. A `min_match` above half the clauses ("10
clauses, `min_match=9`") allows a fixed number of misses, so each merged
duplicate decrements it (never below 1), and a document matching the
duplicated clause is treated exactly as if the duplicates were kept. A
`min_match` at or below half ("10 clauses, `min_match=2`") means "match at
least that many distinct words" and stays as-is, capped at the number of
distinct clauses. Either way, a `min_match` computed from raw token counts
(for example, a percentage of pasted text) still works when the text repeats
words.

## Variables

`$name` refers to a value supplied alongside the expression:

```
{"query": {"expr": {
  "q": "simple_query($input, fields=[title_t]) AND year_i:>=$year",
  "vars": {"input": "whatever the user typed: AND) OR *", "year": 1960}
}}}
```

A variable's contents are used as a value and are not parsed as query
syntax: operators, quotes, and parentheses inside it are just text to search
for. That makes `$name` the safe way to pass user input to an expression, as
in the example above, where the operators in the input are searched for as
text. A `$` inside quotes or inside a word is an ordinary character
(`status_s:costs$5`).

## Dates in expressions

Details of the date grammar. [Dates and time zones](dates.md) explains the
model.

- Both Solr and Elasticsearch/OpenSearch date-math syntaxes are accepted:
  Solr-style word units appended directly to the anchor (`NOW-1DAY/DAY`,
  `2024-01-01T00:00:00Z+2MONTHS`),
  and Elasticsearch/OpenSearch one-letter units (`now-1d/d`,
  `2024-01-01T00:00:00Z||+2M`). The `||` separator goes between a literal
  date and its math; `now` needs no separator.
  Word units are case-insensitive (`YEARS`, `MONTHS`, `WEEKS`, `DAYS`/`DATE`,
  `HOURS`, `MINUTES`, `SECONDS`, and the millisecond aliases); `WEEK`/`WEEKS`
  is a Luxir extension of the Solr grammar, coherent with the `w`
  abbreviation and civil week rounding. One-letter units are case-sensitive:
  `y`, `M`, `w`, `d`, `h`/`H`, `m`, and `s`, so `M` means month while `m`
  means minute. Week rounding starts on Monday.
- `NOW` and `now` use one clock snapshot for the entire search request or
  update message. Commands are evaluated left to right. Prefer `||` when a
  truncated time or numeric zone offset makes the anchor boundary hard to
  read; direct suffix parsing otherwise chooses the longest valid anchor.
- Without math, a partial literal keeps its window. Once a math suffix
  begins, its anchor is the start instant of that literal, and a `/unit`
  command creates a window again, so `gte`, `gt`, `lte`, and `lt` around
  rounded date math select the edge you would expect. Math commands cannot
  contain whitespace; they can otherwise be bare field values in `expr` and
  `simple_query`.
- `time_zone` is a request-level setting. The default (`""`), `Z`, and `UTC`
  mean UTC. Fixed offsets accept `+hh`, `+hhmm`, or `+hh:mm` (and negative
  forms) through `+/-18:00`; otherwise the value is a case-sensitive IANA
  name such as `America/Denver`.
- In a zoned request, offset-less literals are local civil times, as are
  rounding and calendar additions (`YEAR`, `MONTH`, `WEEK`, `DAY`). Hour,
  minute, second, and millisecond additions are physical durations. `NOW`
  and epoch millis are instants and do not move when the frame changes. A
  literal with `Z` or its own numeric offset names that offset's instant;
  subsequent math rebases the instant into the request frame.
- At a daylight-saving or political clock change, a nonexistent local time
  is shifted forward by the size of the gap. An ambiguous local time uses
  the earlier occurrence initially; later civil operations retain the source
  occurrence when its offset is still valid. A zone can skip a whole civil
  granule (a dateline change can remove a day); the query keeps the
  gap-shifted result and returns a `date_granule_skipped` warning.
- Updates have no time-zone setting: ingest date math and offset-less ingest
  literals are UTC. Under a zoned search request the same offset-less text
  is therefore interpreted in the request zone at query time but in UTC at
  ingest. Use an explicit `Z` or numeric offset when the instant must be
  identical on both paths.

## Errors

Parse errors report the byte offset, the surrounding text, and what to do
instead:

```
expr parse error at byte 18: +/- prefixes cannot mix with AND/OR at the same
level; use NOT or parentheses (context: "status_s:live AND <HERE>+tag_s:beta")
```

## Limits

- There is no default search field.
- `?` and mid-word `*` in a term are literal characters, and `/re/` is not a
  regex; use `wildcard(...)` and `regex(...)`.
- `geo_box` and `geo_distance` have no function form; use their structured
  JSON objects. New structured query arms are not automatically callable
  until their expression behavior is declared.
- `$variables` are values only: they cannot supply a score decoration, a
  field name, or a fragment of expression syntax.
- Nesting depth is bounded per request, including expressions nested inside
  structured query nodes.

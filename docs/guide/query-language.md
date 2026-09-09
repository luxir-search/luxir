# The Luxir query language (`expr`)

`expr` is the query string aimed at developers: the thing you type into a
curl body, a dashboard, or a filter. It has a strict grammar and reports
parse errors with a byte offset. It is not meant for raw end-user input -
a search box should send its text through
[`simple_query`](quickstart.md#forgiving-end-user-search), which never fails
to parse. `expr` would rather error than guess.

Anywhere the JSON API takes a query object, a bare string is an expression:

```http
POST /collections/main/_search

{"query": "status_s:active AND year_i:>=1960", "fields": ["id"]}
```

Filters take them too:

```
{"query": {...}, "filter": ["status_s:active AND year_i:[1960 TO 1970]"]}
```

The string form is shorthand for the `expr` arm:
`{"query": {"expr": {"q": "..."}}}`. Echo mode (`?explain=request`) prints
the request back in the structured form.

An expression is plain shorthand: it builds the same query the equivalent
structured JSON would. The syntax contributes structure - which fields, how
clauses combine, ranges - and nothing else. Your search words reach the
field's analyzer untouched, exactly as they would from a structured `match`
query, and what a value means is decided by its field, not by its shape:
`zip_s:02134` stays the string "02134", leading zero and all, because
`zip_s` is a string field.

## Terms and phrases

```
title_t:dune                term match, analyzed like the field was
title_t:"dune messiah"      phrase (on analyzed text fields)
title_t:'dune messiah'      same thing; handy inside JSON
title_t:"dune messiah"~2    phrase allowing a positional spread of 2
tag_s:"in stock"            on an unanalyzed string field: one exact term
year_i:1982                 exact numeric match
```

A word with no field is a parse error. There is no default search field:
either name one (`title_t:dune`), use `match(dune, field=title_t)`, or use
`simple_query` if the text came from a search box.

Phrase slop measures the spread of the query-adjusted positions: for a
candidate occurrence of every phrase term, subtract that term's query
position, then take `max - min`. A match is accepted when that spread is no
greater than the slop. Slop zero is an exact phrase. Terms may reorder; an
adjacent transposition costs 2, so `"a b"~1` does not match `b a`, while
`"a b"~2` does. Position holes count naturally under the same rule.

Multi-valued text fields place a position gap of 100 between values. This
discourages accidental cross-value phrases but does not make values an
absolute boundary: a phrase can cross adjacent values at slop 100 or more.

## Special characters

A character is only special in the position where its meaning applies, so
most values need no escaping:

- `:` separates the field name at the first colon only.
  `url_s:https://x.com/a?b=1` and `time_s:12:30:00` parse as you'd hope.
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

Coming from Lucene or Solr: `?` and mid-word `*` are **not** wildcards here,
and `/re/` is not a regex - all three are ordinary characters, and that will
not change. Wildcard and regex queries will arrive as named functions
(`wildcard(...)`, `regex(...)`) when the engine grows the query types.

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

One level of a query uses one style or the other. `+a AND b` and
`a b AND c` are parse errors - mixing the styles is ambiguous, and parsers
that accept it disagree about what it means, so this one makes you pick.
Parentheses let the styles nest:

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

## Ranges and comparisons

Any queryable field takes a range. Square brackets include the endpoint,
curly braces exclude it, and the two mix. `*` leaves an end open.

```
year_i:[1960 TO 1970]
year_i:{1960 TO 1970}
year_i:[1960 TO 1970}
year_i:[* TO 1970]
created_dt:[2020-01-01T10:30:00Z TO *]
id:[user_100 TO user_200]        string/text fields range over their terms
year_i:>=1960                    also >, <=, <
```

On string, id, and text fields the range runs over the indexed terms in
plain byte order (no collation), and uses the positional constant-scoring
rule described below. Text
endpoints fold the way the field folds, like prefix and fuzzy text.

Endpoints are converted exactly the way field values are at indexing time,
so querying a literal finds the documents indexed with it.

A date literal means the window it names: `created_dt:2024-06-25` matches
the whole day, `created_dt:2024-06` the whole month, and range endpoints
include the granule they name - `[2024-01 TO 2024-06]` covers January
through June, `{... TO 2024-06}` excludes all of June. A full timestamp is
still a single instant.

Date fields accept both Solr and OpenSearch date math. `NOW` and `now` use one
clock snapshot for the entire search request or update message. Commands are
evaluated left-to-right;
accepted forms include `NOW-1DAY/DAY`, `2024-01-01T00:00:00Z+2MONTHS`, and
`2024-01-01T00:00:00Z||+2M`. Solr word units are
case-insensitive (`YEARS`, `MONTHS`, `WEEKS`, `DAYS`/`DATE`, `HOURS`,
`MINUTES`, `SECONDS`, and the millisecond aliases). `WEEK`/`WEEKS` is a
Luxir extension beyond Solr's own grammar, coherent with the `w` abbreviation
and civil week rounding. OpenSearch abbreviations are
case-sensitive: `y`, `M`, `w`, `d`, `h`/`H`, `m`, and `s`, so `M` means month
while `m` means minute. Week rounding starts Monday.

`SearchRequest.time_zone` sets the civil frame for every query in the request.
The default (`""`), `Z`, and `UTC` mean UTC. Fixed offsets accept `+hh`,
`+hhmm`, or `+hh:mm` (and negative forms) through `+/-18:00`; otherwise the
value is a case-sensitive IANA name such as `America/Denver`. An invalid zone
rejects the whole request, even when the request contains no date clause.

Offset-less literals are local civil times in that frame, as are rounding and
calendar additions (`YEAR`, `MONTH`, `WEEK`, and `DAY`). Hour, minute, second,
and millisecond additions are physical durations. `NOW` and epoch millis are
instants and do not move when the frame changes. A literal with `Z` or its own
numeric offset also names that offset's instant; subsequent math rebases the
instant into the request frame.

At a daylight-saving or political clock change, a nonexistent local time is
shifted forward by the size of the gap. An ambiguous local time uses the
earlier occurrence initially; later civil operations retain the source
occurrence when its offset is still valid. A zone can skip a whole civil
granule (for example, a dateline change can remove a day). The query keeps the
gap-shifted result and returns a `date_granule_skipped` warning rather than
silently hiding the substitution.

Updates have no time-zone setting: ingest date math and offset-less ingest
literals remain UTC. Consequently, under a zoned search request the same
offset-less text is interpreted in the request zone at query time but in UTC
at ingest. Use an explicit `Z` or numeric offset when the instant must be
identical on both paths.

The direct Solr suffix and OpenSearch `||` separator are both accepted. Prefer
`||` when a truncated time or numeric zone offset makes the anchor boundary
hard to read; direct suffix parsing otherwise chooses the longest valid anchor.

Without math, a partial literal retains the window described above. Once a
math suffix begins, its anchor is the start instant of that literal; a `/unit`
command creates a window. This makes `gte`, `gt`, `lte`, and `lt` around rounded
date math select the same lower/upper edges as OpenSearch. Math commands cannot
contain whitespace; they can otherwise be bare field values in `expr` and
`simple_query`.

Juxtaposed comparisons on one field are a parse error - `year_i:(>=1960
<1970)` would mean "either side", which is never what anyone wants; write
`AND` (or `OR` if you do want either).

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
structured `{"exists":{"field":"field"}}` query. It matches documents that
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
and term-range queries: their default constant is `1` and a required clause
contributes `0`. Filter and prohibited clauses never score.

Prefix and fuzzy text is folded the way the field folds - `title_t:Runn*`
finds what "Runner" indexed - but never split into words. On unanalyzed
string fields the text is used exactly as written.

Fuzzy matching currently requires the first byte to match exactly (the
default `prefix_length` is 1, which bounds the scan); `hte~1` will not find
"the". Pass `prefix_length=0` through the `fuzzy(...)` function to trade a
wider scan for first-position typos.

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
exists(year_i)
range(field=year_i, gte=1960, lt=1970)
boost(title_t:dune, boost=2)
constant_score(status_s:active AND year_i:>=1960, score=1.0)
boolean(required=[status_s:active], optional=[title_t:dune, title_t:messiah], min_match=1)
simple_query($user_input, fields=[title_t, body_t], operator=AND)
all()
```

`expr` is already the surrounding language and is written inline. `geo_box`
and `geo_distance` currently have no function form; use their structured JSON
objects. New structured query arms are not automatically callable until their
expression behavior is declared.

Arguments work like Python's: at most one positional argument, then
`name=value` pairs. The positional slot is the query's main value and takes
raw text up to the closing `,` or `)`; where the slot is itself a query
(`boost`, `constant_score`, or lists like `required=[...]`), it takes a full
expression instead. Values are typed by the argument: numbers,
`true`/`false`, `AND`/`OR`, `[lists]`, quoted strings.

Quoting an argument protects it from the grammar but does not change what
it means: `match("foo bar")` and `match(foo bar)` search the same text. If
you want a phrase, say so - `phrase(foo bar, field=title_t)`. This differs
from term position, where `title_t:"foo bar"` is a phrase.

In `boolean(...)`, optional clauses only rank matches when a required or
filter clause is present; set `min_match` to make the optional group a real
constraint ("at least N of these"). With only optional clauses, at least
one must match.

Duplicate optional clauses are merged into a single weighted clause, and
`min_match` adjusts by the intent its value expresses. A `min_match` above
half the clauses ("10 clauses, `min_match=9`") is a miss budget - you allowed
one absence - so each merged-away duplicate decrements it (never below 1),
and a document matching the duplicated clause behaves exactly as if the
duplicates were kept. A `min_match` at or below half ("10 clauses,
`min_match=2`") means "match at least that many distinct words" and stays
as-is, capped at the number of distinct clauses. Either way, `min_match`
computed from raw token counts (for example, a percentage of pasted text)
behaves sensibly when the text repeats words.

## Variables

`$name` refers to a value supplied alongside the expression:

```
{"query": {"expr": {
  "q": "simple_query($input, fields=[title_t]) AND year_i:>=$year",
  "vars": {"input": "whatever the user typed: AND) OR *", "year": 1960}
}}}
```

A variable's contents are used as a value, never read as more query syntax:
operators, quotes, and parentheses inside it are just text to search for.
That makes `$name` the safe way to hand user input to an expression - as in
the example above, where the injection attempt searches for its own
punctuation. A `$` inside quotes or inside a word is an ordinary character
(`status_s:costs$5`).

## Errors

Parse errors report the byte offset and the surrounding text:

```
expr parse error at byte 12: +/- prefixes cannot mix with AND/OR at the same
level; use NOT or parentheses (context: "status_s:live <HERE>+tag_s:beta")
```

Nesting depth is limited per request, and expressions nested inside other
query nodes count against the same limit; exceeding it is an error as well.

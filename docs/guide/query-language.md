# The Solux query language (`expr`)

`expr` is the query string aimed at developers: the thing you type into a
curl body, a dashboard, or a filter. It has a strict grammar and reports
parse errors with a byte offset. It is not meant for raw end-user input -
a search box should send its text through
[`simple_query`](quickstart.md#forgiving-end-user-search), which never fails
to parse. `expr` would rather error than guess.

Anywhere the JSON API takes a query object, a bare string is an expression:

```
POST /collections/main/query
{"query": "status_s:active AND year_i:>=1960", "fields": ["id"]}
```

Filters take them too:

```
{"query": {...}, "filter": [{"name": "live", "query": "status_s:active AND year_i:[1960 TO 1970}"}]}
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
title_w:dune                term match, analyzed like the field was
title_w:"dune messiah"      phrase (on analyzed text fields)
title_w:'dune messiah'      same thing; handy inside JSON
tag_s:"in stock"            on an unanalyzed string field: one exact term
year_i:1982                 exact numeric match
```

A word with no field is a parse error. There is no default search field:
either name one (`title_w:dune`), use `match(dune, field=title_w)`, or use
`simple_query` if the text came from a search box.

## Special characters

A character is only special in the position where its meaning applies, so
most values need no escaping:

- `:` separates the field name at the first colon only.
  `url_s:https://x.com/a?b=1` and `time_s:12:30:00` parse as you'd hope.
- `*` is a wildcard only at the end of a term. `mess*` is a prefix query;
  `a*b` is the literal term `a*b`.
- `~` is fuzzy only as a trailing `~` or `~N` (a whole number of edits).
  `dune~1` is fuzzy; `a~b` is literal.
- `^` is reserved for boost, which is not implemented yet. A trailing `^2`
  is a parse error; anywhere else it is a literal character.
- Quotes start a quoted value only where a value can begin - right after
  `field:`, whitespace, `(`, a `,` or `=` in arguments. Anywhere else a
  quote is an ordinary character, so `title_w:don't` is one word.
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
title_w:dune OR title_w:messiah AND status_s:live
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
+status_s:live -tag_s:beta title_w:dune
```

One level of a query uses one style or the other. `+a AND b` and
`a b AND c` are parse errors - mixing the styles is ambiguous, and parsers
that accept it disagree about what it means, so this one makes you pick.
Parentheses let the styles nest:

```
+(title_w:dune OR title_w:messiah) -tag_s:beta
```

## Field groups

Parentheses after `field:` apply the field to everything inside:

```
title_w:(dune OR messiah)        same as title_w:dune OR title_w:messiah
title_w:(dune mess* fuzz~1)      terms, prefixes, fuzzy, all against title_w
year_i:(>=1960 AND <1970)
title_w:(dune OR body_w:spice)   a field named inside the group overrides it
temp_i:(-5 OR 10)                on a numeric field, - is the value's sign
```

A group is boolean structure, not a bag of words. To match the words
together as one analyzed value, use `match(dune messiah, field=title_w)`.

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
plain byte order (no collation), and matches score a constant. Text
endpoints fold the way the field folds, like prefix and fuzzy text.

Endpoints are converted exactly the way field values are at indexing time,
so querying a literal finds the documents indexed with it.

A date literal means the window it names: `created_dt:2024-06-25` matches
the whole day, `created_dt:2024-06` the whole month, and range endpoints
include the granule they name - `[2024-01 TO 2024-06]` covers January
through June, `{... TO 2024-06}` excludes all of June. A full timestamp is
still a single instant.

Juxtaposed comparisons on one field are a parse error - `year_i:(>=1960
<1970)` would mean "either side", which is never what anyone wants; write
`AND` (or `OR` if you do want either).

## Prefix, fuzzy, existence

```
title_w:mess*        terms starting with "mess"
title_w:dune~        fuzzy; edit distance chosen from the term length
title_w:dune~1       fuzzy, one edit (maximum 2; ~0 means exact)
year_i:*             documents with any value in the field
*:*                  every document
```

`field:*` works on every field type.

Prefix and fuzzy text is folded the way the field folds - `title_wl:Runn*`
finds what "Runner" indexed - but never split into words. On unanalyzed
string fields the text is used exactly as written.

Fuzzy matching currently requires the first byte to match exactly (the
default `prefix_length` is 1, which bounds the scan); `hte~1` will not find
"the". Pass `prefix_length=0` through the `fuzzy(...)` function to trade a
wider scan for first-position typos.

## Functions

Every query type can be written as a function call. The function name is
the query's JSON name and the arguments are its JSON fields, so the
structured API documentation doubles as the function reference - including
for query types added after this page was written:

```
match(dune messiah, field=title_w, operator=AND, min_match=2)
phrase(dune messiah, field=title_w)
fuzzy(smith, field=name_s, max_edits=2, prefix_length=0)
prefix(mess, field=title_w)
range(field=year_i, gte=1960, lt=1970)
constant_score(status_s:active AND year_i:>=1960, score=1.0)
boolean(required=[status_s:active], optional=[title_w:dune, title_w:messiah], min_match=1)
simple_query($user_input, fields=[title_w, body_w], operator=AND)
all()
```

Arguments work like Python's: at most one positional argument, then
`name=value` pairs. The positional slot is the query's main value and takes
raw text up to the closing `,` or `)`; where the slot is itself a query
(`constant_score`, or lists like `required=[...]`), it takes a full
expression instead. Values are typed by the argument: numbers,
`true`/`false`, `AND`/`OR`, `[lists]`, quoted strings.

Quoting an argument protects it from the grammar but does not change what
it means: `match("foo bar")` and `match(foo bar)` search the same text. If
you want a phrase, say so - `phrase(foo bar, field=title_w)`. This differs
from term position, where `title_w:"foo bar"` is a phrase.

In `boolean(...)`, optional clauses only rank matches when a required or
filter clause is present; set `min_match` to make the optional group a real
constraint ("at least N of these"). With only optional clauses, at least
one must match.

## Variables

`$name` refers to a value supplied alongside the expression:

```
{"query": {"expr": {
  "q": "simple_query($input, fields=[title_w]) AND year_i:>=$year",
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

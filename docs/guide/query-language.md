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

An expression only describes the shape of a query: fields, boolean
structure, ranges. The parser never interprets the text itself - values
pass through to query building unmodified and are analyzed there, per field
type, exactly as if you had sent the structured JSON. The parser does
consult the schema to decide what kind of query a clause becomes, but never
guesses from the value: `zip_s:02134` keeps its leading zero because
`zip_s` is a string field, not because the parser looked at the digits.

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
- `~` is fuzzy only as a trailing `~` or `~N`. `dune~1` is fuzzy; `a~b` is
  literal.
- `^` is reserved for boost, which is not implemented yet. A trailing `^2`
  is a parse error; anywhere else it is a literal character.
- `\` escapes the next character when you do need one: `status_s:a\:b`.
- Inside quotes the only escapes are `\"`, `\'`, and `\\`.

Whitespace, parentheses, and quotes are always structural; quote or escape
values that contain them.

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
```

A group is boolean structure, not a bag of words. To match the words
together as one analyzed value, use `match(dune messiah, field=title_w)`.

## Ranges and comparisons

Numeric and date fields take ranges. Square brackets include the endpoint,
curly braces exclude it, and the two mix. `*` leaves an end open.

```
year_i:[1960 TO 1970]
year_i:{1960 TO 1970}
year_i:[1960 TO 1970}
year_i:[* TO 1970]
created_dt:[2020-01-01T10:30:00Z TO *]
year_i:>=1960                    also >, <=, <
```

Endpoints are converted exactly the way field values are at indexing time,
so querying a literal finds the documents indexed with it.

## Prefix, fuzzy, existence

```
title_w:mess*        terms starting with "mess"
title_w:dune~        fuzzy; edit distance chosen from the term length
title_w:dune~1       fuzzy, one edit (maximum 2; ~0 means exact)
year_i:*             documents with any value in the field
*:*                  every document
```

`field:*` works on every field type.

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

## Variables

`$name` refers to a value supplied alongside the expression:

```
{"query": {"expr": {
  "q": "simple_query($input, fields=[title_w]) AND year_i:>=$year",
  "vars": {"input": "whatever the user typed: AND) OR *", "year": 1960}
}}}
```

A variable is substituted as a value and never re-parsed as syntax, which
makes it the right way to feed user input into an expression: nothing the
text contains can change the query's structure. A `$` inside quotes or
inside a token is just a character (`status_s:costs$5`).

## Errors

Parse errors report the byte offset and the surrounding text:

```
expr parse error at byte 12: +/- prefixes cannot mix with AND/OR at the same
level; use NOT or parentheses (context: "status_s:live <HERE>+tag_s:beta")
```

Nesting depth is limited per request, and expressions nested inside other
query nodes count against the same limit; exceeding it is an error as well.

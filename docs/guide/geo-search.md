# Geo search

A location is a typed `geo_point`, and a geo query can go anywhere another
query can: as the main query, as a non-scoring filter on text or vector
retrieval, or as one boolean clause among several. "Coffee within five
kilometers" is a text query with a distance filter beside it, evaluated as
one query with no post-filter stage.

## Define a point field

A field whose name ends in `_p` (single point) or `_ps` (several points)
uses a default template with the points index on, so `location_p` needs no
definition. Any other name needs an explicit schema definition:

```http
POST /collections/places/_schema

{
  "fields": {
    "location": {"type":"geo_point","index":"range"}
  }
}
```

`column` defaults to `true`. `index: "range"` builds a two-dimensional points
index for boxes and circles. If it is omitted, the same queries remain correct
and scan the point column instead. The indexed and scan paths return the
same results; only the cost differs.

For a document with several locations, set `multi: true`:

```json
{"type":"geo_point","index":"range","multi":true}
```

## Point formats

The canonical point value is `[lon, lat]`, the same coordinate order used by
GeoJSON:

```http
POST /collections/places/_update

{
  "docs": [
    {"id":"nyc","name_s":"New York","location":[-74.0060,40.7128]},
    {"id":"london","name_s":"London","location":[-0.1278,51.5074]}
  ],
  "commit": {}
}
```

Longitude must be within `[-180,180]` and latitude within `[-90,90]`. Luxir
quantizes each coordinate into a 32-bit grid, roughly centimeter resolution.

The same point can be written in any of these forms, over HTTP JSON and gRPC
alike:

| Form | Example | Notes |
|---|---|---|
| Array | `[-74.0060, 40.7128]` | `[lon, lat]` |
| Object | `{"lat": 40.7128, "lon": -74.0060}` | values may be numeric strings |
| GeoJSON | `{"type": "Point", "coordinates": [-74.0060, 40.7128]}` | `[lon, lat]` |
| String | `"40.7128,-74.0060"` | latitude first, as in Elasticsearch |
| WKT | `"POINT (-74.0060 40.7128)"` | longitude first; case-insensitive |
| Geohash | `"dr5regw3p"` | 1 to 12 characters; decoded to the centre of the cell |

Keep the order straight: arrays, GeoJSON and WKT put longitude first, while
the `"lat,lon"` string puts latitude first.

A multi-valued field takes an array of points, in any mix of forms:

```json
{"id":"tour","location":[[-74.0060,40.7128],"51.5074,-0.1278","u4pruydqqvj"]}
```

An array whose elements are all numbers is always one point, so
`[-74.0060, 40.7128]` sent to a multi-valued field is one point, not two.
Any other array lists points, one per element, and `null` elements are
skipped. A single-valued field also accepts a one-element array of points,
and rejects a document that gives it more than one.

## Malformed points and altitude

By default a value that is not a valid point, such as an unreadable string, a
coordinate out of range, or an array of the wrong length, fails the whole
document with an error naming the field. Two field properties relax this:

```json
{"type":"geo_point","index":"range","ignore_malformed":true,"ignore_z_value":true}
```

- `ignore_malformed: true` skips a malformed point and indexes the rest of the
  document. In a multi-valued field only the bad points are skipped. Skipped
  values are not reported in the update response.
- A third coordinate (`[lon, lat, z]`, `"lat,lon,z"`, `POINT Z (lon lat z)`)
  is an altitude. It is rejected unless `ignore_z_value: true`, which accepts
  the point and drops the altitude. Altitude is never indexed.

Both default to `false`, so a typo or a swapped coordinate order is reported
rather than dropped. Templates and `parent` inheritance carry them like other
properties. They belong to the logical field, like `multi`: a
[variant](schema.md#field-variants) of a geo field follows the primary's
settings and cannot set its own.

## Bounding boxes

`geo_box` uses named scalar bounds so latitude and longitude cannot be confused:

```http
POST /collections/places/_search

{
  "query": {
    "geo_box": {
      "field": "location",
      "min_lat": 40.0,
      "max_lat": 42.0,
      "min_lon": -75.0,
      "max_lon": -72.0
    }
  },
  "fields": ["id","name_s"],
  "count_limit": -1
}
```

All edges are inclusive after the same coordinate quantization used at ingest.
`min_lat` must not exceed `max_lat`. If `min_lon` is greater than `max_lon`,
the box intentionally crosses the international date line:

```json
{
  "geo_box": {
    "field":"location",
    "min_lat":-20,
    "max_lat":20,
    "min_lon":170,
    "max_lon":-170
  }
}
```

That matches the band from 170 degrees east through 180/-180 to 170 degrees
west.

## Distance queries

`geo_distance` matches points within an inclusive radius in meters. Query
centers use separate `lat` and `lon` keys:

```http
POST /collections/places/_search

{
  "query": {
    "geo_distance": {
      "field":"location",
      "lat":40.7128,
      "lon":-74.0060,
      "radius_meters":50000
    }
  },
  "fields":["id","name_s"],
  "count_limit":-1
}
```

Distances use a great-circle calculation over the mean Earth radius. A radius
must be finite and non-negative. For a multi-valued field, a document matches
when any point is inside the circle, and the document is returned once.

## Nearest documents

`geo_nearest` matches the `k` documents closest to a point, nearest first:

```http
POST /collections/places/_search

{
  "query": {
    "geo_nearest": {
      "field":"location",
      "lat":40.7128,
      "lon":-74.0060,
      "k":5
    }
  },
  "fields":["id","name_s"]
}
```

Distances are the same great-circle distances `geo_distance` uses. A
multi-valued document counts once, at its nearest point, and documents at the
same distance keep index order. `max_distance_meters` (inclusive) stops the
search at a radius, so fewer than `k` documents may match.

Filters, deletes, and an enclosing facet bucket narrow the candidates before
the `k` nearest are chosen, as they do for `knn`: with a filter,
`geo_nearest` returns the `k` nearest documents that pass it. With a points
index the search walks the index outward from the center and stops once no
unvisited cell can hold a nearer point, so its cost follows `k` rather than
the collection size. A filter that admits only a few documents is checked
document by document instead, and a field without a points index is scanned.

Hits score `1 / (1 + meters)`, so the nearest ranks first. The score is a
32-bit float, so documents within about one part in ten million of each
other's distance (a meter at 10,000 km) score alike and fall back to index
order. To order by the exact distance and return it, sort by it and request
`_sort_` (next section); with only `k` hits the sort is cheap.

## Sort by distance

`geo_distance(field, lat, lon)` is the distance in meters from a point to a
document's location. As a sort clause it orders any query's matches by
distance, and `_sort_` in `fields` returns each hit's sort values:

```http
POST /collections/places/_search

{
  "query": {"match":{"description_t":"coffee"}},
  "sort": ["geo_distance(location, 40.0, -75.0)"],
  "fields": ["id","name_s","_sort_"]
}
```

```json
{
  "docs": [
    {"id": "nyc", "name_s": "New York", "_sort_": [115653.49817598039]}
  ]
}
```

The sort is ascending unless `desc` is given, and documents without a point
sort last in both directions. For a multi-valued field, choose the point:
`min(geo_distance(...))` sorts by the nearest and `max(geo_distance(...))` by
the farthest. `lat` and `lon` must be numbers or `$variables`.

An ascending sort prunes as it goes. Once it holds a full page, a document
whose points all lie outside the bounding box of the circle at the current
last distance is rejected without computing a distance. With a points index,
the circle is looked up in the index and the query skips straight to the
documents inside it; the lookup repeats each time the last distance halves.
Inside a larger value expression (`geo_distance(...) / 1000`),
`geo_distance` is evaluated per document without this pruning.

## Score by proximity

`distance_feature` matches every document with a point and scores it by how
near the point is to an origin, as Elasticsearch's query of the same name
does for geo fields:

```text
score = boost * pivot_meters / (pivot_meters + meters)
```

The score is `boost` at the origin, half of it at `pivot_meters`, and falls
toward zero beyond. A multi-valued document scores by its nearest point. Its
usual place is an optional clause beside a text query, so that relevance
decides first and proximity breaks the near-ties:

```http
POST /collections/places/_search

{
  "query": {
    "boolean": {
      "required": [{"match":{"description_t":"coffee"}}],
      "optional": [
        {"distance_feature":{"field":"location","lat":40.7128,"lon":-74.0060,"pivot_meters":1000}}
      ]
    }
  },
  "fields": ["id","name_s"],
  "get_scores": true
}
```

Wrap it in `boost` to weigh it against the text score. It does not narrow the
match set: put a `geo_distance` filter beside it for that.

Top-k collection prunes with it. Once the k-th best score reaches a floor
`theta`, a document can only compete while its proximity score reaches the
floor minus the most the other clauses can add, that is, within
`pivot * (boost / floor - 1)` meters. The query then reads the documents
inside that circle from the points index and skips the rest, and reads them
again each time the circle radius halves. This works as the main query and as
an optional clause beside a required or filter clause, and as one of several
optional clauses; nested inside another boolean, under `dis_max`, or with
`min_match` it scores correctly without skipping. A field without a points
index scores every document.

## Combine place with relevance

`geo_box` and `geo_distance` are constant-scoring when used as queries, and
every geo query is non-scoring when used as a filter. The usual pattern is a text query with a geo filter, so text relevance is
unaffected:

```http
POST /collections/places/_search

{
  "query": {"match":{"description_t":"coffee"}},
  "filter": [
    {
      "geo_distance": {
        "field":"location",
        "lat":40.7128,
        "lon":-74.0060,
        "radius_meters":5000
      }
    }
  ],
  "fields":["id","name_s","description_t"]
}
```

```json
{
  "docs": [
    {
      "id": "nyc",
      "name_s": "New York",
      "description_t": "coffee and bagels"
    }
  ]
}
```

The geo constraint participates in the same prepared execution as the lexical
query. A selective surrounding domain can drive point verification; a broader
domain can be driven by the points index.

## Current limits

- Polygon queries are not implemented.
- Geo columns are not yet decoded by document field projection, so requesting
  the geo field itself does not return `[lon,lat]`; store coordinates in
  separate numeric fields as well when they must be displayed.

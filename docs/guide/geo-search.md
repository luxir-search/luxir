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

Geo fields reject `stored: true`: points are kept in the field's column for
queries and sorting, not as stored values.

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
The column stores each point as the Morton (Z-order) interleave of the two
grid coordinates, so nearby points share high bits: a block of clustered
points compresses well, and each block's value range bounds a 2-D cell that
lets column scans skip blocks that cannot match.

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
the `"lat,lon"` string puts latitude first. A string with no comma that is
not WKT is read as a geohash. For example, a coordinate sent as the bare
string `"40"` becomes a coarse geohash cell, not a number.

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
  is an altitude. By default `ignore_z_value: true` accepts the point and
  drops the altitude. Set `ignore_z_value: false` to reject a third coordinate.
  Altitude is never indexed.

`ignore_malformed` defaults to `false`; `ignore_z_value` defaults to `true`.
An in-range longitude/latitude swap cannot be detected. Rejected points name
the recognized form and interpreted `lon` and `lat` when both can be read.
Templates and `parent` inheritance carry these options like other
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

## Polygon queries

`geo_polygon` matches points inside a polygon. Vertices use named `lat` and
`lon` keys; the outer ring needs at least three distinct vertices and may
repeat its first vertex at the end:

```http
POST /collections/places/_search

{
  "query": {
    "geo_polygon": {
      "field":"location",
      "points":[
        {"lat":40.0,"lon":-75.0},
        {"lat":42.0,"lon":-75.0},
        {"lat":42.0,"lon":-72.0},
        {"lat":40.0,"lon":-72.0}
      ],
      "holes":[
        {"points":[{"lat":40.5,"lon":-74.5},{"lat":41.0,"lon":-74.5},{"lat":41.0,"lon":-74.0}]}
      ]
    }
  },
  "fields":["id","name_s"]
}
```

A point matches when it is inside the outer ring and inside no hole, each by
the even-odd rule. Edges are straight lines in latitude/longitude, as in
Lucene's point polygon queries, and a polygon cannot cross the date line;
split such a polygon into two `geo_polygon` clauses under a boolean `should`.
A point exactly on an edge may match either way. With `index: "range"` the
points index prunes whole cells by the polygon's bounding box and edges, and
checks each point only in cells an edge crosses. Each cell test walks every
edge, so very large polygons cost proportionally more.

## Combine place with relevance

Geo queries are constant-scoring when used as queries and non-scoring when
used as filters. The usual pattern is a text query with a geo filter, so text relevance is
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

## Return points

Requesting a geo field in `fields` returns its points in the ingest order,
`[lon, lat]`:

```json
{"id":"nyc","location":[-74.006,40.7128]}
```

The values are the quantized grid points, so they can differ from the input
in roughly the eighth decimal place. A multi-valued field returns its points
as one flat array of pairs, `[lon1, lat1, lon2, lat2, ...]`. On the gRPC
API, a geo field is a `multi_d` column with one such array per document.

## Current limits

- Geo-distance sorting is not implemented.
- The query does not emit the computed distance as a result field.
- Polygons cannot cross the date line.

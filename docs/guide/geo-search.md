# Geo search

A location is a typed `geo_point`, and a geo query can go anywhere another
query can: as the main query, as a non-scoring filter on text or vector
retrieval, or as one boolean clause among several. "Coffee within five
kilometers" is a text query with a distance filter beside it, evaluated as
one query with no post-filter stage.

## Define a point field

Geo fields need an explicit schema definition; the default suffix templates
do not include one:

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

## Index points in longitude, latitude order

A point value is `[lon, lat]`, the same coordinate order used by GeoJSON:

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

A multi-valued field takes an array of point arrays:

```json
{"id":"tour","location":[[-74.0060,40.7128],[-0.1278,51.5074]]}
```

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
  "get_number": true
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
  "get_number":true
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

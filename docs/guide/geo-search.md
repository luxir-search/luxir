# Geo search

Solux represents a location as a typed `geo_point` and answers bounding-box
and radius queries inside the normal query tree. A geo query can be the main
query, a non-scoring filter on text or vector retrieval, or a boolean clause.
There is no post-filter stage with a different idea of the result set.

## Define a point field

Geo fields need an explicit schema definition; the default suffix templates
do not include one:

```bash
curl http://localhost:9400/collections/places/_schema -d '{
  "fields": {
    "location": {"type":"geo_point","index":"range"}
  }
}'
```

`column` defaults to `true`. `index: "range"` builds a two-dimensional points
index for boxes and circles. If it is omitted, the same queries remain correct
and scan the point column instead. The indexed and scan paths have the same
contract, so the choice is about cost rather than behavior.

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

Longitude must be within `[-180,180]` and latitude within `[-90,90]`. Solux
quantizes each coordinate into a 32-bit grid, roughly centimeter resolution.
Invalid shapes, non-finite numbers, and out-of-range coordinates are
per-document update errors.

A multi-valued field takes an array of point arrays:

```json
{"id":"tour","location":[[-74.0060,40.7128],[-0.1278,51.5074]]}
```

## Bounding boxes

`geo_box` uses named scalar bounds so latitude and longitude cannot be confused:

```http
POST /collections/places/_query

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
west, rather than rejecting the apparent reversal.

## Distance queries

`geo_distance` matches points within an inclusive radius in meters. Query
centers use separate `lat` and `lon` keys:

```http
POST /collections/places/_query

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

## Combine place with relevance

Geo queries are constant-scoring when used as queries and non-scoring when
used as filters. The common search shape keeps text relevance intact:

```json
{
  "query": {"match":{"description_w":"coffee"}},
  "filter": [
    {
      "name": "nearby",
      "query": {
        "geo_distance": {
          "field":"location",
          "lat":40.7128,
          "lon":-74.0060,
          "radius_meters":5000
        }
      }
    }
  ],
  "fields":["id","name_s","description_w"]
}
```

The geo constraint participates in the same prepared execution as the lexical
query. A selective surrounding domain can drive point verification; a broader
domain can be driven by the points index.

## Current limits

- Polygon queries and geo-distance sorting are not implemented.
- The query does not emit the computed distance as a result field.
- Geo columns are not yet decoded by document field projection, so requesting
  the geo field itself does not return `[lon,lat]`; store coordinates in
  separate numeric fields as well when they must be displayed.

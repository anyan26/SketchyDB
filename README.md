# SketchyDB

```
Fast answers. Probably.
```
SketchyDB is a database that has a radical philosophy:

Being exactly correct is expensive.

Most databases spend enormous amounts of time and memory trying to answer questions perfectly.

SketchyDB asks:
```
What if we are okay with being 99% correct...and much much faster?
```

Instead of computing everything exactly, SketchyDB uses probabilistic data structures and randomized algorithms to provide answers with explicit confidence guarantees.

## Pending Features
### Heavy Hitters 
Find the most frequent items in massive dataset without counting everything.

### Approximate Cardinality
Count billions of unique values using only kilobytes of memory.

### Sketch-Accelerated Queries

### Confidence-Driven Query Planning
Using specify the theoretical lower-bound they are willing to accept.

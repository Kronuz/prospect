# Changelog

## Unreleased

- `fielddict::reader::check_fields` scans the field pool through a reader it builds and drops
  (through the same checksum verifier as `open_at`), not through `pool()`, which caches a pool
  reader for the life of the open dictionary. An index that is opened and checked but never
  queried keeps no pool reader: 416 B less heap per segment. A query that needs the pool still
  caches it, and builds it a second time after a check (about 1.6 us more per segment on that first
  query); refusals and results are unchanged (`docs/perf/fielddict-pool-reader.md`).

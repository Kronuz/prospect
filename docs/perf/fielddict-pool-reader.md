# The field check keeps no pool reader

`fielddict::reader::check_fields` scans the field pool through a `termdict::reader` it builds and
drops (read through `open_at`'s checksum verifier), instead of `pool()`, which caches one for the
life of the open dictionary. An index that is opened and checked but never queried keeps no pool
reader. A query that needs the pool still caches it through `pool()`.

## Result

Per segment, one dictionary with 4 or 10 fields (one own, the rest pooled, 6 terms each), 1,000
segments opened one after another on one thread, medians over 9 interleaved runs of the harness
(`test/fielddict_pool_heap_ab.sh`, strict job 261124-160830-97749f; base prospect:44303965ec66,
fix prospect:c4e3e64, A/A a second build of base; macOS arm64, Release, `malloc_size` for the heap,
beside 5 light holds).

| fields | arm | heap B/segment base | heap fix | open ns base | open ns fix | A/A (base copy) |
|---|---|---|---|---|---|---|
| 4 | open (reader only) | 224 | 224 | 205 | 204 | 204 |
| 4 | open + check | 640 | **224** | 3,636 | **3,165** (-13%) | 3,654 |
| 4 | open + check + first pooled query | 640 | 640 | 3,956 | **5,528** (+40%) | 3,864 |
| 10 | open | 224 | 224 | 207 | 203 | 205 |
| 10 | open + check | 640 | **224** | 5,411 | **4,899** (-9%) | 5,397 |
| 10 | open + check + first pooled query | 640 | 640 | 5,779 | **7,481** (+29%) | 5,615 |

- Heap: **416 B less per segment** after the check (65% of the dictionary's 640 B), the same at 4
  and 10 fields: it is one pool reader, whatever the pool holds. Per index that is 1.7 KB at 4
  segments and 4.2 KB at 10 (the same 1.6 and 4.0 KB that strata's eager-open harness measured on
  whole indexes, about 7% of the heap per open index).
- Open time: the reader alone is unchanged. The check is 470 to 510 ns per segment faster (no cache
  node, mutex or `make_unique`; the A/A band is under 1%).
- The trade: the first query that needs the pool, after a check, builds the pool reader a second
  time: **+1.57 us per segment at 4 fields and +1.70 us at 10** (A/A band
  2 to 3%), about 1.6 ms on the first query of a 1,000-segment index. An index checked and queried
  ends with the same 640 B and about 1.6 us more work per segment than before; an index checked and
  never queried keeps 416 B less and does 0.5 us less work.

## Where the second build goes

A light profile of the pool reader's construction on the same dictionaries (not a gate; one run,
200,000 constructions): `termdict::reader` 942 ns (4 fields) and 965 ns (10), of which the FST
validation (`fst::map(bytes)`, "one pass over every node") is 957 and 986 ns, the alphabet parse
23 and 42 ns, and `fst::map(bytes, fst::trusted)` 0 ns. So the repeat is the FST validation of the
same immutable index bytes; it grows with the pool's node count (these pools hold 24 and 54 terms),
so a larger pool repeats more. The block checksums are not repeated by it: `verify_in` runs when a
block is opened, not when the reader is built.

It cannot be removed inside `fielddict.hh`: `termdict::reader` always validates its index in its
constructor and offers no trusted constructor, and its `fst::map` is private. Recording "the
check validated the pool's index" in a spare bit of `fielddict::reader` is cheap (no per-segment
memory) but needs `termdict::reader(blocks, index, trusted)` forwarding `fst::trusted`, a change in
termdict (shared, pinned by strata). With it, `pool()`'s lazy build would skip validation after a
check and the first-query row would equal the base's; the refusals stay identical because the check
has already refused a malformed index.

## Reproduce

`test/fielddict_pool_heap_ab.sh build DIR BASE` (light) then `run DIR 9 1000` (strict). The
harness `test/fielddict_pool_heap.cc` measures the dictionary level, not a whole index; the
whole-index figure is strata's `bench/eager_open` (docs/perf/eager-open.md there). Test:
`fielddict_test` counts live heap blocks across `check_fields` (unchanged), a pooled lookup (caches
once) and a second check; its mutant, `check_fields` scanning through `pool()` again, fails the heap
check.

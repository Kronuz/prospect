# Release rows

The correctness rows of Phases 3 to 5 that the first Xapian-free Xapiand release waits on (D39),
run at the composition the release builds. Phase 6's latency rows are 7c's (`x7-gate`). The phase
gates keep these rows and re-run them at their own commits after the release.

## Pins

Xapiand master pins these in `CMakeLists.txt` (last changed at `Xapiand:34b610bf5`, unchanged at
`Xapiand:90db0129a`):

| Library | Pin | Note |
|---|---|---|
| prospect | `prospect:850d4e8` | |
| strata | `strata:5d742b2` | Xapiand declares strata first, so its pin wins over prospect's own (`strata:9d52824`) |
| compressors | `compressors:5566260` | Xapiand's override of strata's `5c54128` |
| hashes | `hashes:03ab2d6` | override of `e3bf5b6` |
| threadpool | `threadpool:63e03a5` | override of `eb4751a` |

unicode, snowball, analysis and task equal prospect's pins; docset and bitpack are prospect's.

Every row ran at `prospect:2cc69d4` (quest branch `quest/xapiand/release-rows`) through
`tools/release_rows/rows.sh`. Each job checks out the five pins as refs, asserts their commits,
and asserts that the branch's `include/` equals `prospect:850d4e8`'s, so the engine under test is
the pin's and only the harness and tests are new. The libraries are substituted with
`FETCHCONTENT_SOURCE_DIR_*`. Host: macOS 15.6 x86_64; AppleClang 17.0.0 for the rows, Homebrew
clang 23.1.2 for the fuzz targets.

The pin's own suite: all 51 of `prospect:850d4e8`'s tests pass at the release's strata (job
261111-201230-8ef3e1).

## Results

| Row | Result | Jobs |
|---|---|---|
| A1 merge parity and verify | pass | 261111-201053-f77c3a, 261111-201056-33db85 |
| A2 operator parity | pass | 261111-201050-c7d247, 261111-201053-f77c3a, 261111-201230-8ef3e1 |
| A3 one shard against many | **miss** at the release's level; prospect's collectors pass | 261111-201050-c7d247 (prospect's evidence only) |
| A4 stemmer vocabularies | **miss**: six names | 261111-201059-749f46 |
| A5 fuzzing and versions | **miss** as one CPU-hour; zero findings; versions pass | 261111-201102-4af80d, 24 fuzz jobs, 261111-201050-c7d247 |
| A6 ASan and UBSan | pass | 261111-223819-03f694, CI run 43153036741 |

### A1: merging and deletes

`parity` (`tools/parity.cc`) indexes 10,000 documents of the corpus and runs the oracle's 419
ranked queries (`strata-oracle` `rank.jsonl`, Xapian 2.1.0) on each index. Every pass also checks
that the pruned top 10 equals the counted one, and that no hit is a deleted extra or repeats.

| Index | Counts equal | Top 10 equal | Pruned = counted | No deleted extra or repeat | Worst relative error |
|---|---:|---:|---:|---:|---:|
| 1 segment | 419/419 | 419/419 | 419/419 | 419/419 | 3.87e-16 |
| 4 segments | 419/419 | 419/419 | 419/419 | 419/419 | 3.87e-16 |
| 16 segments | 419/419 | 419/419 | 419/419 | 419/419 | 3.87e-16 |
| 4 + an all-deleted segment (`PARITY_DELETES=7`) | 419/419 | 157/419 | 419/419 | 419/419 | 0.0536 |
| the same, merged | 419/419 | 419/419 | 419/419 | 419/419 | 3.87e-16 |
| 16 + an all-deleted segment | 419/419 | 157/419 | 419/419 | 419/419 | 0.0536 |
| the same, merged | 419/419 | 419/419 | 419/419 | 419/419 | 3.87e-16 |

With deletes, the counts are exact and the weights drift. The collection statistics still
count deleted documents until a merge drops them, as Xapian's and Lucene's do, so the row
requires exact counts before the merge and exact results after it.

`prospect_engine verify` compares every query's top 10 with a brute-force evaluation over the
same index. The index holds 1,000,000 documents indexed through a 64 MB buffer, with every
11th document then deleted (90,910 deletes), in 8 segments. It is then force-merged into one
reordered segment (151.8 s).

| Pass | `queries.txt` | `terms-stratified.txt` |
|---|---|---|
| 8 segments, with deletes | 962 queries, 0 mismatches | 200 queries, 0 mismatches |
| merged, 1 segment | 962 queries, 0 mismatches | 200 queries, 0 mismatches |

This row runs at 1M documents on the host. Phase 3 kept the 5M row on the gate VM; this row
covers correctness across segments and merges, not scale.

### A2: operators

These are the operators Xapiand's query DSL serves (`src/query_dsl.cc`), with the vectors each
was compared on. `operators_test` compares each query's match count and its top 50 (docid,
weight), counted and pruned, with Xapian's recorded `ops` vectors. `parity` compares the
count and the top 10 on `rank.jsonl` at 1, 4 and 16 segments, and after each merge with deletes
(the exact passes; before those merges, 157 of 419 top 10s match, as A1 says). `shards_test` compares 3,696 rows
of the sharded vectors (`shards-s*.jsonl`, 6 files).

| Operator | `operators_test` | `parity` (each exact pass) |
|---|---:|---:|
| AND | 100 | 100/100 |
| OR | 78 | 144/144 |
| AND_NOT | | 19/19 |
| XOR | 110 | |
| AND_MAYBE | 14 | 40/40 |
| FILTER | 24 | |
| SCALE_WEIGHT | 122 | |
| NEAR | | 60/60 |
| PHRASE | | 100/100 |
| SYNONYM | 74 (12 not compared) | |
| MAX | 64 | |
| single term | | 1/1 |

Every compared row equals the oracle's. Rows not compared:

- SYNONYM over a non-term child (12 rows): the vectors record these rows but do not say how
  Xapian weights them, so `operators_test` counts them without checking them.
- WILDCARD and EDIT_DISTANCE: Xapiand expands each into OR, SYNONYM or MAX over the matching
  terms in its own code. The operators they expand into are covered above. The expansion
  itself is Xapiand's and needs a Xapiand test.
- Value and geo ranges (`_range`): Xapiand serves them as posting sources in its own
  `src/multivalue/range.cc` and `geospatialrange.cc`; they need a Xapiand test.
- `match_all` (behind `_not`, `match_all` and `match_none`): not in the vectors; it needs a
  Xapiand test.
- ELITE_SET is no longer served by Xapiand master. Its 83 rows are still compared and equal
  (4 tied at the cutoff and skipped, 20 over composite children recorded only).
- n-ary SCALE_WEIGHT: Xapian refuses every recorded row, and `operators_test` checks that
  prospect refuses a negative factor the same way.

### A3: aggregations over one shard and over many (miss)

`aggregate_test`'s indexed section builds 60 rounds of random multi-valued documents with
deletes, each round in 1 shard and in 2 to 7 shards over random segments. The shards'
exhaustive visitor feeds the matches into one partial, or into one partial per shard merged in
any order. Both must render byte for byte as one partial over the live matches, which is also
the one-shard result. The request nests every collector kind prospect has: count, sum, avg,
min, max, stats, extended_stats, variance, std_deviation, median, mode, values, terms,
histogram, range and filter. Result: 240 comparisons, 12,006 matches collected, 1,721
documents deleted, all equal. `shards_test`'s visitor row saw each of 145,089 live matches
exactly once. The in-memory section compares every split into 1 to 64 partials with brute
force.

Scope: this proves prospect's collectors, which the release does not use. Xapiand master serves
aggregations from its own `src/aggregations` (long double sums, results in shard order) and
never includes `prospect/aggregate.hh`. Decision (the lead, on review F1): A3 is a miss at the
release's level. prospect's 16-kind test stays as prospect's evidence; the release's row is hunt
row 91 (a Xapiand shard-parity test), and A3 is re-run when it lands. prospect's terms collector has no `_shard_size` cutoff: its partials are exact and merge
exactly. Counts past `_shard_size` are approximate by design (behavior-changes audit #25), and
that approximation lives in Xapiand's layer, so this row does not touch it.

### A4: stemmers (miss)

For every `stem_language` Xapiand's schema accepts (`src/database/schema.cc`,
`_get_stem_language`), Xapian 2.1.0's stems (the oracle's `stem`) were compared with snowball's
(`tools/release_rows/stem_parity.cc`). The vocabularies are snowball-data's word lists.

Of the 45 names, 39 stem equal to Xapian word for word and six differ:

| Name | Xapiand maps it to | Xapian 2.1.0 | snowball | Vocabulary | Sample (Xapian) |
|---|---|---|---|---|---|
| `earlyenglish` | `en` (not a language name) | stems | refuses | earlymodernenglish, 36,534 words | `'t` -> `'t`, `0d` -> `0d`, `10d` -> `10d` |
| `german2` | `de` (not a language name) | stems | refuses | german, 35,053 words | `aalglatten` -> `aalglatt` |
| `nb` | `no` (not a language name) | stems | refuses | norwegian, 20,913 words | `11'er` -> `11`, `aabakken` -> `aabakk` |
| `nn` | `no` (not a language name) | stems | refuses | norwegian, 20,913 words | `11'er` -> `11`, `aabakken` -> `aabakk` |
| `english_lovins` | `en` (not a language name) | refuses (`InvalidArgumentError`) | refuses | lovins, 42,603 words | |
| `english_porter` | `en` (not a language name) | refuses (`InvalidArgumentError`) | refuses | porter, 42,603 words | |

On Xapiand master, the schema accepts all six names, and the stemmer is built from the name as
given (`src/engine/text.cc` `stemmer_for`, `src/multivalue/generate_terms.cc` `getStemmer`).
So a field using any of them is refused at analysis with "Language code NAME unknown".
For `earlyenglish`, `german2`, `nb` and `nn`, that is a behavior change from Xapian, which
stems them. `english_lovins` and `english_porter` fail under Xapian 2.1.0 too. Under both
engines, `lovins` and `porter` stem equal (42,603 of 42,603 words each).

Matching names (words compared, all equal): armenian and hy 101,780; basque and eu 21,386;
catalan and ca 48,897; danish and da 23,868; dutch, nl and kraaij_pohlmann 45,670; english
and en 42,649; lovins 42,603; porter 42,603; finnish and fi 84,399; french and fr 21,653;
german and de 35,053; hungarian and hu 29,881; italian and it 37,426; norwegian and no 20,913;
portuguese and pt 32,016; romanian and ro 87,642; russian and ru 49,785; spanish and es 28,378;
swedish and sv 30,738; turkish and tr 96,325.

Decision (the lead): the row is recorded as a miss. The fix is the hunt row "Six stemmer
names", a separate quest, and this row is re-run when it lands.

### A5: decoders (miss as one CPU-hour)

The release reads one versioned format from prospect: the segment file (`.sx`; the postings,
values, field-dictionary and stored sections live inside it). prospect's other wire form,
`sortkey`, is not used by Xapiand. `sortkey_test`, which refuses an unknown version byte, passes
in the pin's suite.

Fuzzing: four libFuzzer targets built with ASan and UBSan at the release pins (job
261111-201102-4af80d), seeded from `fuzz/corpus/*`. Each ran six seeds for 600 s with
`-rss_limit_mb=2048 -timeout=10`: an hour of wall-clock time per decoder at nice 19, under light
holds beside the other row jobs. CPU time was not measured.

| Target | Seeds | Executions per run (with the replayed seeds) | exec/s | Coverage | Peak RSS | Findings |
|---|---|---:|---:|---:|---:|---:|
| `fuzz_segment` | 1-6 | 2,142-3,526 | 3-5 | 6,443-6,485 | 357-383 MB | 0 |
| `fuzz_postings` | 1-6 | 4,662-5,671 | 7-9 | 3,911-3,914 | 1,586-2,045 MB | 0 |
| `fuzz_values` | 1-6 | 10,198-15,255 | 16-25 | 2,460-2,470 | 440-477 MB | 0 |
| `fuzz_fielddict` | 1-6 | 17,883-19,743 | 29-32 | 4,598-4,758 | 555-599 MB | 0 |

Jobs: segment 261111-201105-74a86d, 261111-201108-63232e, 261111-201111-fb8de3,
261111-201114-9f665c, 261111-201117-334338, 261111-201120-b8edb5; postings 261111-201123-48fad1,
261111-201126-e1d239, 261111-201129-9bc82b, 261111-201132-4a9573, 261111-201135-50a022,
261111-201138-698b31; values 261111-201141-365038, 261111-201144-b67b2c, 261111-201147-634ad4,
261111-201150-c36d62, 261111-201153-bd3f1c, 261111-201156-5abf6c; fielddict
261111-201159-483e21, 261111-201202-8c50a4, 261111-201206-73d52f, 261111-201209-32089f,
261111-201213-5cf20a, 261111-201216-328810.

The hour did not reach a plateau. Each of the 24 runs was still adding new inputs in its last
0 to 8% of executions (segment seed 2's last new unit came at execution 2,534 of 2,534,
fielddict seed 2's at 19,587 of 19,587). Coverage looks flat across seeds because the runs
barely left the seed corpus. Over the coverage of the replayed seeds, postings gained 0 to 3
edges per run, values 8 to 18 and segment 60 to 102; fielddict's runs diverged (4,598 to 4,758).
`fuzz_segment`'s length limit stayed at 26 bytes for the whole run. Without the replayed seeds
(about 989, 844, 279 and 756), about 10k (segment), 26k (postings), 68k (values) and 110k
(fielddict) mutated inputs ran per decoder. Every run started from the same seed corpus and its
new units were discarded, so each decoder got six independent 10-minute starts, not an hour of
progress. Executions are slow because each input opens a segment under ASan; earlier records
show the same rates (`fuzz/MEMORY.md`, `docs/reviews/f13-query-code.md`).

Decision (the lead, on review F2): A5 is a miss as "one CPU-hour", with zero findings. Follow-ups:
hunt row 92 (the slow targets) and keeping the corpus across runs so the hour accumulates.

`fuzz_postings` peaked at 1,586 to 2,045 MB, and seed 6 came within 3 MB of the limit. This is
the growth `fuzz/MEMORY.md` records: strata's stored-section check decodes each frame afresh,
and ASan's quarantine keeps the freed blocks. The decoder does not leak. strata fixed it in
`strata:71750940`, which is past the release's `strata:5d742b2`. With the fix, the same hour
holds 374 to 408 MB. A longer run at this pin would end in an out-of-memory stop that is not a
finding. The pin is the lead's decision: moving the release's strata past `71750940` removes
the growth, and the row is re-run at the new pin either way.

Versions: `segment_test` opens each of 11 current fixtures. Under a good prefix CRC, each
fixture is refused as `unsupported_segment` at 10 unknown versions: version + 1,
min_read_version - 1, 0x80000000, 0x80000008, 14, 13, 12, 1, 0 and 0xffffffff. A refused file is
never read and never fenced as damage. Each of 8 format-12 fixtures is refused the same way.
Result: 19 fixtures, 118 of 118 refusals. The corpus replays (986 segment, 841 postings, 275
values and 751 field-dictionary inputs) pass in the Release build at the pins. The doc-value
column's own version byte (`include/prospect/docvalues.hh:281`) is refused as corrupt, not
unsupported, and has no fixture; the segment version that governs it is the one tested.

### A6: sanitizers

At the release pins: the reviewer's probe 261111-223819-03f694 built the eight row tests and
`parity` with CI's ASan+UBSan flags (Debug, -O1) at Xapiand's pins and ran them, plus parity at 4
segments and with deletes. It exited 0 in 912 s, with all 8 tests passing and no sanitizer
report, and reproduced the rows' numbers. The fuzz targets above are ASan+UBSan builds at the
same pins.

CI run 43153036741 at `prospect:2cc69d4` is green on every platform, including
`linux-asan-ubsan`, which runs the whole suite (the row tests among it) under ASan and UBSan,
and `linux-tsan`. CI builds at prospect's own pins (strata main), not Xapiand's.

## Differences from the oracle

| Row | Difference | Status | Decided by |
|---|---|---|---|
| A4 | `earlyenglish`, `german2`, `nb`, `nn` stemmed by Xapian, refused by snowball | behavior change, open: hunt row "Six stemmer names" | the lead |
| A4 | `english_lovins`, `english_porter` accepted by the schema, refused by both stemmers | open, same hunt row | the lead |
| A1 | weights before a merge with deletes (157 of 419 top 10s equal, worst relative error 0.0536) | by design: statistics count deleted documents until a merge; counts exact, everything exact after the merge | the row's definition (`phase3-ratchets` A6) |

## Residuals

- `parity`'s live check (`h.ord < n`) catches only the deleted extras past N, not the deleted
  older versions that reuse docids 1 to N; the exact counts cover those (review F5).
- `parity`, `verify`, `stems` and `fuzz` trust that their commit-named binary directory was
  filled by a `build` or `fuzz-build` that asserted the pins; nothing stamps it (review F7).
- The doc-value column's version byte is refused as corrupt and has no fixture of its own (review
  F8, A5).

## Reproducing

```sh
tools/release_rows/rows.sh build      # with --ref pin=, strata=, compressors=, hashes=, threadpool=
tools/release_rows/rows.sh parity
tools/release_rows/rows.sh verify
tools/release_rows/rows.sh stems
tools/release_rows/rows.sh fuzz-build # with the same refs
tools/release_rows/rows.sh fuzz TARGET SEED
```

Inputs: `$HOME/.scratch/release-rows/inputs` holds the 20,000- and 1,000,000-document corpora
(sha256 prefixes `6b2ba61f751bf7f6`, `d6fa4469c3c2963e`) and `queries.txt` (`a8e8f2c1a4bede53`).
The oracle is `strata-oracle`'s vectors (`rank.jsonl` `f70efd105342c2e7`) and its `stem` tool.

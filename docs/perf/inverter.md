# Inverter: one-thread indexing throughput

Targets T4's one-thread row (PLAN, "Indexing speed": at Phase 3, at least 1.0× the faster
of one-thread Lucene and Tantivy, keyed, at equal memory). At the n1017 gate run (5M
documents, `STRATA_BUDGET_MB=2056`, medians of five trials; `perf/indexing-gap`'s evidence
table) strata took 1,091 s, Tantivy-0.26-st 512 s, and Lucene-10.4.0-st 890 s: 0.47× the
faster.

Scope: the work one thread does per document (the engine's parse and tokenize, stage 1's
checks, the buffer) and the flush's inversion and encoding. Eight-thread scaling is
`perf/indexing-gap`'s, index size and merging `perf/index-size`'s.

**Design target** (PLAN, "Indexing speed", owner 2026-10-17): at least 4.5× the faster
baseline's throughput, about 1:55 at one thread on 5M documents against Tantivy's 8:30. Every
result below is stated as a ratio to Tantivy and as the distance left to 4.5×.

Every experiment reports the four axes: wall time, peak RSS, index bytes, and segments,
plus `verify` and oracle parity on its index. A/Bs run strata's two builds and
Tantivy-0.26-st (the anchor, and the drift check) interleaved, at least five trials, on
the first 100,000 documents of the gate corpus, after a 20,000-document dry run, under a
light hold.

## E0: where one thread's time goes (profile, no code change)

Hypotheses, each with what would refute it:

- H1: at 100,000 documents (no flush before the commit, no merges) more than half the
  time is per-document allocation, copying, and hashing ahead of encoding: the engine's
  `tokens_document`, stage 1's `valid` and the buffer's copy of the document, the version
  map, and `segment_writer::add`. Refuted if `flush_from`'s encoding (postings, positions,
  competitive pairs, the dictionary, stored frames) holds more than half the samples.
- H2: no single function holds more than a quarter of the samples. Refuted by one that
  does; it is then the first experiment's target.
- H3: the commit's sync is under 5% of the wall time on both platforms. Refuted if it is
  more.

Procedure: `strata_engine` at `4c9ebfd`, Release with `-march=native -g
-fno-omit-frame-pointer`, indexing the first 100,000 documents of the gate corpus with
`STRATA_BUDGET_MB=2056` (the gate's budget), a 20,000-document dry run first. macOS
(i9-9980HK, APFS): `sample` at 1 ms over the whole run; Linux (the gate VM): `perf record
-F 999 -g`. Reported: inclusive shares of each phase (read and parse, tokenize, stage 1 and
its parts, inversion, each encoder, the commit), and the hottest functions by self time.

### E0 results, macOS

21,454 samples (1 ms) over one run of 21.5 s, 100,000 documents, one segment, at `4c9ebfd`
(`.scratch/runs/e0-mac.sample.txt`, aggregated by `.scratch/tools/sample_agg.py`):

| Phase | Samples | Share |
|---|---:|---:|
| Engine: read lines, find `id` and `text` | about 210 | 1.0% |
| Engine: `tokens_document` (an `unordered_map` per document, a vector and a string per distinct term) | 7,105 | 33.1% |
| Stage 1, `index::apply`: `valid()`'s `std::set` 3,256 (15.2%), the buffer's deep copy of the document 1,067 (5.0%), keys and the version map about 300 | 4,828 | 22.5% |
| Clearing the transaction (`~operation`) | 669 | 3.1% |
| Flush: inversion, `segment_writer::add` (`buffer_for` 1,835, `put_stream` 1,074) | 4,592 | 21.4% |
| Flush: encoding, `segment_writer::flush` (term sort 1.8%, postings, positions, impacts, dictionary, stored frames) | 2,712 | 12.6% |
| Flush: freeing the buffered documents, other | about 1,250 | 5.8% |
| Commit: files and sync | 85 | 0.4% |

The allocator's own frames (`malloc`, `free`, and their nanozone helpers) hold about 30% of
all samples by self time. The corpus sample has 18.3M tokens, 8.8M distinct (document, term)
pairs, and 338,367 distinct terms besides the 100,000 keys: the current path spends about
390 ns per token in `tokens_document` and 370 ns per distinct term in `valid()`.

- H1 holds: per-document allocation, copying, and grouping (the engine, stage 1, and
  inversion) hold 80% of the samples; encoding holds 12.6%.
- H2 is refuted by `tokens_document` (33.1% inclusive), the first target.
- H3 holds: the commit is 0.4%.

Linux (the gate VM): owed, while the T3 run and the indexing gap's hold have the VM.

### Calibration, macOS, 100,000 documents

Five interleaved rounds (`.scratch/tools/ab_index.py`, `runs/cal-mac`), keyed, strata at
`STRATA_BUDGET_MB=2056`:

| Engine | Median wall | Range | Peak RSS | Index bytes | Segments |
|---|---:|---:|---:|---:|---:|
| Tantivy-0.26-st | 12.10 s | 11.62–12.45 | 420 MB | 47,919,012 | 1 |
| strata `4c9ebfd` | 21.88 s | 21.35–27.44 | 1,141 MB | 43,493,711 | 1 |

strata's throughput is 0.544× Tantivy's (95% interval 0.449–0.569). 4.5× is about 2.69 s
at this sample on this host: 8.1× faster than strata today.

## Experiments

Each is written here before its A/B runs: the hypothesis, the mechanism, and the rule that
keeps or removes it.

### E1: documents as token streams, no per-term allocation

Hypothesis: the engine's grouping of tokens by term and stage 1's duplicate check, with the
heap objects per distinct term they create and free (an `unordered_map` node, a vector, and a
string per term in the engine; a `std::set` node per term in `valid()`; the buffer's deep copy
of all of them), are 50 to 60% of the one-thread time at 100,000 documents, and come out
without changing a byte of the segment.

Mechanism:

- `document` gains a token stream, additively: `text`, one string, and `tokens`, each a term
  at an offset and size of `text` and a position. Each token adds one to its term's wdf and its
  position to the term's positions, as `segment_writer::add_tokens` already inverts them
  (byte-identical to `add`, `segment_test`). `terms` keeps the terms with an explicit wdf, the
  positionless ones, and the key; a term is in one of the two.
- Stage 1 validates tokens without a set: in bounds, sized 1 to `max_term_bytes`, positions
  strictly increasing, none with the key prefix (the key stays in `terms`); a `terms` entry
  among the tokens is found through the entries' first bytes, and `terms` itself is checked by
  sorting its views when it has more than one.
- The flush inverts each buffered document's `terms` and `tokens` as one document.
- The engine builds that form: the text copied once, one token per word, the key in `terms`:
  two allocations per document instead of about three per distinct term.

Neither baseline hands the writer a document this way: Lucene's `IndexingChain` and Tantivy's
`SegmentWriter` pull each field's tokens one at a time from an analyzer's stream and insert
each into the term hash as it comes. A flat array of a document's tokens is what lets a later
step hash them all first and prefetch their table slots ahead of the probes.

Keep rule: every segment file byte-identical to `4c9ebfd`'s on the 20,000 and 100,000
samples, ctest green, and the 100,000-document A/B at least 1.3× faster than `4c9ebfd` with
the interval excluding 1.0. Otherwise removed.

Result (`d5468e4`): kept. Every segment file, manifest, and metadata file byte-identical to
`4c9ebfd`'s at 20,000 and 100,000 documents; ctest 33 of 33. Five interleaved rounds at
100,000 documents (`runs/e1-ab`, the host's load falling during the run, Tantivy 9.31 → 7.00 s):

| Engine | Median wall | Range | Peak RSS | Index bytes |
|---|---:|---:|---:|---:|
| Tantivy-0.26-st | 8.09 s | 7.00–9.31 | 421 MB | 47,919,012 |
| strata `4c9ebfd` | 13.88 s | 11.84–17.25 | 1,141 MB | 43,493,711 |
| strata E1 | 6.73 s | 5.59–8.12 | 829 MB | 43,493,711 |

E1 is 2.12× faster than `4c9ebfd` (per-round ratios 2.01–2.34, interval 2.01–2.34) and
1.20× Tantivy's throughput (interval 1.15–1.26), up from 0.58×; 4.5× is 3.74× away. Peak RSS
falls 27%, since a token document is smaller than its terms grouped.

What is left (`runs/e1-mac.sample.txt`, about 4,900 active samples): the flush holds 84% of
them, inversion (`segment_writer::add`) 49% and encoding (`flush_from`) 28%; the per-document
path (parse, stage 1, the buffer copy) 15%. In inversion, `buffer_for`'s hash, probe, and
`memcmp` take 22% and the LEB128 streams in `std::string`s 18%; in encoding,
`write_postings` 21%, decoding the streams back 17%, positions 11%, competitive pairs 9.5%,
the dictionary 7%, checksums 7%.

### E2: inversion on arrival

Hypothesis: buffering documents and inverting them at flush costs one copy and one free of
every document (about 7% at 100,000 documents) and, at the gate's scale, holds the buffer in
a form about five times larger than its postings (a token document is about 3.3 KB here; its
postings, positions, and columns about 0.6 KB), so a budget holds a fifth of the documents a
flush could, and 5M documents make five times the segments and the merges.

Mechanism: stage 2 inverts each accepted transaction's documents into the index's active
`segment_writer` as `apply` returns (D18's stage 2 at one thread: the design's diagram
inverts on arrival; the code inverted at flush). Stage 1 evaluates on pointers into the
transaction, so a rejected transaction still leaves nothing. A document replaced or deleted
while buffered is marked dead by ordinal. The flush writes the writer's postings as they are
when the buffer's docids increase and none died (the case for appends); otherwise it rebuilds
them in docid order without the dead, as a merge writes its output (`add_document` and
`flush_from`), so the segment is byte for byte what flushing the live documents in docid
order writes today. The budget charges the writer's own allocations (D17) instead of an
estimate per document.

Neither baseline does the slow half this way: Lucene and Tantivy also invert on arrival, but
keep a replaced document in the flushed segment as a deleted one; strata's segments never
hold a document dead at birth, so the format is unchanged.

Keep rule: byte-identical segments at 20,000 and 100,000 documents; a test that replaces,
deletes, and reorders docids inside one buffer checks the flushed segment against the same
live documents flushed in docid order (equal bytes and digests); ctest green; the 100,000
A/B not slower than E1 (a win at the gate's scale is checked by the confirmation run, since
100,000 documents never fill the buffer). Otherwise removed.

Result: kept, as the scaffold for E3. Byte-identical segments, manifests, and metadata at
20,000 and 100,000 documents; ctest 33 of 33, with a new index_test case (12 seeds of 900
transactions: replaces and deletes inside the buffer, explicit docids out of order,
rejected transactions, term and token documents mixed) whose segments equal, byte for byte,
an index that buffers whole documents and inverts them at flush; replacing the rebuild with
the as-is flush makes it abort ("docids must be distinct"). The term table and arena now
start small (1,024 slots, 4 KiB chunks), since a buffer holds them from its first document.
Five interleaved rounds at 100,000 documents under host load (`runs/e2-ab`):

| Engine | Median wall | Range | Peak RSS |
|---|---:|---:|---:|
| Tantivy-0.26-st | 8.47 s | 7.32–9.05 | 418 MB |
| strata E1 | 6.89 s | 6.11–7.72 | 817 MB |
| strata E2 | 6.28 s | 5.40–7.17 | 446 MB |

E2 is 1.077× faster than E1 (interval 1.034–1.131) at 45% less peak RSS, and 1.26× Tantivy
(interval 1.17–1.57); 4.5× is 3.57× away. The memory is the larger effect: 446 MB is about
4.5 KB a document in RSS for the whole process, where E1 held 8.2 KB.

### E3: the tuple-stream inverter (the research note's C)

The research note (`research/indexing`, `docs/research/indexing.md` sections 3, 5.2, 6)
measured three inverters writing byte-identical segments through `flush_from`: today's
hash-append (A, 340 ns a token on this host), Lucene's and Tantivy's slices (B, 139), and
tuples in arrival order with a radix-partitioned flush (C, 118; its add phase 25 against
A's 191). The lead's direction (2026-10-17): build C into the index path, take out the
per-document representation and bookkeeping, then the encoder's per-term cost.

Hypothesis: with C as the active buffer, the add phase (now `segment_writer::add` on
arrival, about half of E2's time) falls to the cost of hashing each token and appending a
tuple, and the flush's grouping stays in cache; 100,000 documents at least 1.5× faster
than E2.

Mechanism, as the note's section 6 lays it out, with what E2 built kept around it:

- On arrival each token becomes a 20-byte tuple (hash, ordinal, text offset, position,
  length) appended in arrival order, the token's bytes copied once into the buffer's text
  arena; nothing is looked up.
- At flush the tuples are partitioned by the hash's top byte into 256 runs; each run gets
  an L2-sized table keyed by hash and a stable counting sort by term, and every token's
  bytes are compared with its term's first, so two terms whose hashes collide are never
  merged. Terms sort, and each term's postings go to `flush_from` in ordinal order, so the
  encoder and the segment are unchanged.
- E2's dead ordinals and docid order become a relabelling during the grouping: one lookup
  per tuple through the live documents' new ordinals.

Keep rule: byte-identical segments at 20,000 and 100,000 documents; the E2 equivalence test
and ctest green; the 100,000 A/B at least 1.3× faster than E2 with the interval excluding
1.0. Otherwise removed.

Result (`include/strata/tuple_buffer.hh`): below its own keep rule on time (1.21×, not 1.3×),
kept on the lead's direction as the base of the research design (the partition on arrival,
the term-partitioned flush at more threads, document order at flush); its memory is the open
cost. Byte-identical segments, manifests, and metadata at 20,000 and 100,000 documents;
ctest 34 of 34, including `tuple_buffer_test` (6 seeds of 2,000 random documents against
`segment_writer::add`, equal bytes with the hash whole and narrowed to masks that make terms
collide, tokens of 254, 255, and 300 bytes, positions past 2^24, terms given whole with an
explicit wdf, positionless, or long, documents left out and docids shuffled; with the
collision split disabled it fails) and E2's equivalence test, now over the tuple buffer.
E2's `segment_writer` additions (the rebuild, scans, per-document keys) are gone; the
writer is E1's again, with `no_more_docs` now declared in it.

Five interleaved rounds at 100,000 documents (`runs/e3-ab2`):

| Engine | Median wall | Range | Peak RSS |
|---|---:|---:|---:|
| Tantivy-0.26-st | 6.52 s | 5.79–6.77 | 420 MB |
| strata E2 | 5.33 s | 4.82–5.47 | 447 MB |
| strata E3 | 4.22 s | 3.75–4.39 | 1,229 MB |

E3 is 1.214× faster than E2 (interval 1.156–1.459) and 1.544× Tantivy (interval
1.494–1.634); 4.5× is 2.91× away. The first build's time was 1.086× E2 until the zeroing
went: value-initialized tuple chunks, arena chunks, and the flush's partition copy cost 13%
of the samples in `bzero`; the hash now reads a zeroed word written after each document's
bytes instead.

Peak RSS is 2.75× E2's: 20 bytes a token, the text, and the flush's partition copy of every
tuple (368 MB here). At the gate's equal memory that means fewer documents per flush and
more segments at 5M than E2. Where the time goes now (`runs/e3-mac.sample.txt`, about 3.0 s
active): the flush 61% (`flush_from` 28%, grouping 16%, the term sort about 8%, partition
8.5%, assembling postings 5%), stage 1 and the buffer's add 19%, the engine's tokenizing
7.5%.

### E4: the encoder's per-term cost

Hypothesis: `flush_from` spends about 1.9 µs a term here (849 samples over 438,467 terms in
E3's profile), and most of it is work no term needs: every term's whole-list competitive
pairs are built (a vector of every posting's pair, then a 256-entry scan) although the term
info stores them only for terms with skip blocks (df ≥ 128, 1.8% of terms here); terms with
fewer than 128 postings build their LEB128 tail in a string of its own before copying it;
each block of a long list allocates its pairs; and the tuple buffer assembles each term's
postings with a `push_back` per posting and position. Without them the encoder costs under
1 µs a term and the 100,000-document run at least 10% less.

Mechanism, byte for byte the same output:

- Whole-list pairs only for df ≥ 128, from a scratch reused across terms.
- A list without full blocks writes its tail in place; a long list's per-block pairs reuse
  one scratch.
- The tuple buffer sizes a term's posting arrays from its tuple count and fills them by
  index.

Keep rule: byte-identical segments at 20,000 and 100,000 documents, ctest green, and the
100,000-document A/B at least 1.1× faster than E3 with the interval excluding 1.0.
Otherwise removed.

Result: kept by BENCHMARKS rule 3 (the interval excludes 1.0 and the effect passes 3%),
though short of the 1.1× this item set itself and of its hypothesis: the encoder fell from
849 to 584 samples, 1.94 to 1.33 µs a term, not under 1 µs. Byte-identical segments at
20,000 and 100,000 documents; segment, index, and tuple_buffer tests pass. Five interleaved
rounds at 100,000 documents (`runs/e4-ab`):

| Engine | Median wall | Range | Peak RSS |
|---|---:|---:|---:|
| Tantivy-0.26-st | 6.65 s | 5.79–6.84 | 417 MB |
| strata E3 | 4.10 s | 3.51–4.36 | 1,229 MB |
| strata E4 | 3.86 s | 3.37–3.98 | 1,246 MB |

E4 is 1.095× faster than E3 (interval 1.036–1.202) and 1.710× Tantivy (interval
1.698–1.772); 4.5× is 2.63× away. What is left of the encoder (`runs/e4-mac.sample.txt`):
assembling each term's postings from its tuples 122 samples, `write_postings` 117,
`write_positions` 74, the sections' checksums 54, the dictionary 47, the key order check 29.

Process note: this item's build and tests (about 21:35 to 21:47) ran while
`perf/indexing-gap`'s strict hold `ig-e2` (an eight-thread A/B on the gate VM) was active;
the check printed the hold and the command went on. Its numbers from that window may need a
rerun. Every compile, test, and measurement here now goes through `.scratch/gate.sh`, which
refuses under a strict hold.

### E5: grouping without reading the text, terms sorted by 8-byte chunks

Hypothesis: of E4's flush, the grouping's check of every token's bytes against its term's
(a random read of the text and a `memcmp` per token) and the sort of the segment's terms
(`std::sort` over 438,467 `string_view`s, a `memcmp` per comparison, through 100,000 keys
that share a 38-byte prefix) are about 15% of the run, and most of it can go without
weakening either.

Mechanism:

- A token of at most 8 bytes hashes by one masked word load, a multiply, and an xorshift,
  then adds its length times a constant: for a fixed length that is a bijection on its
  bytes, so two such tokens with equal hashes and lengths are the same term. Only longer
  terms, terms given whole, and a narrowed test hash compare bytes.
- Terms sort by their bytes in 8-byte big-endian chunks with the remaining length (an
  integer sort, no `memcmp`); runs that tie on a chunk and continue are sorted again on
  the next chunk, so keys sharing a long prefix cost a few integer passes.

Lucene's `BytesRefHash` and Tantivy's `ArenaHashMap` compare bytes on every probe; neither
skips the comparison by the hash's structure. For the sort, Lucene's `BytesRefHash.sort`
already uses an MSB radix sort over bytes (`MSBStringRadixSorter`) and Tantivy
`sort_unstable_by` over term slices; the chunked sort here is the MSB idea with a 64-bit
digit.

Keep rule: byte-identical segments at 20,000 and 100,000 documents, tests green with the
collision masks, and the 100,000-document A/B faster than E4 by BENCHMARKS rule 3. Otherwise
removed.

Result: unresolved on this host, kept provisionally (E9 builds on both mechanisms); a rerun
on the gate VM is owed. Byte-identical segments; tests pass. Three A/B sessions at 100,000
documents, 17 rounds pooled (`runs/e5-ab`, `e57-ab`, `e5-ab2`): E5 is 1.066× E4, interval
0.985–1.130, so the interval does not exclude 1.0 (the host's load came and went: one
session's E4 ranged 3.42–4.88 s). The profile attributes the effect where it was aimed
(`runs/e4-mac` against `runs/e5-mac`): the grouping 408 → 311 samples and the term sort
about 160 → 80, about 180 ms of a 3.4 s run (5%).

### E7: the engine's front end

Hypothesis: the engine builds each document afresh (an `operation` per line, `"Q" + id`
strings, `std::getenv` per line) and splits the text with a `memchr` call per word; reusing
the batch's operations (their strings and vectors keep their capacity) and splitting 8 bytes
at a time with an exact space mask take most of `tokens_document`'s 7% and the documents'
construction and destruction out of the run.

Keep rule: byte-identical segments at 20,000 and 100,000 documents, and the 100,000-document
A/B faster than E5 by BENCHMARKS rule 3. Otherwise removed.

Result (`f6b06a1`): kept. Byte-identical segments at 20,000 and 100,000 documents; the index
test passes. Six interleaved rounds at 100,000 documents on a quiet host (`runs/e57-ab`;
Tantivy 5.73–5.78 s): E7 is 1.043× E5 (interval 1.024–1.075) and 1.875× Tantivy (interval
1.833–1.895); 4.5× is 2.40× away. Peak RSS rises from 1,248 to 1,346 MB: the reused
operations keep a batch's capacity.

### E9: C in batches, into a dense term store

Hypothesis: C keeps every token as a 20-byte tuple until the flush, so the buffer is about
2.75 times E2's and the process pays a page fault per 4 KiB it touches: E4's run at 100,000
documents spent 0.73 s of its 3.37 s in the kernel on 301,088 page faults (macOS zero-fills
each page, about 2.4 µs), and at the gate's equal memory a 2 GB buffer of tuples holds about
400,000 documents, 12 to 13 flushes and merges at 5M. Grouping each batch of about a million
tuples as it fills (C's partition and counting sort, unchanged, in cache) and appending each
term's postings for the batch to a compressed store keeps the buffer at about a fifth of
the tuples' size and the tuples' memory reused, so at 100,000 documents the run is faster by
the faults it no longer takes, and at 5M the same budget holds about five times the
documents per flush.

Mechanism: tuples go into 256 buckets by the hash's top byte as they arrive (the note's
refinement D, so no partition pass); when a batch fills, each bucket is grouped and
verified as C does, and each group (one term's postings in the batch) costs one probe of
the store's term table and one appended chunk of LEB128 postings (ordinal deltas, wdfs,
position deltas) linked from its term, newest first; the batch's tuples, text, and side
entries are then reset with their memory kept. The flush drains the last batch, sorts the
store's terms by 8-byte chunks, and decodes each term's chunks into `flush_from`, as E2's
flush decoded its streams; documents left out or reordered relabel there. Neither baseline
groups before it appends: Lucene's and Tantivy's writers probe their term table and append
to a term's slices for every token.

Keep rule: byte-identical segments at 20,000 and 100,000 documents; tuple_buffer_test with
batches drained every 1, 7, and 500 tuples and the collision masks; ctest green; the
100,000-document A/B faster than E7 by BENCHMARKS rule 3, and peak RSS at least 40% lower.
Otherwise removed.

Result: kept. Byte-identical segments, manifests, and metadata at 20,000 and 100,000
documents; ctest 34 of 34, with tuple_buffer_test now draining batches every 1, 7, and 500
tuples (with and without the collision masks, with documents left out and reordered) and
checking keys and scans before and after a drain. Six interleaved rounds at 100,000
documents (`runs/e9-ab`):

| Engine | Median wall | Range | Peak RSS |
|---|---:|---:|---:|
| Tantivy-0.26-st | 5.97 s | 5.58–6.37 | 416 MB |
| strata E7 | 3.45 s | 3.02–4.82 | 1,352 MB |
| strata E9 | 3.03 s | 2.99–3.06 | 588 MB |

E9 is 1.143× faster than E7 (interval 1.002–1.586; E7's two slow rounds are its page faults
under the host's memory pressure, E9's range is 70 ms) at 57% less peak RSS, and 1.975×
Tantivy (interval 1.876–2.071); 4.5× is 2.28× away.

### E10: small terms flushed as their chunks' bytes (removed)

Hypothesis: a term with fewer than 128 postings and 128 positions (98% of the terms) can go
to `flush_from` as the format's bytes, its chunks' ordinal, wdf, count, and position
sections end to end, instead of being decoded into arrays and encoded again.

Result: removed. Byte-identical segments; tests pass. Six interleaved rounds at 100,000
documents (`runs/e10-ab`): 1.017× E9, interval 0.991–1.022, not a win. E9's profile
(`runs/e9-mac.sample.txt`) says why: the small terms are many but cheap; the encoder's time
is in the long lists, which hold 81.5% of the postings: decoding the chunks 270 samples (98
of them walking each term's chain of chunks, a cache miss per chunk), `write_postings` 144,
`write_positions` 67, the output's string and checksum path about 170, the dictionary 61.

### E15: the chunks decoded section by section, their chains fetched ahead

Hypothesis: E9's decode spends 98 of its 270 samples chasing each term's chain of chunks
(one dependent miss per chunk) and the rest in a `push_back` per value with a separate
LEB128 call; fetching the next terms' newest chunks while one term decodes, and decoding
each section in its own tight loop into arrays sized from the term's counts, takes most of
it away: at least 3% of the run by BENCHMARKS rule 3.

Mechanism: the flush prefetches the head chunk of the term eight places ahead in sorted
order; a term's arrays are sized once from its df and position count; each chunk's ordinal
deltas accumulate from −1 with wraparound (the first delta is the ordinal + 1), and its
wdfs, counts (or a copy of its wdfs when it has none apart), and positions decode in loops
of their own with a one-byte fast path.

Keep rule: byte-identical segments at 20,000 and 100,000 documents, tests green, and the
100,000-document A/B faster than E9 by BENCHMARKS rule 3. Otherwise removed.

Result: removed. Byte-identical segments; tests pass. Six interleaved rounds at 100,000
documents (`runs/e15-ab`): 1.017× E9, interval 0.990–1.030, not a win. Tight loops do not
help a decode whose cost is the chunks' memory: a term's chunks sit one per batch across the
whole log, and reading them in term order is a dependent miss per chunk, which fetching the
next terms' newest chunks does not cover.

### Linux: E9 against the base and Tantivy (gate VM)

Five interleaved rounds at 100,000 documents in the gate VM (Ubuntu 24.04, clang, the page
cache dropped before each run; `~/inv/runs-e9`):

| Engine | Median wall | Range | Peak RSS |
|---|---:|---:|---:|
| Tantivy-0.26-st | 7.10 s | 6.38–7.25 | 291 MB |
| strata `4c9ebfd` | 13.50 s | 12.26–14.37 | 1,263 MB |
| strata E9 (`2117583`) | 3.11 s | 2.88–3.16 | 463 MB |

On Linux E9 is 4.27× the base (interval 4.05–4.62) and 2.272× Tantivy (interval
2.187–2.331); the base was 0.512×, as at the n1017 gate run. 4.5× is 1.98× away. `perf
record -g` over E9 (2,190 samples, `~/inv/perf-e9.data`): the batches' drains 28.5% (the
chunks' appends 8.6%, the term table's probes 7.6%, the grouping 6.9%), the flush's
decode of the chunks 15.3% (6.5% of it walking each term's chain), the encoder's long lists
9.2%, stage 1 9.3%, the engine's split 6.7%, page faults 7%, tuple pushes 5%.

Batch size, macOS, five rounds (`runs/batch-sweep`): 2^18 tuples 0.931× E9's 2^20, 2^21
1.014× (peak RSS +48 MB), 2^22 0.990× (+125 MB). 2^20 stays.

### E18: three per-call costs (the term table's probes, the version map, section writes)

Hypothesis: three overheads that each sit near the A/B's noise are worth about 4% together:
the drain probes the store's term table one group at a time (7.6% on Linux, a cache miss
per probe of a term not seen in the batch before); stage 1 builds a `std::string` to look a
key up in the version map and probes it again to update it (`lookup_key` and `set_key`,
about 3%); and every term's few bytes go to the section streams in their own call, each one
a virtual write and two checksum calls (about 2% on macOS).

Mechanism: the drain fetches the table slots of the groups eight ahead of the one it
probes; the version map is found by view (a transparent hash) and updated through the
iterator its lookup returned; a section gathers writes in 64 KiB before they reach the
output and the checksums. Each part's own function is checked in the profile.

Keep rule: byte-identical segments at 20,000 and 100,000 documents, tests green, and the
100,000-document A/B faster than E9 by BENCHMARKS rule 3. Otherwise removed.

Result: kept, as one item. Byte-identical segments at 20,000 and 100,000 documents; ctest 34
of 34. Two A/B sessions at 100,000 documents, six rounds each (`runs/e18-ab`, `e18b-ab`):
all three parts 1.055× E9 (interval 1.028–1.066), then 1.044× (1.009–1.066) beside a build
without the gathered section writes, 1.024× (1.004–1.042), which alone is under the rule's
3%; the gathering's own share is 1.019× (0.988–1.041). In the profile the term table's
probes fall 161 → 108 samples; the version map's and the output path's shares do not move
measurably in one sampled run. E18 is 1.943× Tantivy (interval 1.908–2.005) on macOS; 4.5×
is 2.32× away there.

### E17: long chunks bit-packed in the store

Hypothesis: the flush's decode of the store (15.3% on Linux) is mostly LEB128 decoding a
value at a time, 30M values at 100,000 documents of which the long lists hold about 80%;
storing each chunk section's full runs of 128 values as PFOR blocks (bitpack's SIMD
encoder and decoder, the same codec as the format's blocks) and only the rest as LEB128
makes decoding them a block unpack instead of a branch per byte: at least 3% of the run
by BENCHMARKS rule 3, at no more memory.

Mechanism: append() gathers a chunk's ordinal deltas, wdfs, counts, and position deltas as
32-bit values, then writes each section as PFOR blocks of 128 and a LEB128 tail; decode()
unpacks the blocks into the term's arrays and reads the tails. A chunk of fewer than 128
values per section is LEB128 as before.

Keep rule: byte-identical segments at 20,000 and 100,000 documents, tests green, peak RSS
within 3%, and the A/B faster than E18 by BENCHMARKS rule 3, on macOS and the gate VM.
Otherwise removed.

Result: removed. Byte-identical segments; tests pass. macOS, six rounds (`runs/e17-ab`):
0.995× E18, interval 0.976–1.020; the gate VM, five rounds (`~/inv/runs-e17`): 0.993×,
interval 0.970–1.084. Like E15, a cheaper decode of the values does not move the run: the
decode's cost is reaching the chunks, not reading them. On the gate VM E18 (with the small
first pages) is 2.390× Tantivy (interval 2.164–2.470); 4.5× is 1.88× away there.

### E19: sorted runs instead of a term table

Hypothesis: E9's store costs two random accesses per term and batch, a probe of a term
table that outgrows the cache (7.6% on Linux) and, at the flush, a dependent miss per chunk
while walking each term's chain across the log (6.5%), and E15 and E17 showed the decode
is bound by those misses, not by its arithmetic. Sorting each batch's terms and writing the
batch as one run (an entry per term, in order, in one log piece) removes the table, and the
flush, merging the runs by term, reads every run in order: at least 3% by BENCHMARKS rule 3
on the gate VM, not slower on macOS, and better as the vocabulary grows past the cache.

Mechanism: the drain groups the batch as before, sorts its groups by 8-byte chunks, and
writes each as an entry ([term size][term][postings, positions, cf, flags][section
lengths][LEB128 ordinal deltas from −1, wdfs, counts when one differs from its wdf, position
deltas]); a key's bytes are found in its run. The flush keeps a heap of one cursor per run;
a term's entries come out together and decode in run order, which is ordinal order. The
alphabet is the union of the terms' bytes, kept as runs are written (a first merge pass finds
it when documents are left out). This is the multiway merge of sorted runs that Witten,
Moffat, and Bell describe for building an index on disk, applied in memory to a batch at a
time; Lucene and Tantivy keep one hash table of terms for the whole segment.

Keep rule: byte-identical segments at 20,000 and 100,000 documents, tests green (with a run
larger than a log page), and the A/B by BENCHMARKS rule 3 on the gate VM and not slower on
macOS. Otherwise removed.

Result: kept. Byte-identical segments, manifests, and metadata at 20,000 and 100,000
documents; ctest 34 of 34; `tuple_buffer_test` adds 40,000 documents in one batch, whose run
passes a log page (the first build placed keys by log offset and crashed there; keys are
now pointers into their run). Six interleaved rounds at 100,000 documents:

| Platform | E18 | E19 | E19 / E18 | Against Tantivy | Peak RSS |
|---|---:|---:|---:|---:|---:|
| Gate VM (`~/inv/runs-e19`) | 2.95 s | 2.72 s | 1.075× (1.030–1.100) | 2.487× (2.445–2.530) | 450 → 414 MB |
| macOS (`runs/e19-ab`) | 2.90 s | 2.77 s | 1.044× (0.982–1.074) | 2.029× (1.867–2.120) | 587 → 559 MB |

4.5× is 1.81× away on the gate VM and 2.22× on macOS.

### 1M documents: E19 on the gate VM

Two rounds at 1,000,000 documents, keyed, `STRATA_BUDGET_MB=2056` (`~/inv/runs-1m`):
Tantivy-0.26-st 111.8 s (106.0–117.6; peak RSS 3.01 GB, 683 MB, one segment), E18 43.4 s
(3.90 GB), E19 41.8 s (40.5–43.1; 3.83 GB, 620 MB, one segment): 2.672× Tantivy (interval
2.619–2.726), a little above the 100,000-document ratio, so the shape holds at ten times
the documents. The memory does not: the buffer never filled its 2,056 MB share (one flush, at
the commit), but the flush built the whole 620 MB segment in a string, its capacity
doubling, beside the positions spill and the buffer itself, and peak RSS passed Tantivy's.

### E11: the flush writes its segment to the file as it goes

Hypothesis: at 1M documents most of E19's peak RSS above the buffer is the segment the
flush builds in a `std::string` (up to twice its 620 MB in capacity) before writing it;
writing it to its file a MiB at a time instead cuts peak RSS by at least a quarter there,
and costs no time (one copy fewer).

Mechanism: the index creates the segment's file first and hands the flush a sink that
writes it from its start in 1 MiB pieces; ENOSPC removes the partial file and keeps the
buffer, as `write_unsynced` does; the segment is then mapped (or read back where the
backend cannot map) and its footer checked. The positions section still gathers in memory
until the postings end, as before.

Keep rule: byte-identical segments, tests green (the ENOSPC cases included), peak RSS at 1M
at least 25% lower, and the 100,000-document A/B not slower than E19. Otherwise removed.

Result: kept by BENCHMARKS rule 3 on memory, short of the 25% this item set: peak RSS falls
21% at 1M (3,832 → 3,030 MB in all five rounds, `~/inv/runs-1m-e11`) and 20% at 100,000
(556 → 447 MB, `runs/e11-ab`), and time does not move (1.005× at 100,000 on macOS, interval
0.965–1.085; 1.002× at 1M on the gate VM over the last three rounds, 0.910–1.062).
Byte-identical segments; ctest 34 of 34, the ENOSPC cases included.

What is left at 1M is mostly the buffer's own size: E19's runs repeat each term's bytes and a
36-byte header in every batch that holds it, about 12M entries at 1M documents, so the
entries' headers and terms take about as much as the postings themselves. Compact headers
are the next memory item.

## Interface with perf/indexing-gap

perf/indexing-gap builds the flush on more than one thread over this file's buffer (it owns
the parallel flush and the segment's assembly; this branch owns `tuple_buffer.hh` and the
one-thread flush). What the buffer hands the flush, by commit:

- `f6b06a1` (E7, which perf/indexing-gap merged at `a6d8cbb` and built its E3 on): every
  tuple kept to the flush in arrival order; the flush partitions them into 256 runs by the
  hash's top byte, groups each run (`group_run`: table, counting sort, verification), sorts
  the groups by term, and hands each group's postings to `flush_from`.
- `2117583` (E9) changes it: tuples go to 256 buckets as they arrive, and every batch of
  about a million is grouped and appended to a term store (a term table and a chain of LEB128
  chunks per term); the flush holds no tuples and no partitions, only the store's terms,
  each decoded alone. `flush()` itself is unchanged in shape.
- `10eca3e` (E19) changes it again: there is no term table; each batch becomes a run, its
  terms sorted, one entry per term; the flush merges the runs by term with a heap. The
  partitions of E7 no longer exist at the flush: a flush on several threads splits by key
  range instead, each thread merging the runs over its range (each run is sorted, so a range
  is a contiguous stretch of every run), and the assembly concatenates the ranges in order.
- E11 (this commit) changes `flush()`'s signature: it writes to an `output&` and a
  `spill&` (the index passes the segment's file and an in-memory positions spill) instead
  of returning the segment as a string.

The boundary from the commit after E11 on is a public set of steps of `tuple_buffer`, the
same ones its one-thread `flush()` takes, so a parallel flush depends on them and not on the
buffer's internals:

1. `drain_for_flush(run)`: the last batch drained, its allocations on the flush's charge.
2. `plan(order)`: which documents are written and their new ordinals (`flush_plan`).
3. `add_documents(w, plan)`: the columns into a writer, in that order.
4. `alphabet(plan)`: every byte the written terms use.
5. `splitters(ranges)`: up to `ranges - 1` increasing terms sampled from the runs (every
   64th entry of each), cutting the terms into ranges of about equal counts.
6. `each_term(lo, hi, plan, f)`: each term in `[lo, hi)` in byte order with its postings in
   the plan's numbering, as `flush_from` takes them; read-only on a drained buffer, so
   disjoint ranges can run on several threads at once.

`tuple_buffer_test` checks that ranges cut at `splitters()` concatenate to the whole, with
and without documents left out, over one run and many. Segments and times are unchanged
(1.013×, interval 0.978–1.022).

Later commits keep these six steps as they are. What changed beside them:

- `097d33f` (E23), `297464f` (E26), `0a5cabe` (E33), `c6bf4d9` (E34): the drain's
  internals and the run entry's format, private to `tuple_buffer`.
- `147be0e` (E25): the merge's heap, inside `each_term` and `flush`.
- `b7dd0f4` (E31): `tuple_buffer::add_split`, beside `add`, for documents whose text the
  index splits (`document::split_text`); the index's multi-thread path splits such a
  document into explicit tokens when it buffers it.

### E20: compact run entries

Hypothesis: a run entry's fixed header (a 4-byte term size and eight 4-byte fields) is 36
bytes beside a term of 8 and postings of a few bytes, and there is one per term and batch
(about 1.3M at 100,000 documents, 12M at 1M); writing the header as LEB128 values and a flag
byte (about 9 bytes for most entries) makes the buffer about a quarter smaller at 1M and the
run faster by the pages it no longer touches.

Mechanism: an entry is [term size][term][postings][positions][cf][flags u8][four section
byte lengths], every number LEB128, then the sections as before; the run's marks and keys
point at entries and terms as before.

Keep rule: byte-identical segments, tests green, peak RSS lower at 100,000 and 1M, and the
100,000-document A/B not slower than the previous commit. Otherwise removed.

Result: kept. Byte-identical segments; ctest 34 of 34.

| Run | Time, E20 / previous | Peak RSS |
|---|---:|---:|
| macOS, 100,000, six rounds (`runs/e20-ab`) | 0.998× (0.982–1.011) | 457 → 409 MB |
| Gate VM, 100,000, five rounds (`~/inv/runs-e20`) | 1.015× (0.996–1.037) | 359 → 326 MB |
| Gate VM, 1M, two rounds (`~/inv/runs-1m-e20`) | 1.021× (1.008–1.035) | 3,031 → 2,580 MB |

On the gate VM E20 is 2.541× Tantivy at 100,000 documents (interval 2.440–2.575) and, at 1M,
38.6 s against Tantivy's 111.8 s of the 1M session (2.89×).

### E21: the front end without zeroing or a branch per check

Hypothesis: the engine sizes each document's tokens by resizing to the most a text can hold,
which zeroes about 7 KB a document before overwriting part of it (a `std::vector` resize
value-initializes), and stage 1 tests each token's six conditions with a branch each; the
split and the checks are about 10% of the run on Linux (`fill_document` 6.7%, `valid` 3.1%).
Reserving the tokens and appending them, and folding the bounds, size, and order checks into
one flag per token, take at least 3% off by BENCHMARKS rule 3.

Mechanism: `fill_document` reserves the most a text can hold and appends (no reallocation,
no zeroing); `valid()` folds the token checks into one flag per document and reads a token's
first byte only when a key prefix or terms entries ask for it.

Keep rule: byte-identical segments, tests green (every token rejection still rejects), and
the A/B by BENCHMARKS rule 3 on one platform without a loss on the other. Otherwise removed.

Result: kept. Byte-identical segments; ctest 34 of 34, every token rejection in index_test
still rejecting. Six interleaved rounds at 100,000 documents:

| Platform | E21 / E20 | Against Tantivy | Peak RSS |
|---|---:|---:|---:|
| Gate VM (`~/inv/runs-e21`) | 1.052× (1.012–1.144) | 2.613× (2.561–2.847) | 326 → 282 MB |
| macOS (`runs/e21-ab`, the host loaded) | 1.048× (0.973–1.102) | 2.179× (2.025–2.251) | 412 → 353 MB |

The peak falls because reserved capacity that is never written is never touched; on the
gate VM strata's 282 MB is now under Tantivy's 291 MB at this sample. 4.5× is 1.72× away
on Linux.

### 5M confirmation (gate VM)

One round each on the whole corpus (5,032,104 documents), keyed, `STRATA_BUDGET_MB=2056`,
strata at `3f16e79` (E21), the page cache dropped first (`~/inv/runs-5m`); the host carried
other agents' compiles (load 10–14), which slowed both: Tantivy took 707 s here against 512 s
at the n1017 gate run.

| Engine | Wall | User + system | Peak RSS | Index bytes | Segments |
|---|---:|---:|---:|---:|---:|
| Tantivy-0.26-st | 707.2 s | 650.5 + 165.2 s | 11.64 GB | 3,066,216,363 | 1 |
| strata E21 | 279.0 s | 223.7 + 54.8 s | 3.70 GB | 2,800,784,369 | 3 |

strata is 2.535× Tantivy at 5M in one session (n1017's base was 0.47×), and 4.5× is 1.78× away
(157 s at this session's Tantivy). Three flushes and no merge: E19 and E20's density puts
1.7M documents in a flush at the gate's budget. A fifth of strata's time is in the kernel
(54.8 s), most of it the page faults of building a new buffer after each flush; the next
items are reusing the buffer's memory across flushes and the encoder.

### Measurement conditions from E23 on (2026-10-18)

The Mac's data volume is 98% full (21 GiB free) and wrote at 12 to 22 MB/s (`dd`, 02:00 to
03:00) while the gate VM's small-indexes fixtures wrote beside; E24's 100,000-document
profile on macOS spent 52% of its samples in `pwrite` (`runs/e24-prof.sample.txt`), and
`ab_index.py`'s cold-cache runs on the VM (caches dropped, the corpus read from the virtual
disk, which is a file on that volume) took 2.8x the day before's times. From E23's A/B on,
gate-VM A/Bs read the corpus from the page cache (`AB_WARM=1`) and write their indexes to
tmpfs (`AB_INDEX_ROOT=/dev/shm/inv-ab`), so the host's disk is out of every engine's times,
Tantivy's included. The VM also ran the fixture builds and an unheld `research_workloads`
process (load average 10 to 12), so per-round ratios spread by about ±8% and the intervals
below are wider than the effects they bound. macOS timing waits for the disk; macOS
instruction counts (`/usr/bin/time -l`, one run each at 100,000 documents) stand in as
evidence of work removed:

| Build | Instructions | Change |
|---|---:|---:|
| E21 | 14.312G | |
| E23 | 14.292G | -0.1% |
| E24 | 14.265G | -0.2% |
| E25 | 13.850G | -2.9% |
| E26 | 13.320G | -3.8% |
| E28 | 13.117G | -1.5% |
| E27 | 12.990G | -1.0% |

Two gate-VM sessions at 100,000 documents, each step against the one before it in the
same rounds (`.scratch/runs/vm-e28/runs.tsv`, six rounds; `.scratch/runs/vm-e30/runs.tsv`,
ten rounds):

| Step | Session 1 | Session 2 |
|---|---:|---:|
| E23 / E21 | 1.035 (0.995-1.082) | 1.056 (1.011-1.076) |
| E24 / E23 | 1.009 (0.995-1.040) | |
| E25 / E24 (session 2: / E23) | 1.020 (0.970-1.096) | 1.028 (0.985-1.070) |
| E26 / E25 | 1.041 (1.014-1.115) | 1.035 (1.000-1.073) |
| E28 / E26 | 0.998 (0.938-1.023) | |
| E27 / E28 (session 2: / E26) | | 1.033 (0.985-1.043) |
| E29 / E27 | | 1.004 (0.979-1.051) |
| E30 / E29 | | 1.008 (0.958-1.029) |
| Tantivy / last | 2.968 (2.823-3.229), E28 | 2.926 (2.844-3.163), E30 |
| Last / E21 | 1.133 (1.074-1.164), E28 | 1.151 (1.054-1.223), E30 |

Peak RSS at 100,000: E21 282 MB, E23 284, E24 276, E26 273. Two rounds at 1M (`.scratch/runs/vm-1m-e28/runs.tsv`)
ran while the load peaked (E21 took 39.0 and 47.9 s), so their times decide nothing; their
peaks do: E21 2,368 MB, E23 2,370, E24 2,314 (-2.3%), E26 2,265 (-2.1%).

macOS, once the disk wrote again (03:45, 875 MB/s): six interleaved rounds at 100,000
documents, every build of the series and Tantivy (`.scratch/runs/mac-e32/runs.tsv`; the
E25 to E30 builds carry E24, which E25's row's peak shows):

| Step | Time | Peak RSS (median of rounds) |
|---|---:|---:|
| E23 / E21 | 1.023 (1.011-1.041) | 328 → 336 MB |
| E25 (with E24) / E23 | 1.038 (1.022-1.064) | 336 → 311 MB |
| E26 / E25 | 1.018 (1.006-1.035) | 311 → 306 MB |
| E28 / E26 | 0.994 (0.974-1.010) | |
| E27 / E28 | 1.016 (1.002-1.024) | |
| E29 / E27 | 1.008 (0.885-1.017) | |
| E30 / E29 | 1.012 (1.000-1.169) | |
| E31 / E30 | 1.079 (1.069-1.091) | 305 → 283 MB |
| E32 / E31 | 1.040 (1.016-1.064) | 283 → 258 MB |
| Tantivy / E32 | 2.707 (2.660-2.851) | Tantivy 397 MB |
| E32 / E21 | 1.269 (1.214-1.279) | |

Decisions from both platforms (rule 3: an interval clear of 1.0 and at least 3%, on one
platform, without a loss on the other): E23 kept (a win on the VM, +2.3% on macOS, peak
+2.5% on macOS); E24 kept (peak -8% on macOS in every round, -2.3% to -2.8% on the VM, time
unchanged); E25 kept (a win on macOS, 1.02-1.03 on the VM); E26 kept (a win on the VM, +1.8%
on macOS); E27, E28, E29, and E30 removed (no win on either).

### E23: run entries encoded where their tuples are

Hypothesis: on Linux at 1M documents `put_entry` holds 11.8% of the samples, a third of them
on the loop that first walks each group's tuples: the drain encodes entries in term order,
which jumps across the batch's 20 MiB of buckets, so every group's tuples are a cache miss;
each entry's scratch and the run also zero-fill as they grow (`std::string::resize`).
Encoding each group's entry right after its bucket is grouped (its tuples still in cache),
into buffers that grow without zeroing, and then copying the encoded entries into the run in
term order (a quarter of the tuples' bytes) takes most of it away: at least 3% at 1M on the
gate VM by BENCHMARKS rule 3.

Mechanism: the drain groups a bucket, encodes each of its groups into a scratch area, sorts
all the batch's groups, and copies their encoded entries into the run's log piece in order,
placing marks and keys as it copies.

Keep rule: byte-identical segments, tests green, and the gate VM's A/B at 100,000 and 1M
faster by BENCHMARKS rule 3, macOS not slower. Otherwise removed.

Result: kept on Linux, macOS owed. Byte-identical segments on both platforms; tests green.
On the gate VM, 1.035× E21 in the first session (interval 0.995-1.082) and 1.056× in the
second (1.011-1.076), a win by BENCHMARKS rule 3 there; the 1M rounds decide nothing (above).
It removes no instructions (-0.1%): the gain is the tuples read while in cache. The copy into
term order costs 0.08 to 0.09 s of 3.2 s at 100,000 (the phase timers of an instrumented
build on the VM: grouping 0.42 s, encoding 0.52, the term sort 0.15, the copy 0.08, the
flush 0.89).

### E24: the positions spill in chunks

Hypothesis: the one-thread flush gathers the positions section in a `string_spill` until
the postings end; its `std::string` doubles, so it touches about twice the section's bytes
(every growth a fresh allocation and a copy of what came before) and, at its last growth,
holds the old and the new copies at once. At 1M documents the section is about 250 MB, so
gathering it in chunks that never move lowers peak RSS by at least 5% and costs no time.

Mechanism: `chunked_spill` keeps the section in chunks of 64 KiB doubling to 1 MiB, each
charged to the flush when allocated; `copy_to` writes them in order to the segment's file
and frees each once written. The index's one-thread flush uses it; the writer's own
`flush()` (merges, the K > 1 path) keeps `string_spill`.

Keep rule: byte-identical segments, tests green, peak RSS at 1M lower by BENCHMARKS rule 3,
and the 100,000-document A/B not slower. Otherwise removed.

Result: kept (`a4f1b40`), after a first removal on the gate VM's numbers alone. Byte-identical
segments; tests green. On the gate VM peak RSS fell 2.3% at 1M (2,370 to 2,314 MB) and 2.8% at
100,000 (284 to 276 MB), short of rule 3's 3%, with time 1.009× E23 (0.995-1.040); on macOS it
fell 8% at 100,000 in every round (333-343 MB to 308-312 MB), a win there.

### E25: the flush's merge keyed by 8-byte prefixes

Hypothesis: the merge of the runs at the flush (`merger::next`) holds about 10% of the CPU
at 100,000 documents on macOS (E24's profile, `runs/e24-prof.sample.txt`: 157 of 1,560
samples outside the file writes, 57 of them `memcmp` from comparing whole terms in the
heap), and it grows with the runs a flush merges (18 at 100,000, about 300 at 5M). Keying
each cursor by its term's first 8 bytes as a big-endian number, ordering equal terms by run
(so a term's entries leave the heap in run order, with no sort per term), and sifting the
top once per step instead of a pop and a push takes at least 3% off by BENCHMARKS rule 3.

Mechanism: the heap holds (prefix, cursor) pairs of 16 bytes; whole terms are compared only
when two prefixes tie, and runs break ties; each step replaces the top with its cursor's
next entry and sifts it down once.

Keep rule: byte-identical segments, tests green, and the A/B by BENCHMARKS rule 3 on one
platform without a loss on the other. Otherwise removed.

Result: kept. Byte-identical; tests green. 2.9% fewer instructions on macOS and 1.038× there
(1.022-1.064, with E24 in the build); on the gate VM 1.020× (0.970-1.096) and 1.028×
(0.985-1.070). Its effect should grow with the runs a flush merges (18 at 100,000, about 300
at 5M), which the 1M rounds could not show under the load.

### E26: run entries as one stream

Hypothesis: encoding a run entry writes four sections into scratch buffers and copies them
behind the header (`encode_entry`'s copies are 52 of E24's 1,560 CPU samples), and the four
section lengths cost about 3 bytes per entry (1.3M entries at 100,000 documents, 12M at
1M). One stream per entry, written once in place with the header put in front of it, takes
at least 3% off by BENCHMARKS rule 3 and makes the runs smaller.

Mechanism: per posting the stream holds the ordinal delta, wdf << 1 with a bit saying the
count differs, the count when it does, and the position deltas; the entry's header (term,
counts, flags, stream length) is written after the stream into room left before it, so
nothing is copied; a run ends in 8 zero bytes so the merge's prefix load stays inside it.

Keep rule: byte-identical segments, tests green, and the A/B by BENCHMARKS rule 3 on one
platform without a loss on the other, peak RSS not higher. Otherwise removed.

Result: kept. Byte-identical; tests green. 3.8% fewer instructions on macOS and 1.018× there
(1.006-1.035); on the gate VM 1.041× E25 (1.014-1.115) and 1.035× (1.000-1.073); peak RSS 276
to 273 MB at 100,000 and 2,314 to 2,265 MB at 1M (-2.1%).

### E28: entries decoded into arrays sized once

Hypothesis: `decode_entry` appends each value with a `push_back` (8.3% of E24's CPU samples
on macOS); sizing the term's arrays once from its entries' counts and writing through
pointers takes at least 3% off by rule 3.

Result: removed (`deb2ed2`). 1.5% fewer instructions on macOS, but 0.994× there (0.974-1.010)
and 0.998× E26 on the gate VM (0.938-1.023): the decode waits on its loads, not on the
appends.

### E27: the batch's terms sorted after a counting pass on two bytes

Hypothesis: `sort_groups` sorts about 72,000 terms a batch with `std::sort` (4.9% of E24's
CPU samples on macOS, about 4.4% on the VM); a counting pass on the first two bytes, then a
sort within each of those buckets (about a hundred terms each), takes at least 3% off.

Result: removed (`7bd8b67`). 1.0% fewer instructions on macOS and 1.016× E28 there
(1.002-1.024), under 3%; on the gate VM 1.033× E26 (0.985-1.043, with E28 inside it). It also
allocated 65,536 counters a drain, which a small index's flush does not need.

### E29: the key-prefix check by one `memchr`

Hypothesis: stage 1's `valid()` reads every token's first byte to check it does not begin
with the key prefix (4.6% of the VM's samples at 100,000 with its bounds loop); a token can
begin with it only where the prefix's first byte is in the text, so one `memchr` over the text
skips the loop for every document of the corpus, which has no `Q` in its texts.

Result: removed (`9a384c0`). On the gate VM 1.004× E27 (0.979-1.051), on macOS 1.008×
(0.885-1.017): the bounds loop that stays is most of `valid()`'s cost. E31's split documents
take the `memchr` instead.

### E30: the drain's copy fetched ahead

Hypothesis: the libc time under `drain` (6.2% of the VM's samples at 100,000) is E23's copy
reading the encoded entries in term order; fetching each entry 8 ahead (and its offset 16
ahead) takes at least 3% off.

Result: removed (`b024a86`). On the gate VM 1.008× E29 (0.958-1.029), on macOS 1.012×
(1.000-1.169); the phase timers then put the copy at 0.08 s of 3.2 s, so the copy was never
the 6.2%.

### THP through glibc (informational, no code)

`GLIBC_TUNABLES=glibc.malloc.hugetlb=1` on E30 at 100,000 documents, eight rounds on the
gate VM (THP in madvise mode): 1.064× (0.859-1.126), page faults 69,490 to 6,797, but one run
spent 3.2 s in the kernel compacting memory for huge pages (6.3 s wall), and peak RSS rose
from 274 to 335 MB. Not pursued as a setting; huge pages placed by the buffer itself, on
regions it fills whole, remain a candidate.

### E31: the text split inside the buffer

Hypothesis: the engine's front end and stage 1 hold about a third of the gate VM's time at
100,000 documents (E27's profile: `fill_document` 4.7%, `valid()` 4.6%, the buffer's token
loop and the copies around them), the research note's design budgets 16 ns a token for all
of it, and each token is touched three times before it is a tuple: the engine writes a
12-byte `text_token`, stage 1 reads it back for its bounds and order, and the buffer reads it
again to hash. A document that hands the index its text unsplit (`document::split_text`), the
buffer splitting the copy it already makes and hashing each word as it finds it, and stage 1
checking the text with one `memchr` for the key prefix, take at least 5% off by rule 3 (the
three token passes become one).

Mechanism: `split_words` (writer.hh) finds words, 8 bytes at a time, as the engine did;
`tuple_buffer::add_split` copies the text into its arena and splits the copy, hashing and
appending each word as a tuple at positions 1, 2, ..., and counts them as the length;
`index::valid` checks a split document's text (no word with the key prefix, none repeating a
terms entry; `tokens` must be empty); the multi-thread path, which takes explicit tokens,
splits a split document when it buffers it; `count_words` sizes the storage estimate
without splitting. `strata_engine` hands its texts unsplit. An additive field of the public
`document`, so it is a design question for the architect before it lands.

Keep rule: byte-identical segments (the split equals the engine's), tests green (split
documents equal to the same documents with explicit tokens on one and two flush threads;
every rejection of a word with the key prefix or repeating a term), and the A/B by rule 3 on
the gate VM without a loss on macOS. Otherwise removed.

Result: kept (`b7dd0f4`). Byte-identical; ctest 34 of 34. On macOS 1.079× E30 (1.069-1.091)
in every round, peak RSS 305 to 283 MB (-7%); on the gate VM, ten rounds at 100,000 with the
host quieter (load about 2; `.scratch/runs/vm-f32/runs.tsv`), 1.083× the kept series before
it (1.050-1.126), peak RSS 273 to 229 MB (-16%): the transaction's token arrays (a reserve
of up to half the text's bytes in 12-byte tokens per document) are gone.

### E32: the version map as one table of buffer indexes

Hypothesis: stage 1 keeps three node-allocating hash maps per buffered document
(`buffer_docids_`, the string-keyed `versions_`, and its reverse `docid_keys_`): three
allocations and two key copies a document, about 350 MB at the 1.7M documents of a 5M
flush, of which `map_bytes()` charges 194 MB, and about 3% of the gate VM's samples at
100,000 documents (lookups, rehashes, `_Hash_bytes`). Every entry describes a buffered live
copy, whose docid, version, and key the buffer already holds, so two flat tables of 16-byte
slots (docid to buffer index; key hash to buffer index, the key's bytes read from the buffer
to confirm) replace them, allocate only when they double, and are charged whole: peak RSS at
1M lower by at least 3% and time not slower, or faster by rule 3.

Mechanism: `index_detail::hash_slots`, open addressing at most half full, linear probing,
removal by backward shift; `key_slot()` finds a key's live copy, `set_key()` records the
previous index in the undo log, `remove_copy` reads the removed copy's key from the buffer
instead of a reverse map. Semantics are unchanged; `map_bytes()` counts both tables.

Keep rule: byte-identical segments, tests green (every key, version, replace, delete, and
rollback case, and a table test against `std::map` with forced hash collisions), and peak
RSS or time better by rule 3 on the gate VM without a loss on macOS. Otherwise removed.

Result: kept (`c62c902`). Byte-identical; ctest 34 of 34. On macOS 1.040× E31 (1.016-1.064)
and peak RSS 283 to 258 MB (-9%) at 100,000 documents; on the gate VM 1.084× E31
(1.060-1.138) and peak RSS 229 to 209 MB (-9%).

### The kept series on the gate VM (E21 to E32)

Ten rounds at 100,000 documents, warm corpus, tmpfs indexes, the host at load about 2
(`.scratch/runs/vm-f32/runs.tsv`): E23, E24, E25, and E26 together 1.115× E21
(1.053-1.130); E31 1.083×; E32 1.084×; all of it 1.299× E21 (1.243-1.328). strata at E32 is
**3.339× Tantivy** (3.257-3.603), 2.150 s against 7.175 s, at 209 MB peak RSS against
Tantivy's 291 MB. 4.5× is 1.35× away.

Three rounds at 1M the same way (`.scratch/runs/vm-1m-f32/runs.tsv`): Tantivy 115.6 s at
3,247 MB, E21 42.5 s at 2,368 MB, the E26-with-E24 series 35.6 s at 2,265 MB (1.170× E21,
interval 1.061-1.234), E32 33.3 s at 1,888 MB (1.171× that, 1.070-1.194): **3.470× Tantivy**
(3.228-4.418) at 42% less memory, one segment of 620,281,976 bytes in every run. 4.5× is
1.30× away at 1M.

### E33: each batch's terms sorted by LSD radix

Hypothesis: at E32 the term sort holds 6.9% of the gate VM's samples at 100,000 documents
(`std::sort`'s partitioning 4.2%, its insertion sort 1.3%, the rest in `sort_groups` and
`sort_run`; `perf-f32-100k.data`): about 72,000 16-byte keys a batch, each comparison a
branch that random keys mispredict. A least-significant-digit radix sort on the same key
(the term's first 8 bytes and its length class), 8 bits a pass with every pass's counts
gathered in one read and passes whose digit all keys share skipped, does it in nine linear,
branch-free passes: at least 3% by rule 3. (E27's counting pass on two bytes, which left
`std::sort` within each bucket, gained 1.6% to 3.3% and was removed.)

Mechanism: `radix_sort` in `sort_groups` for batches of at least 256 terms, then the runs
that tie on the first chunk with both terms past 8 bytes sorted on by `sort_run`, as
before; smaller batches keep `sort_run` alone.

Keep rule: byte-identical segments, tests green, and the A/B by rule 3 on one platform
without a loss on the other. Otherwise removed.

Result: kept. Byte-identical; tests green. Eight rounds at 100,000 documents on macOS
(`.scratch/runs/mac-e34/runs.tsv`; the last 1.5 minutes overlapped another engineer's strict
hold on macOS): 1.051× E32 (1.009-1.129), a win by rule 3; 1.9% fewer instructions and 3.5%
fewer cycles. On the gate VM, twenty rounds pooled from two sessions while another VM held
2.5 of the host's cores (`.scratch/runs/vm-e37-pooled/runs.tsv`; Tantivy's median took 8.3
and 13.5 s against 7.2 the hour before): 1.011× (0.982-1.047), no loss.

### E34: grouping into scratch, the bucket left as it is

Hypothesis: `group_run` counting-sorts each bucket's tuples into a scratch array and copies
them back over the bucket (20 bytes a tuple, 366 MB at 100,000 documents), and the scratch
and the tuples' ids are `std::vector`s whose `resize` zeroes whatever a bucket adds over the
one before (0.7% of the gate VM's samples in `_M_default_append` alone); the drain's libc
time (6% of the samples) is mostly these copies and fills. Since E23 encodes a bucket's
groups right after grouping it, the groups can point into the scratch, which grows without
being zeroed: at least 3% by rule 3, or removed.

Mechanism: `group_scratch`'s sorted tuples and ids are arrays that grow uninitialized
(every element is written before it is read); `group_run` takes its bucket as const and its
groups point into the scratch until the next call; `scan()` groups the undrained buckets
without copying them first.

Keep rule: byte-identical segments, tests green, and the A/B by rule 3 on one platform
without a loss on the other. Otherwise removed.

Result: kept. Byte-identical; tests green. On the gate VM once it was quiet (ten rounds,
Tantivy 6.37-7.34 s, `.scratch/runs/vm-e38/runs.tsv`): 1.031× E33 (1.018-1.051), a win by
rule 3; macOS 1.014× (0.991-1.053), the same instructions and 2.8% fewer cycles. (The noisy
twenty rounds before had 1.033×, 0.995-1.123.)

### E37: long groups' run entries in PFOR blocks

Hypothesis: a frequent term's postings in a batch (a group of hundreds or thousands of
tuples) are written to its run as one LEB128 value at a time (`encode_entry`, 11.7% of the
gate VM's samples at E32) and read back the same way at the flush (`decode_entry`, 6.9%),
about 4 to 7 ns a value each way, and LEB128 spends a byte on a gap PFOR packs in a few
bits. Writing a group of at least 128 tuples as columns (ordinal deltas, wdfs, counts,
position deltas) in bitpack's PFOR blocks of 128 with LEB128 tails, which the flush unpacks
with SIMD, takes at least 5% off by rule 3 and makes the runs smaller (the flush still
re-blocks each term's list for the segment, whose blocks span batches under format 12).

Mechanism: `encode_blocked` gathers a long group's numbers into four columns and packs each
(`bitpack::encode_pfor` per full block, LEB128 for the rest), flag 8 marking the entry;
`decode_blocked` unpacks them into the term's arrays (`bitpack::decode`), the ordinals by a
prefix sum. Groups under 128 tuples keep E26's single stream.

Keep rule: byte-identical segments, tests green (long groups with keys, terms given whole,
and counts apart from wdfs among the random documents), and time or peak RSS better by rule 3
on one platform without a loss on the other. Otherwise removed.

Result: removed. Byte-identical; tests green. On the quiet gate VM 1.016× E34 (0.980-1.036),
on macOS 1.018× (0.975-1.109) with 1.7% fewer instructions and 0.8% fewer cycles, peak RSS
-2.5%: no win by rule 3. With E37 the flush unpacks long groups with SIMD instead of reading
LEB128, but gathering them into columns at the drain costs about what it saves, and the
flush still re-blocks every list. The same holds for the larger version of the idea (each
frequent term's blocks carried from batch to batch so the flush appends them): with the
decode already cheap, the carry would save the flush's PFOR decode and encode only, about
3% by these numbers, for a split of `write_postings` and `write_positions` and per-term state.

### E38: tuples built in their bucket

Hypothesis: `push` is 7.7% of the gate VM's samples at E32, and its annotation puts 56% of
them on one 16-byte load and store: the caller builds each 20-byte tuple on the stack with
4-byte stores, then `push` (not inlined, with a six-register prologue) reloads it with one
16-byte load, which store-to-load forwarding cannot serve from several smaller stores, so it
waits for them to retire. Building the tuple in place in its bucket (`emplace_back` with the
fields) in an inlined `push`, with the bucket's growth out of line, removes the reload and
the call: at least 3% by rule 3.

Keep rule: byte-identical segments, tests green, and the A/B by rule 3 on one platform
without a loss on the other. Otherwise removed.

Result: removed. Byte-identical; tests green; `push` is inlined on both compilers. 1.011× E37
on the quiet gate VM (0.990-1.031), 1.008× E34 on macOS (1.005-1.015): the stall is real but
small next to the store's cache miss, which stays.

### E36: the harness's texts read in place, in transactions of 100 documents

Hypothesis: `strata_engine` applies 1,000 documents a transaction, so each document's text
(about 1.2 KB) is written by the parse, then read twice more (stage 1's `memchr`, the
buffer's copy) after 1.2 MB of other texts have passed through the cache; 100 documents a
transaction keep a transaction's texts in L2 between those passes, and the apply's fixed
cost per transaction is small; and the line itself is copied out of the read buffer
before its text is copied into the document, a copy that a view into the buffer avoids: at
least 3% by rule 3 for the two together. The harness's choice, not the engine's:
`STRATA_TXN_DOCS` (default 1,000) sets the transaction size for the measurement, and
`LineReader::next` gains a view form (a copy only for a line across two reads).

Keep rule: byte-identical segments, and the A/B by rule 3 on one platform without a loss on
the other; kept as the harness's default if so, otherwise the switch is removed.

Result: removed. Byte-identical. macOS, eight rounds (`.scratch/runs/mac-e38/runs.tsv`): the
view read 1.000× (0.995-1.010), transactions of 100 a further 1.016× (1.010-1.026), peak RSS
260 to 243 MB; under rule 3's 3% on both counts.

### 1M confirmation of the kept series (gate VM)

Three rounds at 1M documents, warm corpus, tmpfs indexes, the VM quiet (load about 2;
`.scratch/runs/vm-1m-h34/runs.tsv`), strata at `c6bf4d9` (E23 to E26, E31 to E34):

| Engine | Median wall | Peak RSS | Index bytes | Segments |
|---|---:|---:|---:|---:|
| Tantivy-0.26-st | 95.83 s | 3,247 MB | 682,628,217 | 1 |
| strata E21 | 36.73 s | 2,369 MB | 620,281,976 | 1 |
| strata `c6bf4d9` | 27.43 s | 1,886 MB | 620,281,976 | 1 |

**3.494× Tantivy** (3.207-3.498) at 42% less memory, 1.334× E21 (1.227-1.339). 4.5× is 1.29×
away at 1M.

### 5M confirmation of the kept series (gate VM)

One round each on the whole corpus (5,032,104 documents), keyed, `STRATA_BUDGET_MB=2056`,
the page cache dropped before each run and the indexes written to the VM's disk, as in the
first 5M confirmation; the VM otherwise quiet (load about 2;
`.scratch/runs/vm-5m-h34/`):

| Engine | Wall | User + system | Peak RSS | Index bytes | Segments |
|---|---:|---:|---:|---:|---:|
| Tantivy-0.26-st | 883.7 s | 707.1 + 384.3 s (1.23 cores) | 11.62 GB | 3,066,216,363 | 1 |
| strata E21 | 321.2 s | 198.5 + 120.9 s | 3.70 GB | 2,800,784,369 | 3 |
| strata `c6bf4d9` | 192.2 s | 139.3 + 50.0 s | 3.33 GB | 2,799,742,768 | 3 |

In this session strata measured 4.597× Tantivy at 5M (E21 2.752×), not counted (below), and
the kept series was 1.671× E21: more than at 1M (1.334×) or 100,000 (1.299×), because the costs that grow with a
flush's size fell most (E25's merge over about 330 runs, E32's tables in place of three
hash maps of 1.8M entries, and the pages E24, E31, and E32 no longer touch: system time 121
to 50 s, minor faults 2.08M to 1.39M).

Not counted (the lead, 2026-10-18): one round each, and Tantivy took 883.7 s here against
512 s at the n1017 gate run (its 707 s of user time alone is more than that run's wall),
which marks the round as disturbed, most likely by a hung researcher process that held a
gate-VM core from about 01:28 until the lead killed it at about 07:10. With the page cache
dropped and 11.6 GB resident in a 16 GB VM, Tantivy also spent 384 s in the kernel and read
10.5 GB (the corpus and its merge to one segment), where strata read 8.3 GB and ends at three
segments without merging. Against n1017's 512 s, which is not the same session, the kept
series' 192.2 s would be 2.66×. The 5M row comes later, strata against Tantivy once, run by
the lead after the cut lands.

### Reusing a flushed buffer's memory (informational, for D17's question)

What keeping freed memory in the process would save, measured without code: the kept series
with glibc told to keep what it frees (`MALLOC_MMAP_THRESHOLD_=33554432`,
`MALLOC_TRIM_THRESHOLD_=68719476736`, `~/inv/keep-h34.sh` on the VM), against itself, at 1M
documents. At a 600 MB budget (several flushes and merges, two segments at the end) minor
page faults fall from 358,700 to 261,700 in every round (-27%); at 2,056 MB (one flush) from
523,700 to 488,500, and time does not move (0.931×, 0.910-1.116). The 600 MB times were
taken while other work loaded the VM (load average up to 31): a first session gave 1.201×
(1.160-3.477, one 168 s outlier), a second 0.928× (0.701-1.757), so they decide nothing. On
the quiet VM a fault costs about 5 µs (2 s of system time for 358,700), so at 5M (1.39M
faults, 50 s of system time) reuse is worth a few percent, not the 10% the loaded runs
suggested. The question for D17 stands with that size.

### What bounds the kept series against 4.5×

Phase timers of the kept series at 100,000 documents on macOS (an instrumented copy of
`c6bf4d9`'s tuple buffer, three runs, user time about 1.95 s and system 0.18 s of 2.4 to
2.7 s wall):

| Phase | Seconds | Share of CPU |
|---|---:|---:|
| Drain: grouping each bucket (`group_run`) | 0.25-0.28 | 13% |
| Drain: encoding each group's run entry (LEB128) | 0.30-0.33 | 15% |
| Drain: sorting each batch's terms, copying entries into term order | 0.13-0.15 | 7% |
| Flush: merging the runs and decoding each term's entries | 0.30 | 14% |
| Flush: `flush_from` (PFOR blocks, skip data, positions, the dictionary, the per-document sections, the writes) | 0.56 | 26% |
| Everything else: the harness's parse, stage 1, splitting, hashing, the tuples, the commit | about 0.5 | 25% |

Under format 12 a block of 128 postings spans batches, so every list is encoded twice: into
LEB128 runs at the drain (0.32 s), and from the decoded runs (0.30 s) into the segment's
blocks at the flush (most of `flush_from`'s 0.56 s). Holding the tuples to the flush instead
(the research note's C) avoids both but costs 20 bytes a token, about 850 MB at 100,000
documents against this series' 205 MB, and a flush every 250,000 or so documents at the
gate's 2 GB budget. What remains reachable without a format change is small: every item
from E27 on that did not survive was worth 1% to 3%.

With format 13's proposed copy-merge items 1 to 3 (a stored list base, tail-coded blocks
anywhere, the same for positions), each batch's run could hold its terms' final blocks: the
drain would encode blocks once (about what `flush_from` spends on them now, 0.4 s) instead of
LEB128, and the flush would merge runs and append pieces with no decode or second encode.
By these timers that is about 1.18 s of encoding and decoding becoming about 0.65 s, a
quarter of the CPU at 100,000 documents, which on the gate VM (3.43× at `c6bf4d9`) is about
4.5×.

### Reruns after the hung process (gate VM, exact-commit builds)

The lead found a researcher process that held a gate-VM core at 100% from about 01:28 to
07:10 with no hold, across every gate-VM A/B of E23 to E38. The keeps that rest on gate-VM
numbers alone are rerun on builds of the exact commits (`c21` `3f16e79`, `c23` `097d33f`,
`c25` `147be0e`, `c26` `297464f`, `c33` `0a5cabe`, `h34` `c6bf4d9`), ten rounds at 100,000
documents. The first rerun overlapped the scheduler research's 8-vCPU workloads (Tantivy
11.5-24.2 s) and decides nothing; the second (`.scratch/runs/vm-rerun2/runs.tsv`) started as
one of those finished (load average 59, falling to 2) and still ran 1.6× slow:

| Step | Rerun | Before |
|---|---:|---:|
| E23 / E21 | 1.085 (1.070-1.130), every round 1.03-1.22 | 1.035, 1.056 |
| E25 / E23 | 1.005 (0.949-1.051) | 1.020, 1.028 |
| E26 / E25 | 0.999 (0.897-1.053) | 1.041, 1.035 |
| E34 / E33 | 1.017 (0.973-1.085) | 1.031 |
| E37 / E34 | 1.012 (0.892-1.052) | 1.016 |

E23 holds. E26 and E34, kept on gate-VM wins alone, do not repeat them here, and neither
passes rule 3 on macOS (1.018×, 1.014×); a run on a gate VM with nothing else on it decides
them. E25 stands on its macOS win.

The deciding run (the lead's ruling beforehand: a keep needs the time win under rule 3):
the gate VM rebooted onto the tsc clocksource (it had read the clock through hpet, at 31 µs
a read, since 2026-10-17), a 15-minute strict hold the lead granted, twenty interleaved
rounds at 100,000 documents on the exact-commit builds, with Tantivy
(`.scratch/runs/vm-tsc/runs.tsv`; `clocksource.txt` there: tsc before and after, load
average 0.4 to 1.1). No row from before the reboot is used.

| Step | Time | Decision |
|---|---:|---|
| E26 / E25 | 1.012 (0.996-1.039) | removed (`96e1f92`) |
| E34 / E33 | 1.008 (1.000-1.026) | removed (`2db1c54`) |
| Tantivy / E34's build | 3.696 (3.587-3.833) | |
| Tantivy / E25's build | 2.930 (2.878-3.225) | |

E26's 3.8% fewer instructions and 2.1% lower peak RSS at 1M, and E34's 2.8% fewer cycles
on macOS, do not qualify under the ruling.

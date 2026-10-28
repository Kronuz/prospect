> Status: CURRENT (2026-10-28) `include/prospect/fielddict.hh`, `reader.hh`, `writer.hh`, `tuple_buffer.hh`, `merge.hh`, `searcher.hh` on `phase8/fields`; not landed.

# Fields

A term is a field and some bytes. The same bytes in two fields are two terms, the empty term is a
term, and nothing in the term space is reserved. This is F2 of strata's `docs/design/fields-and-keys.md`
(D1 to D5, D7, D9, D10, D14); this page says how the code does it.

## What a document says

```
document ──► field part (field ref, terms | text | tokens, host length)
         ├─► field part ...                               + the document's own length (optional)
         └─► key (the core's)
```

A document is a list of field parts. A part names its field by a reference into the transaction's
declarations (`field_ref::declared(i)`, resolved to an id the field table assigned) or by an id read
from the table the transaction was built against. Stage 1 (`inverted_plugin::validate`) resolves every
reference, refuses a part on the key field, refuses a field named twice, and checks the part's terms
and tokens as before. A term may be empty. The buffer stores ids: `inverting_buffer::add` takes the
transaction's resolution and rewrites each reference (`resolve`) before it tokenizes.

The document's length (D5) is the host's: `part::length` when given, else the sum of the parts'
lengths. Xapiand passes the sum of every wdf, its id term included, so `bm25_xapian` needs no per-field
statistics and no norms. A split text counts its words.

## Terms below the dictionary

Everything under the dictionary (the tuple buffer's runs, the merge, `flush_from`) handles one opaque,
sorted key per term: the field id's varint followed by the term bytes.

```
varint(field id)  =  1 byte below 240; else 0xF0+n, then n big-endian bytes   (D10)
key               =  varint(field id) ++ term bytes
```

The varint is order preserving and prefix free, so the keys sort by field and then bytes, and no
key is a prefix of another field's. Lead bytes 0xF5 to 0xFF, a long form below 240 and a leading zero
byte are refused (`get_field`), so equal ids have equal bytes. Only the dictionary writer and reader
split a key back into a field and a term.

## One `terms` section, two kinds of dictionary

```
terms      [own dictionary of field a][own dictionary of field b] ... [the pool]
term_index [statistics, 20 B][directory][own indexes ...][the pool's index]
```

A field with at least 256 terms in the segment (`own_threshold`) has a termdict dictionary of its own,
with an alphabet of its own. Every other field shares the pool, whose keys are the full key above.
Measured on Xapiand's own terms this is the smallest total on every corpus tried (design section 2):
one numeric field would otherwise put binary bytes into the alphabet of every text term.

The directory (`fielddict::builder`, `reader`) has an entry for each field with a dictionary of its own:

```
[own blocks bytes][entries][entry bytes][own index bytes]
[samples: one per 16 entries, 20 B: id, entry offset, blocks offset, index offset]
[entries: id delta, flags, terms, blocks, index  (LEB)][own indexes][pool index]
```

A lookup of (field, term) binary-searches the samples, decodes at most 16 entries, finds the field's
entry, and opens that dictionary on first use; a field with no entry is looked up in the pool under its
key. Opening a segment reads the directory's header and builds no per-field map; dictionaries open on first use (`format13_test`'s protected-pages open covers it for one field; T6's 49- and 2,100-field rows are not run yet).
`flags` is 0; the bits are reserved for norms and per-field statistics (D4), and a reader refuses any.

A dictionary needs its alphabet when it is built, so the writer is given a plan before the first
term: for each field, at most how many terms it will have and the set of bytes they use
(`fielddict::planner`). A flush plans exactly from its sorted keys. A merge plans from its inputs'
directories (`add_plan`), which overlap and may hold terms whose documents are gone, so the plan is
an upper bound: a field near 256 terms may get a dictionary of its own that a flush of the same live
documents would pool. Both are valid segments and answer the same.

## Reading

`inverted_plugin::reader::find_term(field, bytes)` and `term(field, bytes)` look a term up;
`scan_terms` calls `f(field, term, info)` in (field, term) order through `term_cursor`, which
interleaves the own dictionaries and the pool by field id; `scan_dictionary(field, from, f)` walks
one field. `check()` walks the whole directory, and verifies each sample against its entry.

## Searching by name (D7, D14)

Field ids belong to an index, and a search may span several (the shards of one Xapiand index). A query
names its fields; `segment_snapshot` carries the field table of the index its segment belongs to, and
each lookup resolves the name to that segment's id:

```
find_term(snapshot, "title", "x"):   id = snapshot.fields.find("title")
                                     key field? → keys reader (a key leaf: one ordinal, wdf 0)
                                     else       → segment.find_term(id, "x")
```

`term_table`, `snapshot_df` and `multi_snapshot::df` take `(field name, term)`. A shard that does not
know the field answers with no terms, and the statistics are summed by name, so two shards that
declared their fields in different orders answer as one index (T5b). A snapshot built without its
table throws instead of answering empty.

The key field is the one declared with role `key`. Its terms are the keys: a term query on it is a key
leaf over the `keys` reader, never a dictionary lookup, and a part naming it is refused at stage 1.

## Reordering and merging

A merge and the reorder's graph walk the inputs' cursors by the least (field, term), so the same bytes
in two fields are two terms in the graph, with their own degrees.

## What is checked

| Claim | Test | Killed by |
|---|---|---|
| D10 varint canonical, order preserving, prefix free; both writers agree | `fielddict_test` | order, long-form and threshold mutants |
| Own dictionary at 256 terms, pool below; every term found, in order | `fielddict_test`, `fields_test` | threshold, plan mutants |
| Same bytes in two fields; the empty term; host length | `fields_test` | length, reference mutants |
| Key field refused in a part; key leaves | `fields_test`, `format13_test` | key-field mutants |
| Flush, merge and buffer digest agree | `fields_test` | |
| Two shards, different declaration orders | `shards_test` (T5b) | sum-by-id mutant |
| Xapian parity over Xapiand's bank, 49 fields | `parity_terms` (T3b) | |
| Graph terms by field | `reorder_test` | bytes-only mutant |
| A damaged directory fences | `fuzz_fielddict` | |
| Sizes against flat terms | `prospect_engine fields` (T1) | |

`docs/status/fields.md` has the numbers.

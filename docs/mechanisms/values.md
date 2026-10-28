> Status: CURRENT (2026-10-27) how the values plugin works; decisions and evidence in `docs/status/values-bytes.md`.

# Values

A document can carry opaque byte values by 32-bit slot. prospect never interprets them: the
host's codec (Xapiand's `sortable_serialise` strings and `StringList` blobs today) fills and reads
them. The values plugin is the fourth entry of `search_layout`, with its own section `values`
and structure bit `values`.

```
 document.values  ──▶ inverting_buffer ──▶ values_builder ─┐
 (slot, bytes)*        (kept beside the                     ├─▶ values section
 merge inputs' columns ─────────────────▶ merge ───────────┘     columns, directory, count
```

## The section

Columns back to back in slot order, then the slot directory, then its length:

| Part | Holds |
|---|---|
| Column, per slot | A `docvalues.hh` column of kind `bytes`, single-valued: the sorted dictionary of the slot's distinct values (memcmp order), a dictionary ordinal per document that has a value, a frozen presence set when some documents have none, skip groups of 4,096 documents, statistics |
| Directory entry, 32 bytes | `u32 slot`, `u8 kind` (3), `u8 flags` (0), `u16 0`, `u32 documents with a value`, `u32 0`, `u64 offset`, `u64 size` |
| Count | `u32` slot count |

A segment of documents without values holds the 4-byte section (count 0). A segment configured
without the `values` bit holds no section and answers "no values".

At open the reader checks the directory alone, in O(slots): the count fits, the directory's region
checksum, slots strictly increasing and below 0xFFFFFFFF, kind and flags and reserved fields, a
document count within the segment's, columns contiguous from offset 0 with nothing left over. A
column opens on first use (once, from any thread): its region checksums, then `column_view::open`'s
validation of the whole column, then the column's ordinal count equal to the segment's and its
document count equal to the directory's. Damage is `corrupt_segment`.

## Reading

```
reader.value(ordinal, slot)        -> optional<string_view>  (a view into the segment)
reader.get(slot)                   -> column*  (null: no document here has the slot)
column.value(ordinal), has(ordinal), documents(), distinct(), first(), last()
column.begin()                     -> cursor: next(), advance(target), doc(), value()
reader.each_value(ordinal, f)      -> f(slot, bytes) for each of the document's values
```

`documents()` counts deleted documents too, as docvalues statistics do; `first()` and `last()`
bound the live values. `cursor` visits documents with a value in ordinal order.

## Writing

`values_builder` takes `begin()` per document and `add_value(ordinal, slot, bytes)` per value (once
per slot and document, a slot's documents in increasing order). The inverter path keeps a
document's values beside the tuple buffer's columns, only for documents that have some, and hands
them to the builder at the flush in the flush's order. At the flush each slot sorts its distinct
values into the dictionary, maps each value to its ordinal, and streams its column.

## Merge

A merge rebuilds the section a slot at a time: each input's column is read in ordinal order, the
live documents' values keyed by their new ordinals and sorted, then the column is written. Values
of deleted documents are dropped, and a dictionary holds only live values. The output is the bytes
of one flush of the live documents. Order is the merge's (docid order or a reordering).

## Digest

One record per value of a live document: tag `V`, `u64 docid`, `u32 slot`, `u32 length`, the
bytes. Segmentation does not change it; buffered documents digest as their flushed form.

## Limits

A value and a document's values together are at most 64 MiB; slot 0xFFFFFFFF is reserved for
`index_sort`'s key component. Stage 1 rejects values out of slot order, duplicates, the reserved
slot, and anything over the limit.

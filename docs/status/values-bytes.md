> Status: CURRENT (2026-10-27) P1 of xapiand-on-strata: the values plugin. Branch `phase4/values-bytes`, worktree `prospect-wt/values-bytes`.

# phase4/values-bytes ledger

Workstream: prospect values plugin (xapiand-on-strata P1, strata `docs/design/xapiand-on-strata.md`
D3 and the step table; Q2 (a): opaque `bytes` slots, typed later). Developer agent.

## State

Implementation written and building; evidence below fills in as it lands. Format change: a
decoder-safety review follows before landing.

## Done criteria

From the step row: `bytes` column per slot, slot directory per segment, flush, merge, open, check,
digest record, reader (`value(ord, slot)`, presence iteration), fuzz target, fixtures, pre-freeze
bump. From the lead: decoder safety on arbitrary bytes (fuzz target); a merge keeps every value of
live documents and drops deleted ones; a revert check per bug fixed; sanitizers (ASan+UBSan, TSan);
`docs/mechanisms/values.md`; CI green on every job; a screen A/B that indexing without values is
unchanged.

## Decisions (shapes the design leaves open)

1. **Column layout.** One docvalues `bytes` column per slot (kind `bytes`, single-valued): a sorted
   dictionary of the slot's distinct values (memcmp order, as D3 says) and per-document dictionary
   ordinals, with presence, skip groups and statistics from `docvalues.hh`. No new codec. Typed
   and multi-valued columns reuse the same directory later (kind and flags bytes are reserved and
   must be 3 and 0 today).
2. **Slot directory.** In the section's tail, after the columns: 32 bytes per slot (slot, kind,
   flags, documents with a value, column offset and size), then the slot count. Columns are
   contiguous in slot order, and the directory must account for every byte; a reader checks all
   of that at open (cost O(slots), no column opened). Recommendation taken: the directory is a
   tail like `stored`'s index, not a separate pinned section, so a segment with values has
   one new section and a segment without them (flag set, no values) a 4-byte one.
3. **Structure bit.** `values` (bit 16) is configured by default like the other readable bits, so
   every segment of a default index carries a values section, 4 bytes when empty. An index
   configured without the bit refuses documents with values at the flush (`refuse`), as `stored`.
4. **Reserved slot.** 0xFFFFFFFF is `index_sort`'s key component (`format::key_slot`) and is
   refused. Slot 0 is an ordinary slot.
5. **Empty value is a value.** A document that sets a slot to empty bytes has it present. The
   seam decides whether Xapian's "empty means unset" applies, before it reaches the plugin.
6. **Limits.** A value, and a document's values together, are at most 64 MiB
   (`format::max_stored_record`): the flush's per-document working memory is sized for a stored
   record of that size. Xapian bounds values by memory only; Lucene's binary doc values stop near
   2 GB per value and its sorted ones at 32 KiB, so this is above Lucene's sorted-set limit and
   below its binary one. Revisit if Xapiand's geo or StringList values exceed it (census: values
   are tens of bytes).
7. **Document part.** `values_plugin::part` is a vector of `{slot, bytes}` strictly increasing by
   slot; stage 1 rejects anything else. `part::set(slot, bytes)` keeps the order and replaces
   (Xapian `add_value`).
8. **Presence iteration.** `column::cursor` walks documents with a value in ordinal order with
   `next()` and `advance(target)`, over the skip groups and the presence set. docset's
   `frozen_view` has `contains` and `rank` but no select or next, so the cursor tests ordinals
   one by one inside a group's span: a very sparse slot in a very large segment costs the span.
   A `next(doc)` on `frozen_view` would remove it (docset follow-up, not done here).
9. **Merge.** Rebuilds a slot at a time from the inputs' columns (live values keyed by new
   ordinal, sorted) and streams the section; it does not copy a column verbatim. A copy path for
   a single source without deletes is a later optimization.
10. **Core change (strata).** `values` had no file rank (`unranked`), so a streamed values section
    was refused. Strata branch `phase4/values-rank` (from the pinned 75d1e7e) gives it rank 7
    (after `stored`, before `docids`) and bumps `pre_freeze` to 0x8000000B. Prospect repins to
    that commit; the strata branch must merge before this one.

## Done

(see Evidence as items land)

## Evidence

## Next

## Blocked on

Nothing.

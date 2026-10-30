> Status: REJECTED (2026-10-28) P2 term lists are abandoned (R-P2; ledger commit prospect:fc1a88d on phase4/term-lists); the code stays on that branch.


# Term lists

A term list is one document's terms, each with its wdf, in term order. Xapiand reads them to show
a document (`INFO`'s term details, `_terms`) and to aggregate over a field's terms
(`DocView::terms(prefix)`): questions that go from a document to its terms, where the postings go
from a term to its documents.

```
postings   term  -> (ordinal, wdf) ...        (the inverted plugin)
term list  ordinal -> (term, wdf) ...         (this plugin: the transposition)
```

## What is stored

The `term_lists` section (kind 16, structure bit 64, file rank 8 after `values`) is strata's frame
layout, the one `stored` uses: records by ordinal in compressed frames, a frame index, a frame
count. A record is:

```
(empty)                                   a document with no terms
[count LEB]                               at least 1
  per term, in strictly increasing byte order:
  [shared LEB][suffix length LEB, at least 1][suffix bytes][wdf LEB]
```

`shared` is the length of the prefix the term has in common with the previous one; the first term
shares nothing. A record has no other bytes. Positions are not stored; they stay in `positions`.
A key is not a term (F8), so no list holds one.

The bit is opt-in: an index builds term lists only when created with
`index::options::opt_in` holding `format::bit::term_lists`, and a `segment_writer` only when its
`structures` has the bit. It requires `inverted`: a segment with the bit and without postings is
corruption.

## Writing

The lists are derived from what the segment's inverted builder writes. At the flush, the builder
reports each term as it writes it, in term order, with its ordinals and wdfs
(`term_lists_builder::observe`, linked by strata's `link` hook, which hands a builder the writer's
other builders). Every way a document gets terms is covered, because the report is made where the
postings are: explicit terms, tokens, split text. At `finish`, the postings are counting-sorted by
ordinal (stable, so each document's entries stay in term order), each document's record is
front-coded and handed to the frame builder, and the frames are written.

Working memory is 16 bytes per posting plus the distinct terms of the segment, charged to the
flush's budget; then the encoded records, as `stored`'s are.

## Merging

A merge copies records. For each live document in output order the merge reads its record from the
input (one decoded frame cached per input; spilled once when the output order is not the inputs'
order, as `stored` does) and adds it to the output's frame builder. A deleted document has no
place in the output order, so its list is dropped. Term lists need no term remapping, because a
record holds the term bytes, not ids. The code is `strata::detail::merge_frames`, shared with
`stored`. A merge builds the section only when every source holds it.

## Reading

```
reader.list(ordinal, frame_cache)  -> term_list   (a view; valid until the cache decodes another frame)
term_list.size(), empty(), begin(), prefixed(prefix)
cursor.next(), skip_to(target), term(), wdf()
```

`next()` decodes one term and checks it: sharing within the previous term, a non-empty suffix inside
the record, a term past the previous one, a wdf within 32 bits, the count and the bytes agreeing.
Damage throws `corrupt_segment`. A prefix cursor skips terms before the prefix and ends at the
first term after it, so it reads the list from its start to the end of the prefix's range.

## Checking

`TermListsPlugin::check` decodes every frame, requires the frames to cover the ordinals exactly,
reads every list whole, and counts documents, terms and bytes. `prospect::check_segment` then
cross-checks against the postings: the lists' (ordinal, term, wdf) triples and the postings' agree
in count and in an order-independent sum of a hash. The fuzz target `fuzz/fuzz_term_lists.cc`
mutates record bytes directly and the section inside base segments.

## Limits

1. A list is at most `max_stored_record`; the flush refuses a document past it.
2. A prefix read is sequential in the list. A list of tens of thousands of terms costs a scan to
   reach a late prefix; restart points are a later, format-changing addition.
3. strata's `Index::check` does not cross-check the lists against the postings; prospect's
   `check_segment` does.

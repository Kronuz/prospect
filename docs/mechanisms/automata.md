# Term expansion by automata

A prefix, wildcard or fuzzy query names a set of terms by a pattern: every term of a field starting
with `obam`, every term `ob?ma` accepts, every term within two edits of `obama`. prospect resolves
the set per segment by walking an automaton over the field's term dictionary, merges the segments'
sets under Xapian's limit types, and fills the query's `term_table` with each term's info, so the
scorers (`synonym_scorer`, term scorers, `multi_phrase_scorer`) take the terms as they take any
other. The decisions are `docs/design/automata.md` (D1 to D12); the automata are
[Kronuz/automata](https://github.com/Kronuz/automata) (its `docs/design.md`).

```
query ─> automaton ─┬─> segment 1: walk(field cursor) ─> (term, info) ...─┐
 prefix | glob |    ├─> segment 2: walk(field cursor) ─> (term, info) ...─┼─> merge, limit ─> term_table
 levenshtein        └─> segment n: ...                                    ┘   (live terms only)
```

## The pieces and where they live

| Piece | Where | What |
|---|---|---|
| `prefix`, `glob`, `levenshtein` | `automata.hh` | Character automata over plain-word states: a byte range, a bitset of pattern positions, Ukkonen's band of the edit-distance table (with transpositions) |
| `matcher` | `automata.hh` | Runs an automaton over keys, resuming at the bytes a key shares with the previous one; on a dead key, the dead prefix and the smallest later key that can still match (the seek target) |
| `walk` | `automata.hh` | Drives a matcher over any cursor with `seek`, `next` and `key`, seeking to each target; pauses every `check_every` entries |
| `cursor::seek(from, visit)` | termdict | A seek that shows the values it passes |
| `field_cursor` | `fielddict.hh`, `reader.hh` | One field's terms, own dictionary or pooled range, seekable, each value decoded as it passes |
| `intersect`, `expand` | `expand.hh` | The walk over one segment; the merge with limits and liveness |
| `term_table::put`, hashed columns | `searcher.hh` | The expansion's terms become columns without a second lookup |

The automata know nothing of dictionaries and termdict nothing of automata: the walk meets the
dictionary only through its cursor.

## One segment: the walk

`intersect(snapshot, field, automaton, f)` resolves the field's id in the segment's table. For an
ordinary field it opens a `field_cursor`: termdict's cursor over the field's own dictionary when the
field has at least 256 terms in the segment, otherwise over the pool, where keys are the field id's
varint followed by the term and the cursor strips the varint and ends at the next field.

`automata::walk` seeks the cursor to the automaton's literal prefix (`ob` for `ob?ma`, nothing for
`obama~2` without a fixed prefix) and stops at the prefix's successor. On each key the matcher
answers match or not; on a dead key it gives a target, and the walk seeks there unless the target is
the key's immediate successor. A fuzzy query without a fixed prefix therefore visits the terms near
the live paths of the edit table, not the field.

Term infos are run-relative: a term's postings and positions offsets follow its restart run's
earlier entries unless it is a restart (`format.hh`, `term_run`). The cursor decodes every value it
passes, without impacts, to keep the run's offsets current, and termdict's `seek(from, visit)`
shows the values a seek walks over inside the landing's run, so a seek decodes nothing twice and a
match's info (`info()`, impacts included) costs one decode, never a second dictionary lookup. A
match whose df exceeds the segment's documents is corrupt, as `find_term` reports it.

The key field has no seekable cursor: its source restarts `keys_plugin`'s scan at each seek target,
and a key is a leaf of one ordinal (df 1, no wdf, no positions), as `find_term` builds it.

## The snapshot: merge, limits, liveness

`expand(snapshots, field, automaton, limit, terms)` walks the segments in turn into one ordered map
of distinct terms, each holding its info from every segment that has it.

- **Liveness.** On a segment with deletes, a matched term not yet live walks its postings to the
  first live ordinal (a single posting's ordinal is inline). A term live in no segment is never
  selected and counts toward no limit; a selected term keeps every segment's info, live or not, so
  its df counts deleted documents as every statistic does.
- **`error`** throws `expansion_error` the moment the distinct live terms pass `max`.
- **`first`** keeps the first `max` live terms in term order: when one more arrives, the largest is
  dropped, and the largest kept becomes the cutoff every later walk stops at, so every term up to
  the final cutoff was seen by every segment.
- **`most_frequent`** walks everything, then keeps the `max` terms of highest snapshot df, ties by
  term order, returned in term order.
- `max` 0 is no limit; an empty expansion matches nothing.

The selected terms go into `term_table::put` with the infos the walk decoded. `term_table` hashes
its columns by (field, term), so an expansion of thousands of terms adds them in linear time.

## Budget and cancellation

Every `check_every` entries (1,024) the walk reads the query's cancellation token, throwing
`task::cancelled`, and asks `pause()`, which returns true to stop: `intersect` then returns the key
to resume from, and a new call from it rebuilds the matcher, which is correct for any key order.
`expand` resumes at once; a caller that yields between slices drives `intersect`. `charge` is
offered the matcher's recorded states and each new term's bytes before they are allocated, and
throws to refuse them.

## Tests

- `automata`'s `test/automata_test.cc` and `fuzz/fuzz_automata.cc`: every automaton, the matcher,
  its seek targets and the walk against brute force, over sorted vectors and termdict.
- termdict's seek test: every seek's visits end with `find_run`'s for the landing.
- `test/expand_test.cc`: keyed indexes of several segments with deletes and replacements, the text
  field in own dictionaries, a tag field pooled and the key field; per segment, `intersect` equals
  the dictionary filtered by the reference predicates with each info equal to `find_term`'s, also
  paused and resumed; `expand` equals the terms of the model's live documents, with each limit
  type, cancellation and charges; a term whose df exceeds the segment's documents is corrupt.

## Numbers

<pending: tools/expand_bench over the 3,964,752-term Wikipedia vocabulary>

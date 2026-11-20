# Phrases and NEAR over expanding words

A word of a phrase can be a pattern: `quick bro*` (a prefix), `qu?ck brown` (a wildcard),
`quick~1 brown` (an edit distance). Each such word expands once over the snapshot's term dictionary
through `expand.hh` ([term expansion](automata.md)), and the phrase matches a document where the
words occur at their relative positions, each met by any term of its expansion. The matching is
`multi_phrase_scorer` ([phrase alternatives](phrase-alternatives.md)); this mechanism supplies its
slots and bounds them. Elasticsearch's `intervals` (prefix, wildcard, fuzzy rules) and
`match_phrase_prefix`, and Tantivy's and Quickwit's phrase prefix queries, answer the same question.

```
words ─> per word: literal | expand(prefix | glob | levenshtein) ─> terms, in term order
         bounded by the word's limit and by what the query has left (max_expansions)
      ─> per slot: one weight for the snapshot (OR: a weight per term; SYNONYM: one at the union's df)
      ─> per segment: slot scorers + the terms' infos ─> multi_phrase_scorer(window, offsets)
```

## The pieces

| Piece | Where | What |
|---|---|---|
| `phrase_word` | `phrase_expand.hh` | A literal term, or `starts_with`, `wildcard`, `similar` (edit distance, fixed prefix, transpositions), with its own `expansion_limit` |
| `expanded_phrase` | `phrase_expand.hh` | Expands every word once over the snapshot at construction; `terms()` shows each slot; `scorer(seg, window, offsets)` builds the phrase in one segment; `builder()` is the `scorer_builder` of `search` |
| `phrase_options` | `phrase_expand.hh` | `max_expansions`, the walks' `walk_options` (cancellation, slicing, byte budget), `slot_scoring`, the weight function |
| `phrase_expansion_error` | `phrase_expand.hh` | Past a bound: the word's index, the bound and which one (`per_query`) |
| `multi_phrase_scorer` | `operators.hh` | Phase one: the documents every slot holds; phase two: positions line up, consecutive, at `offsets`, or NEAR within `window` |

NEAR gives each slot a distinct position, as the landed `multi_phrase_scorer` does: `qu*` NEAR `q*` does not
match a document with one "quick". Lucene's unordered intervals let different sources overlap and match it.

## Bounds

The expansion is where a phrase can blow up: a slot of N terms is N postings and N position cursors
in every segment. Two bounds, both an error, never a silent truncation:

- **Per word**: the word's own `expansion_limit`, Xapian's three types. `error` throws past `max`;
  `first` keeps the first `max` terms in term order, `most_frequent` the `max` of highest snapshot df
  (ties by term order). `max` 0 is no bound.
- **Per query**: `max_expansions`, the terms all words select together. A word's expansion gets what
  the query has left; a literal word is not an expansion and costs nothing, a word that matches nothing
  costs nothing. Past it the phrase throws, whatever the words' own types: under the query's
  bound a word is expanded as `error` at what is left, so the walk stops at the first term past it.
  The bound counts the snapshot's distinct live terms, not a segment's (Lucene's intervals and
  Tantivy bound per leaf, so a pattern with 100 terms in each of 3 segments passes their 128 and
  fails ours).

A refused phrase names the word (`phrase word 2 expands past the query's max_expansions of 50
terms`). When both bounds are past at one word, the tighter is named, the word's on a tie. The walk
stops at the first term past an `error` bound, so a refused phrase costs the entries up to it, not the
dictionary (`expansion_bounds_test` counts the entries). The byte budget, `walk_options::charge`,
is offered what the phrase holds: the walks' automata, matchers and terms when it expands, and in
`scorer()` each segment's scorer before it is allocated (per slot term, its copy of the term's
info, the cursor's postings iterator and positions reader, and the slot scorer's iterator and
copies), summed across segments, so a snapshot of many segments costs per segment. The budget
counts bytes offered and never gives any back. Cancellation reaches the phrase through
`walk_options`; `pause()` bounds how often a walk checks, and `expand` resumes a paused walk at
once, so a caller cannot yield between slices of one phrase's expansion.

Defaults are no bound, as Xapian's `max_expansion` 0 is: a number of ours could be smaller than one a caller needs (every cap names its mechanism and is at least the largest of the others'). The host sets the numbers it can afford; the byte budget bounds the expansion and the scorers' construction until it does, not what a search reads and decodes afterwards.

## Scoring

A document scores the sum of its slots' scores, each as it scores alone (`multi_phrase_scorer`). The
weights are computed once per slot over the snapshot, never per segment, so a document scores the same
in whichever segment it lives:

- `slot_scoring::synonym` (the default; the expansions combine as SYNONYM, ARCHITECTURE): one weight at
  `synonym_df` of the slot's terms' snapshot dfs, `tf` the summed wdfs capped at the length.
- `slot_scoring::any`: each term its own weight at its own df, summed.

`phrase_options::weigh` replaces the default weight (`bm25_xapian` over the table's statistics).
dfs count deleted documents, as everywhere; the terms selected have a live document (term expansion D6).

## Why this shape

- One expansion, one matcher. The walk, the limits, liveness, cancellation and budgets are
  `expand.hh`'s; the positions logic is `multi_phrase_scorer`'s. Nothing here walks a dictionary or
  reads a position.
- Expanding at construction, once for the snapshot, gives every segment the same slots and weights
  and lets the bound apply to the query, not to a segment.
- A slot with no term in a segment makes `scorer` return null: the segment matches nothing.
- The key field takes no phrase (its terms have no positions): `std::invalid_argument`. A field
  indexed without positions matches nothing, as a plain phrase does.

## Proof

`test/phrase_expansion_test.cc`: 2,080 random phrases over four worlds (several segments, replaced and
deleted documents, own and pooled dictionaries, terms held only by deleted documents) against a
brute-force evaluation from the model's tokens with automata's reference predicates: each slot's
terms, the matching documents position by position (consecutive, with offset gaps, NEAR), the counts,
each score under both slot modes, and the refusals (which word, which bound).
`test/expansion_bounds_test.cc`: bounds at, below and above the sizes the evaluation gives, for every
limit type, per word and per query; the tie rule; the walk's early stop; cancellation, the byte budget,
the key field. `fuzz/fuzz_phrase.cc` runs the same comparison on decoded inputs, its corpus replaying as
a test. `test/phrase_expansion_mutants.py` reverts the mechanism one line at a time (20 mutants) and
expects a test to fail. No timing: this note makes functional claims only.

## Code

`include/prospect/phrase_expand.hh`.

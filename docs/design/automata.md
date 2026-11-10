# Automata: evaluating strata's `phase5/automata` and where it goes

Revision 1 (2026-11-09), for quest `automata-evaluation` (owner ruling D37). It evaluates strata's
`phase5/automata` at strata:219ded0d (2026-10-18): its design (strata `docs/design/automata.md`
revision 1), its preliminary review (strata `docs/reviews/automata-design.md`, AUT-R1 to AUT-R14,
APPROVE WITH CONDITIONS) and its ledger (strata `docs/status/automata.md`). The branch was written
two days before the 2026-10-20 split moved search to prospect; it cannot land in strata as it is.
The `automata` quest builds what this document decides.

Branch decisions are cited as strata-D1 to strata-D12; this document's own are D1 to D12. Clean
room as the branch was: Lucene and Tantivy from documentation, Xapian's semantics from its API
documentation and oracle vectors, no Xapian source and no GPL Xapiand file.

## Verdict

Port it. The automata and the sorted-key matcher (strata-D1 to strata-D6, `automata.hh`) move
verbatim to a library; the expansion (strata-D7 to strata-D9, `expand.hh`) is rewritten in prospect
over its per-field dictionary; Xapiand's own expansion (`src/engine/strata/expand.cc`,
`pattern.cc`) shrinks to the query mapping. The reason is in "Numbers": Xapiand's strata engine
expands an edit distance without a fixed prefix by testing every term of the field, and on the
3,964,752-term Wikipedia vocabulary the branch's matcher with seeks visits 0.03% to 0.8% of them.

## What the branch is

| File | Lines | What | Verdict |
|---|---|---|---|
| `include/strata/automata.hh` | 584 | strata-D1 to strata-D6: lenient UTF-8 characters, `prefix`, `glob` (bit-parallel NFA), `levenshtein` (Ukkonen's band, optional transpositions, a fixed prefix), the D5 interface with `next`, the resuming `matcher` with dead prefixes and seek targets, `intersect_sorted` | Port verbatim (D2) |
| `include/strata/expand.hh` | 394 | strata-D7 to strata-D9: `intersect` over main's `segment` with a run-cursor replay, `walk_options` (seek, cancellation, pause, budget), `expand` over a snapshot with Xapian's limit types and a liveness rule, `or` and constant-score builders | Rewrite in prospect (D3 to D7) |
| `test/automata_test.cc`, `test/automata_reference.hh` | 374, 71 | Brute force: every {a, b} string to length 10, Unicode and invalid bytes, every 2-, 3- and 4-byte cut, globs past one state word, k 0 to 3, matcher depths 0 to 4,096, seek targets against every skipped key, matchers on threads | Port verbatim |
| `fuzz/fuzz_automata.cc` and 27 corpus files | 124 | Matcher and seeks against brute force; an intersection over a segment, also with input-chosen byte flips | Port; its segment half over prospect's dictionary |
| `test/expand_test.cc` | 478 | Intersection and expansion against `scan_terms` and `find_term`, limits over segments sharing terms, deletes, budget, cancellation, corruption | Rewrite with the expansion (D11) |
| `docs/design/limits.md` | +13 | strata-D11's rows | Move to prospect's limits (D3) |

The branch's decisions, as they stand after the split:

| Branch decision | Holds? | Why |
|---|---|---|
| strata-D1 Characters (`next_utf8`, aliasing, literal prefix up to U+0080 to U+00FF) | Yes | The decoding Xapiand's `pattern.cc` already uses; the check below finds the two equal on valid UTF-8 |
| strata-D2 Prefix as a byte range | Yes | Xapiand's wildcard without pattern flags (`pattern.cc`, `wildcard_matches`) |
| strata-D3 Glob by simulation | Yes | Exact `can_match` and `next`; no determinization blow-up |
| strata-D4 Levenshtein by simulation, any k | Yes | The compiled DFA stays a measured alternative (Track P), not a gate row |
| strata-D5 One automaton interface with `next` | Yes | The owner's O6 (2026-10-17) |
| strata-D6 Resuming matcher, seek targets | Yes | What makes fuzzy cheap ("Numbers") |
| strata-D7 Intersection over `segment`, run replay, `seek_after` 64 | Changes (D3, D4, D5) | `segment` is gone; terms live in prospect's per-field dictionaries; termdict's `cursor::seek` landed |
| strata-D8 Expansion and limits, liveness rule | Mostly (D6) | Limits as Xapiand's; the liveness rule is an owner question (Q2) |
| strata-D8a Keys | Changes (D9) | Format 13 landed: keys are `keys_plugin`, scanned from a lower bound, as Xapiand does today |
| strata-D9 Scoring: `or`, constant score, one store | Changes (D7) | prospect has `synonym_scorer` and `multi_phrase_scorer`; Xapiand builds groups; the store is `term_table` |
| strata-D10 Where the code lives | Changes (D2, D3) | No strata home remains |
| strata-D11 Limits, strata-D12 Errors | Yes | Move with the code |

What it still lacks, from its ledger and review: CI was never green (one `expand_test` check, below);
no full review of revision 1; T4 (oracle parity: the oracle has no expansion mode), T6 (speed
against Lucene, Tantivy and Xapian), T7 (memory), T8 (test parity rows), T9 (the postings-tail
profile) never ran; the keys path waited for format 13; the seekable term cursor waited for layers.

## What the split moved

| The branch assumed | Now |
|---|---|
| `strata::segment::dictionary()`: one termdict, Xapian-prefixed terms | prospect `inverted_plugin::reader` over `fielddict`: a termdict per field with at least 256 terms, the rest pooled under a field-id varint (`include/prospect/fielddict.hh:1-14`) |
| `segment::find_term`, `scan_terms` | `reader::find_term(field, key)` and `scan_terms` (`include/prospect/reader.hh:364-387`); `scan_dictionary(field, from, f)` (`reader.hh:390-393`) |
| termdict's `cursor::seek` (O2) to be written | Landed (termdict:e4314a5), at strata main's pin termdict:d7f5954 (`termdict.hh:695`); it walks the landing restart run with an empty value visitor (`termdict.hh:717-718`) |
| A synonym scorer (O4) to be assigned | `synonym_scorer` (`include/prospect/operators.hh:62`), and `multi_phrase_scorer` for positions holding expansions (`operators.hh:399`) |
| Keys in the dictionary until format 13 | `keys_plugin` (strata `include/strata/core/reader.hh:246`); Xapiand scans it for the key field (`src/engine/strata/expand.cc:94-106`) |
| `segment_snapshot`, `term_table` in strata | prospect `searcher.hh`; `term_table::column` finds a term by a linear scan of its columns (`searcher.hh:184-187`) |
| `fst::map::intersect`, `fst::prefix_automaton` retired once the branch lands (O6) | Still in fst (fst:7338f56 `fst.hh:493`, `fst.hh:896`), with no caller in strata, prospect or Xapiand |
| string-similarity gains a code-point OSA distance (O5) | Not built; Xapiand's `pattern.cc` holds one (`within_edit_distance`) |

What Xapiand's strata engine does today (Xapiand `src/engine/strata/expand.cc:67-151`,
`pattern.cc`): for each field and segment it scans the dictionary from the pattern's literal bytes
while terms start with them, and tests each term: a backtracking glob, or a full optimal string
alignment table on the term's tail. Nothing seeks, so an edit distance without a fixed prefix, or a
glob with a leading `*`, tests every term of the field. The matches go into a `std::set`, then
`group_of` (`search.cc:182`) asks `term_table` for each one's df, a linear lookup per term.

## Numbers

<!-- numbers -->

## Decisions

### D1. Port

The branch is ported. Alternatives: keep Xapiand's scan (loses: it tests every term of the field
for fuzzy and leading-`*` queries, the 130 to 3,400 times slower class in "Numbers", and Phase 5's
speed row cannot pass on it); write a new implementation (loses: the branch's automata were approved
as right and well tested, and pass their tests under ASan and UBSan today); Lucene's compiled
Levenshtein tables (loses: their tables stop at k = 2; they stay strata-D4's measured alternative).

### D2. The automata go to a library, `Kronuz/automata`

`automata.hh` moves verbatim as `automata.hh` in namespace `automata`, header-only, depending only
on `unicode`, with `automata_test`, `automata_reference.hh`, `fuzz_automata` (its segment half
replaced by a termdict one) and the corpus. `intersect_sorted` stays as the reference walk; the
walk over a dictionary is D4's. Creating the repository is the owner's call (Q1).

Alternatives, from the catalog (`~/code/Kronuz`): `fst` holds the only other automaton concept,
byte-level and unused, which O6 retires; putting character automata there would give a
dependency-free map library a `unicode` dependency for an unrelated job. `termdict` must not depend
on automata (AUT-R12): the walk needs only its cursor. `string-similarity` holds ranking metrics,
not automata; `unicode` holds decoding. Keeping it in prospect works, since prospect is its only
consumer, but it is library-shaped code with no prospect dependency, and Phase 5's
`phrase-expansion` and a future completer reuse it; it is the fallback if Q1 says no.

### D3. The expansion goes to prospect, `include/prospect/expand.hh`

It knows prospect's dictionary, term infos, snapshots and `term_table`, so it lives beside them.
Xapiand keeps only the query mapping (D9). strata keeps nothing: the lifecycle core never sees
terms. The limits rows move to prospect's `docs/design` with the code.

### D4. The walk: the matcher over a per-field cursor, seeking on every dead key

For one field of one segment, the walk positions a term cursor at the automaton's literal prefix,
runs the matcher on each key, and on a dead key seeks to the matcher's target, unless the target
is the next key. This is strata-D7 without `seek_after`: with `cursor::seek` a seek re-decodes
nothing behind the cursor. Over the pool, keys carry the field's varint; the walk strips it, as
`fielddict::reader::scan` does (`fielddict.hh:339-361`).

The restarted-scan arm in "Numbers" (each seek ends `scan` and starts another at the target, the
shape `scan_dictionary` offers today) decides whether prospect needs a seekable cursor at all;
where it costs the same, the port uses `scan_dictionary` and adds no API.

### D5. Term infos: matched terms only, through `term_table`

The walk yields matching terms; their infos come from one dictionary lookup each, through
`term_table`, as every other term's do. The branch's run replay (strata-D7) decoded infos during
the walk; after a seek, a term's offsets follow its restart run's earlier values, which termdict's
seek walks without showing (`termdict.hh:717-718`), so a replay needs either a second walk of the
run or an additive `cursor::seek(from, visit)` in termdict. The lookup arm in "Numbers" measures
what the simpler shape costs; if a class pays more than a tenth for it, termdict gains the visitor
seek and prospect's term cursor decodes infos as it walks.

`term_table` gains a hashed index from (field, term) to its column: an expansion of E terms
otherwise costs E² string comparisons in `column` (`searcher.hh:184-187`; read from the code, not
measured).

### D6. Limits

Xapian's limit types, as Xapiand applies them today: `error` (Xapiand's `errc::invalid`), `first`,
`most_frequent` (ties by term order), `max_expansion` 0 for none. They move into the walk as
strata-D8 states: `error` stops a segment at max + 1 matches, `first` at max with a merged cutoff
across segments, `most_frequent` walks everything. A term whose every document is deleted counts,
as now (Xapiand `docs/mechanisms/strata-callbacks.md`, known difference 2), unless Q2 rules for
strata-D8's liveness rule.

### D7. Scoring hand-off: none from the branch

The expansion fills `term_table` columns for its terms; Xapiand's groups combine them as today
(`synonym_scorer` for `OP_SYNONYM`, term scorers for `OP_OR`, `MAX`), and `multi_phrase_scorer`
takes them inside phrases (D40's `phrase-expansion`). The branch's `or` builder duplicates the
groups and is dropped; constant score is built when a Lucene or Tantivy comparison with equal
semantics needs it (Phase 5's speed row), not before.

### D8. Budget and cancellation

The walk keeps strata-D7's hooks: every `check_every` entries (1,024, a preset) it reads the
query's cancellation and asks whether its slice is spent, returning the key to resume from; the
matcher's recorded states and the expansion's terms are charged to the query's byte budget. The
matcher is correct for any key order, so a resumed walk rebuilds it.

### D9. Xapiand's boundary

Xapiand's `expand.cc` keeps the field-name resolution (an empty pattern over every field whose
name starts with the node's) and maps the node to an automaton: no pattern flags is `prefix`;
with flags, `glob` with the bits mapped, since Xapiand's `PATTERN_SINGLE` is 1 and `PATTERN_MULTI`
2 (`src/engine/query.h:87-88`) and the branch's `glob::multi` is 1 and `single` 2
(`automata.hh:156`); an edit distance is `levenshtein(str, distance, prefix_length, true)`.
`pattern.cc`, `pattern.h` and `tests/fuzz_strata_pattern.cc` are deleted. The key field takes the
same matcher over `keys_plugin`'s scan from a lower bound; whether edit distances on the key field
stop being refused (`expand.cc:80-82`) is Q3.

### D10. fst's automaton concept is deleted

`fst::map::intersect` and `fst::prefix_automaton` are deleted when `Kronuz/automata` lands (O6,
the owner, 2026-10-17); nothing calls them.

### D11. The branch's red check

`expand_test.cc:376` expects some byte flip of a terms section, checksums refreshed, to reach the
"document frequency" check; none does, at the branch and since its rebase (job below). The port's
test does not flip bytes hoping to reach it: it builds a term value whose df exceeds the segment's
document count and asserts `corrupt("document frequency")` from prospect's checks
(`reader.hh:375`, `reader.hh:384`, `reader.hh:411`). Byte flips stay in the fuzz target, which must answer or throw.

### D12. The branch afterwards

Once the `automata` quest lands, `phase5/automata` is archived as D33 did (an `archive/` tag, the
branch deleted): its design, review and ledger stay readable as history there.

## Contracts

`automata` (library): strata-D5's interface unchanged (`words`, `start`, `step`, `is_match`,
`can_match`, `next`, `literal`, `byte_range`), the `matcher` with `seek()`, `successor`,
`intersect_sorted`; `charge_fn` for budgets.

`prospect:term-expansion` (the `automata` quest's contract):

```cpp
namespace prospect {
struct expansion_limit { enum kind { error, first, most_frequent } type = error; std::uint64_t max = 0; };
struct walk_options { std::size_t check_every = 1024; const task::cancel_token* cancel = nullptr; std::function<bool()> pause; automata::charge_fn charge; };
// Every term of `field` in one segment the automaton accepts, in order, until f returns false;
// returns the key to resume from when paused.
template <class A, class F> std::optional<std::string> intersect(const segment_snapshot&, std::string_view field, const A&, F&& f, const walk_options& = {});
// The snapshot's distinct matching terms, in order, after the limit; fills `terms`' columns.
// Throws expansion_error past an `error` limit.
template <class A> std::vector<std::string> expand(std::span<const segment_snapshot>, std::string_view field, const A&, expansion_limit, term_table& terms, const walk_options& = {});
}
```

## Checks

- This evaluation: the branch's tests at the branch, Release and ASan with UBSan (jobs below); the
  cross-check `automata_eval check` (appendix): the branch's automata over termdict, with seeks,
  equal Xapiand's predicates applied to every term, on random sets of valid 1- to 4-byte UTF-8,
  and three mutants caught; `automata_eval bench` for "Numbers".
- The `automata` quest's A1: `automata_test` and `automata_eval check`'s comparison, ported to
  prospect's dictionary (own and pooled fields, several segments, deletes); A2: `fuzz_automata`
  for a CPU-hour and the sanitizer builds; A3: Phase 5's speed rows, with `automata_eval bench`'s
  query classes as the smoke stage.

## Open questions for the owner

- **Q1. A public `Kronuz/automata` repository** for D2. Options: (a) yes, header-only, MIT,
  depending on `unicode`; (b) no, `include/prospect/automata.hh`. Recommended (a): it has no
  prospect dependency, and the branch's design and review recommended it once termdict's seek and
  strata-D5's `next` existed, which both do.
- **Q2. Terms with no live document** (strata-D8, AUT-R5). Today a term whose documents are all
  deleted fills `first` slots and counts toward `error` until a merge. Options: (a) keep that;
  (b) test each matched term's postings for a live document on segments with deletes, so such
  terms are never selected, as a Xapian database with committed deletes behaves. Recommended (b):
  it removes a documented difference from Xapian at the cost of one postings probe per matched term
  on segments with deletes; dfs keep counting deleted documents as everywhere else.
- **Q3. Edit distance on the key field.** Xapiand refuses it on the strata engine
  (`expand.cc:80-82`). Options: (a) keep refusing; (b) serve it with the same matcher over
  `keys_plugin`. Recommended (b): Xapian served it, and PLAN's D7 (R-RM, remove rather than refuse)
  forbids a lasting refusal of something the engine serves naturally.
- **Q4. string-similarity's code-point OSA distance** (O5). Options: (a) build it as decided;
  (b) drop it. Recommended (b): its consumers were Xapiand's `pattern.cc`, which D9 deletes, and
  the test reference, which keeps a private copy so it stays independent of what it checks.

## First buildable steps

1. `Kronuz/automata` (after Q1): `automata.hh` from strata:219ded0d, namespace renamed, its tests,
   fuzz target and corpus; CI green on every platform.
2. prospect: `expand.hh` with D4's walk over own and pooled fields, D6's limits, D8's hooks;
   `term_table`'s hashed index; tests against brute force over several segments with deletes.
3. Xapiand: `expand.cc` onto `prospect::expand` with D9's mapping; `pattern.cc`, `pattern.h` and
   `fuzz_strata_pattern.cc` deleted; the E2E wildcard and fuzzy suites unchanged.
4. fst: D10's deletion. strata: D12's archive.
5. The Phase 5 gate rows (T4's oracle expansion mode, T6, T7) in the `phase5` quest.

The port moves about 1,150 lines verbatim (the header, its tests and fuzz target), rewrites the
394-line `expand.hh` as prospect code of about that size, and deletes about 350 lines from Xapiand;
an estimate from the line counts above, not a measurement.

## Appendix: the harness

`automata_eval.cc` compiles the branch's `automata.hh`, Xapiand's `pattern.cc` (with a two-line
shim for `engine/query.h`'s flags) and termdict at strata main's pin. `run.sh check` runs the
cross-check and three mutants (one edit too many, Xapiand's flag bits passed unmapped, a seek one
byte past its target), each of which must fail; `run.sh bench` checks the vocabulary's SHA-256,
asserts all four arms return the same terms for every query, then times them interleaved, the
arm order rotating per run, reporting per-query medians. Each job extracts both files from this
document:

```sh
D=$PWD/.scratch/eval; mkdir -p "$D"
for f in automata_eval.cc run.sh; do
  sed -n "/<!-- begin $f -->/,/<!-- end $f -->/p" docs/design/automata.md | sed '1,2d' | sed '$d' | sed '$d' > "$D/$f"
done
bash "$D/run.sh" "$D" check 2000
```

<details><summary><code>automata_eval.cc</code></summary>

<!-- begin automata_eval.cc -->
```cpp
// automata_eval: strata phase5/automata's automata (strata:219ded0d) against the expansion Xapiand's
// strata engine runs today (Xapiand src/engine/strata/expand.cc and pattern.cc), over termdict.
//   automata_eval check [trials]          brute-force cross-check on random term sets (exit 1 on a mismatch)
//   automata_eval bench VOCAB RUNS [N]    the walks timed over a termdict of VOCAB's first column
#include "engine/strata/pattern.h"
#include "strata/automata.hh"
#include "termdict.hh"

#ifndef AUTOMATA_EVAL_MUTANT
#define AUTOMATA_EVAL_MUTANT 0
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace au = strata::automata;

struct query {
	std::string cls, str;
	bool wildcard = true;
	unsigned flags = 0;  // Xapiand's: PATTERN_SINGLE 1, PATTERN_MULTI 2
	unsigned k = 0;
	std::size_t prefix_len = 0;
};

struct stats {
	std::uint64_t visited = 0, seeks = 0;
};

// Xapiand's walk (expand.cc, one field of one segment): scan from the literal prefix, test each term.
std::vector<std::string> xapiand_walk(const termdict::reader& r, const query& q, stats& st) {
	std::vector<std::string> out;
	std::string prefix;
	std::string_view tail;
	if (q.wildcard) {
		prefix = engine::strata::wildcard_prefix(q.str, q.flags);
	} else {
		const auto lead = engine::strata::head(q.str, q.prefix_len);
		prefix = std::string(lead);
		tail = std::string_view(q.str).substr(lead.size());
	}
	r.scan(prefix, [&](std::string_view term, std::string_view) {
		if (!term.starts_with(prefix)) { return false; }
		++st.visited;
		const bool ok = q.wildcard ? engine::strata::wildcard_matches(q.str, term, q.flags)
		                           : engine::strata::within_edit_distance(tail, term.substr(prefix.size()), q.k);
		if (ok) { out.emplace_back(term); }
		return true;
	});
	return out;
}

// The branch's walk (D6, D7 with termdict's cursor::seek): the matcher over the literal range,
// seeking to its target whenever a key dies.
template <class A>
std::vector<std::string> automaton_walk(const termdict::reader& r, const A& a, stats& st) {
	std::vector<std::string> out;
	const std::string lit(a.literal());
	const auto hi = au::successor(lit);
	termdict::reader::cursor c(r);
	bool more = c.seek(lit);
	if constexpr (A::byte_range) {
		for (; more && (!hi || c.key() < *hi); more = c.next()) {
			++st.visited;
			out.emplace_back(c.key());
		}
		return out;
	} else {
		au::matcher<A> m(a);
		while (more) {
			const std::string_view key = c.key();
			if (hi && key >= *hi) { break; }
			++st.visited;
			const auto res = m(key);
			if (res.match) { out.emplace_back(key); }
			if (!res.died) {
				more = c.next();
				continue;
			}
			const auto t = m.seek();
			if (!t) { break; }
			if (t->size() == key.size() + 1 && t->back() == '\0' && t->starts_with(key)) {
				more = c.next();  // the target is the next key: no skip
			} else {
				++st.seeks;
#if AUTOMATA_EVAL_MUTANT == 3  // a seek one byte past its target
				more = c.seek(*t + '\x01');
#else
				more = c.seek(*t);
#endif
			}
		}
		return out;
	}
}

// The same walk over a restartable scan (fielddict's scan_dictionary shape): a seek ends the scan
// and starts another at the target, so no cursor API is needed.
template <class A>
std::vector<std::string> rescan_walk(const termdict::reader& r, const A& a, stats& st) {
	if constexpr (A::byte_range) {
		return automaton_walk(r, a, st);
	} else {
		std::vector<std::string> out;
		const auto hi = au::successor(a.literal());
		au::matcher<A> m(a);
		std::optional<std::string> from = std::string(a.literal());
		while (from) {
			std::optional<std::string> next;
			r.scan(*from, [&](std::string_view key, std::string_view) {
				if (hi && key >= *hi) { return false; }
				++st.visited;
				const auto res = m(key);
				if (res.match) { out.emplace_back(key); }
				if (!res.died) { return true; }
				auto t = m.seek();
				if (!t) { return false; }
				if (t->size() == key.size() + 1 && t->back() == '\0' && t->starts_with(key)) { return true; }
				++st.seeks;
				next = std::move(t);
				return false;
			});
			from = std::move(next);
		}
		return out;
	}
}

// Xapiand's query as the branch's automaton: no flags is a byte prefix; the flag bits swap.
template <class F>
auto with_automaton(const query& q, F&& f) {
#if AUTOMATA_EVAL_MUTANT == 1  // one edit too many
	if (!q.wildcard) { return f(au::levenshtein(q.str, q.k + 1, q.prefix_len, true)); }
#else
	if (!q.wildcard) { return f(au::levenshtein(q.str, q.k, q.prefix_len, true)); }
#endif
	if (!q.flags) { return f(au::prefix(q.str)); }
#if AUTOMATA_EVAL_MUTANT == 2  // Xapiand's flag bits passed unmapped
	const unsigned g = q.flags;
#else
	const unsigned g = (q.flags & 1 ? au::glob::single : 0) | (q.flags & 2 ? au::glob::multi : 0);
#endif
	return f(au::glob(q.str, g));
}

termdict::tables build(const std::vector<std::string>& terms) {
	termdict::builder b;
	for (const auto& t : terms) { b.add(t, {}); }
	return b.finish();
}

// --- check ---------------------------------------------------------------------------------------

int check(unsigned long trials) {
	// Valid UTF-8 of 1 to 4 bytes, and the pattern characters.
	const std::vector<std::string> alpha = {"a", "b", "c", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9D\x84\x9E"};
	std::mt19937_64 rng(20261109);
	auto pick = [&](std::size_t n) { return std::size_t(rng() % n); };
	auto word = [&](std::size_t max_len, bool meta) {
		std::string s;
		const std::size_t n = pick(max_len + 1);
		for (std::size_t i = 0; i < n; ++i) {
			const std::size_t c = pick(alpha.size() + (meta ? 2 : 0));
			s += c < alpha.size() ? alpha[c] : (c == alpha.size() ? "?" : "*");
		}
		return s;
	};
	std::uint64_t compared = 0, matched = 0, by_kind[3] = {};
	for (unsigned long t = 0; t < trials; ++t) {
		std::vector<std::string> terms;
		const std::size_t n = 1 + pick(400);
		for (std::size_t i = 0; i < n; ++i) { terms.push_back(word(7, false)); }
		std::sort(terms.begin(), terms.end());
		terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
		const auto tables = build(terms);
		const termdict::reader r(tables.blocks, tables.index);
		for (int j = 0; j < 8; ++j) {
			query q;
			const std::size_t kind = pick(3);
			if (kind < 2) {
				q.wildcard = true;
				q.str = word(6, true);
				q.flags = kind == 0 ? 0u : unsigned(1 + pick(3));
			} else {
				q.wildcard = false;
				q.str = word(6, false);
				q.k = unsigned(pick(4));
				q.prefix_len = pick(4);
			}
			stats sx, sa;
			const auto want = xapiand_walk(r, q, sx);
			// The brute force: every term tested by Xapiand's own predicates, no range.
			std::vector<std::string> brute;
			for (const auto& term : terms) {
				bool ok;
				if (q.wildcard) {
					ok = term.starts_with(engine::strata::wildcard_prefix(q.str, q.flags)) && engine::strata::wildcard_matches(q.str, term, q.flags);
				} else {
					const auto lead = engine::strata::head(q.str, q.prefix_len);
					ok = term.starts_with(lead) && engine::strata::within_edit_distance(std::string_view(q.str).substr(lead.size()), std::string_view(term).substr(lead.size()), q.k);
				}
				if (ok) { brute.push_back(term); }
			}
			const auto got = with_automaton(q, [&](const auto& a) { return automaton_walk(r, a, sa); });
			const auto rescanned = with_automaton(q, [&](const auto& a) { return rescan_walk(r, a, sa); });
			std::vector<std::string> sorted_got;
			with_automaton(q, [&](const auto& a) {
				au::intersect_sorted(a, terms.begin(), terms.end(), [&](std::string_view k) {
					sorted_got.emplace_back(k);
					return true;
				});
				return 0;
			});
			if (want != brute || got != brute || rescanned != brute || sorted_got != brute) {
				std::printf("MISMATCH trial %lu kind %zu pattern \"%s\" flags %u k %u prefix %zu: brute %zu, xapiand %zu, automaton %zu, rescan %zu, sorted %zu\n", t, kind, q.str.c_str(), q.flags, q.k, q.prefix_len, brute.size(), want.size(), got.size(), rescanned.size(), sorted_got.size());
				return 1;
			}
			++compared;
			++by_kind[kind];
			matched += brute.size();
		}
	}
	std::printf("check: %lu term sets, %llu queries (prefix %llu, glob %llu, edit distance %llu), %llu matches, all equal to brute force\n", trials, (unsigned long long)compared, (unsigned long long)by_kind[0], (unsigned long long)by_kind[1], (unsigned long long)by_kind[2], (unsigned long long)matched);
	return 0;
}

// --- bench ---------------------------------------------------------------------------------------

std::vector<query> queries(const std::vector<std::string>& terms, std::size_t per_class) {
	std::mt19937_64 rng(5);
	auto draw = [&](std::size_t lo, std::size_t hi) {
		for (;;) {
			const auto& t = terms[rng() % terms.size()];
			if (t.size() >= lo && t.size() <= hi) { return t; }
		}
	};
	std::vector<query> qs;
	auto add = [&](std::string cls, std::string s, bool wildcard, unsigned flags, unsigned k, std::size_t p) {
		qs.push_back({std::move(cls), std::move(s), wildcard, flags, k, p});
	};
	add("prefix", "obam", true, 0, 0, 0);
	add("glob ?", "ob?ma", true, 3, 0, 0);
	add("glob *x", "*ma", true, 3, 0, 0);
	for (unsigned k = 1; k <= 2; ++k) {
		for (std::size_t p : {0, 2}) { add("fuzzy k" + std::to_string(k) + " p" + std::to_string(p), "obama", false, 0, k, p); }
	}
	for (std::size_t i = 1; i < per_class; ++i) {
		add("prefix", draw(4, 12).substr(0, 3), true, 0, 0, 0);
		std::string g = draw(5, 10);
		g[2] = '?';
		add("glob ?", g, true, 3, 0, 0);
		add("glob *x", "*" + draw(4, 10).substr(1), true, 3, 0, 0);
		for (unsigned k = 1; k <= 2; ++k) {
			for (std::size_t p : {0, 2}) { add("fuzzy k" + std::to_string(k) + " p" + std::to_string(p), draw(5, 10), false, 0, k, p); }
		}
	}
	return qs;
}

int bench(const char* path, unsigned runs, std::size_t per_class) {
	using clock = std::chrono::steady_clock;
	const auto t0 = clock::now();
	std::vector<std::string> terms;
	{
		std::ifstream in(path);
		std::string line;
		while (std::getline(in, line)) { terms.push_back(line.substr(0, line.find('\t'))); }
	}
	std::sort(terms.begin(), terms.end());
	terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
	const auto tables = build(terms);
	const termdict::reader r(tables.blocks, tables.index);
	const auto qs = queries(terms, per_class);
	const auto t1 = clock::now();
	std::printf("config: vocab %s, %zu terms, termdict %zu block bytes + %zu index bytes, %zu queries, %u runs, NDEBUG %d, mutant %d, setup %.1f s\n", path, terms.size(), tables.blocks.size(), tables.index.size(), qs.size(), runs,
#ifdef NDEBUG
	            1,
#else
	            0,
#endif
	            AUTOMATA_EVAL_MUTANT, std::chrono::duration<double>(t1 - t0).count());
	if (terms.size() < 1000000 || qs.empty()) { return 2; }
	// Arms: x Xapiand's walk, a the automaton over a cursor, s the automaton over restarted scans,
	// l the automaton over a cursor and then one dictionary lookup per matched term.
	constexpr int arms = 4;
	auto walk = [&](int arm, const query& q, stats& st) {
		if (arm == 0) { return xapiand_walk(r, q, st); }
		if (arm == 2) { return with_automaton(q, [&](const auto& a) { return rescan_walk(r, a, st); }); }
		auto got = with_automaton(q, [&](const auto& a) { return automaton_walk(r, a, st); });
		if (arm == 3) {
			for (const auto& t : got) {
				if (!r.find(t)) { got.clear(); }
			}
		}
		return got;
	};
	std::vector<std::array<std::vector<double>, arms>> us(qs.size());
	std::vector<std::array<stats, arms>> st(qs.size());
	std::vector<std::size_t> matches(qs.size());
	for (std::size_t i = 0; i < qs.size(); ++i) {  // equal answers, in full, before timing
		std::array<std::vector<std::string>, arms> got;
		for (int arm = 0; arm < arms; ++arm) { got[arm] = walk(arm, qs[i], st[i][arm]); }
		if (got[1] != got[0] || got[2] != got[0] || got[3] != got[0]) {
			std::printf("MISMATCH %s \"%s\": %zu %zu %zu %zu\n", qs[i].cls.c_str(), qs[i].str.c_str(), got[0].size(), got[1].size(), got[2].size(), got[3].size());
			return 1;
		}
		matches[i] = got[0].size();
	}
	for (unsigned run = 0; run < runs; ++run) {
		for (std::size_t i = 0; i < qs.size(); ++i) {
			for (int j = 0; j < arms; ++j) {  // interleaved, the order rotating per run
				const int arm = int((j + run) % arms);
				stats s;
				const auto start = clock::now();
				const auto got = walk(arm, qs[i], s);
				us[i][arm].push_back(std::chrono::duration<double, std::micro>(clock::now() - start).count());
				if (got.size() != matches[i]) { return 1; }
			}
		}
	}
	auto median = [](std::vector<double> v) {
		std::sort(v.begin(), v.end());
		return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
	};
	std::printf("\n%-12s %-14s %8s %10s %10s %8s %12s %12s %12s %12s %9s %9s %9s\n", "class", "query", "matches", "x_visited", "a_visited", "a_seeks", "x_us", "a_us", "s_us", "l_us", "x/a", "s/a", "l/a");
	struct total {
		double us[arms] = {};
		std::uint64_t xv = 0, av = 0, n = 0;
		std::vector<double> ratio;
	};
	std::vector<std::pair<std::string, total>> by_class;
	for (std::size_t i = 0; i < qs.size(); ++i) {
		double m[arms];
		for (int arm = 0; arm < arms; ++arm) { m[arm] = median(us[i][arm]); }
		std::printf("%-12s %-14s %8zu %10llu %10llu %8llu %12.1f %12.1f %12.1f %12.1f %9.2f %9.2f %9.2f\n", qs[i].cls.c_str(), qs[i].str.c_str(), matches[i], (unsigned long long)st[i][0].visited, (unsigned long long)st[i][1].visited, (unsigned long long)st[i][1].seeks, m[0], m[1], m[2], m[3], m[0] / m[1], m[2] / m[1], m[3] / m[1]);
		auto it = std::find_if(by_class.begin(), by_class.end(), [&](const auto& e) { return e.first == qs[i].cls; });
		if (it == by_class.end()) { it = by_class.insert(by_class.end(), {qs[i].cls, {}}); }
		for (int arm = 0; arm < arms; ++arm) { it->second.us[arm] += m[arm]; }
		it->second.xv += st[i][0].visited;
		it->second.av += st[i][1].visited;
		it->second.ratio.push_back(m[0] / m[1]);
		++it->second.n;
	}
	std::printf("\n%-12s %4s %12s %12s %12s %12s %12s %12s %9s %9s %9s %9s %9s\n", "class", "n", "x_visited", "a_visited", "x_ms_sum", "a_ms_sum", "s_ms_sum", "l_ms_sum", "x/a_sum", "x/a_min", "x/a_med", "s/a_sum", "l/a_sum");
	for (auto& [cls, t] : by_class) {
		std::sort(t.ratio.begin(), t.ratio.end());
		std::printf("%-12s %4llu %12llu %12llu %12.2f %12.2f %12.2f %12.2f %9.2f %9.2f %9.2f %9.2f %9.2f\n", cls.c_str(), (unsigned long long)t.n, (unsigned long long)t.xv, (unsigned long long)t.av, t.us[0] / 1e3, t.us[1] / 1e3, t.us[2] / 1e3, t.us[3] / 1e3, t.us[0] / t.us[1], t.ratio.front(), median(t.ratio), t.us[2] / t.us[1], t.us[3] / t.us[1]);
	}
	std::printf("\ntimed %.1f s, total %.1f s\n", std::chrono::duration<double>(clock::now() - t1).count(), std::chrono::duration<double>(clock::now() - t0).count());
	return 0;
}

int main(int argc, char** argv) {
	const std::string mode = argc > 1 ? argv[1] : "";
	if (mode == "check") { return check(argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 2000); }
	if (mode == "bench" && argc > 3) { return bench(argv[2], unsigned(std::strtoul(argv[3], nullptr, 10)), argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 10); }
	std::fprintf(stderr, "usage: automata_eval check [trials] | bench VOCAB RUNS [per_class]\n");
	return 2;
}
```
<!-- end automata_eval.cc -->

</details>

<details><summary><code>run.sh</code></summary>

<!-- begin run.sh -->
```sh
#!/bin/bash
# automata_eval's driver. usage: run.sh DIR check TRIALS | run.sh DIR bench VOCAB SHA256 RUNS PER_CLASS
# DIR holds automata_eval.cc; JOB_REF_{STRATA,XAPIAND,UNICODE,TERMDICT,FST,VARINT} name the sources.
set -eu
D=$1; mode=$2; shift 2
mkdir -p "$D/shim/engine"
printf '#pragma once\ninline constexpr unsigned PATTERN_SINGLE = 1;\ninline constexpr unsigned PATTERN_MULTI = 2;\n' > "$D/shim/engine/query.h"
build() {  # build OUT [flags]
	local out=$1; shift
	c++ -std=c++20 -O2 -DNDEBUG "$@" -I"$D/shim" -I"$JOB_REF_STRATA/include" -I"$JOB_REF_XAPIAND/src" -I"$JOB_REF_UNICODE" \
		-I"$JOB_REF_TERMDICT" -I"$JOB_REF_FST" -I"$JOB_REF_VARINT" "$D/automata_eval.cc" "$JOB_REF_XAPIAND/src/engine/strata/pattern.cc" -o "$out"
}
c++ --version | head -1
for r in STRATA XAPIAND UNICODE TERMDICT FST VARINT; do v=JOB_REF_$r; echo "ref $r: ${!v}"; done
if [ "$mode" = check ]; then
	build "$D/eval"
	file "$D/eval"
	"$D/eval" check "$1"
	for m in 1 2 3; do  # each mutant must be caught
		build "$D/eval-m$m" -DAUTOMATA_EVAL_MUTANT=$m
		if "$D/eval-m$m" check "$1"; then echo "mutant $m survived"; exit 1; fi
		echo "mutant $m caught"
	done
else
	vocab=$1 sha=$2 runs=$3 per=$4
	echo "$sha  $vocab" | shasum -a 256 -c -
	build "$D/eval" -march=native
	file "$D/eval"
	"$D/eval" bench "$vocab" "$runs" "$per"
fi
```
<!-- end run.sh -->

</details>

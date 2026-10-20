# The loss attribution's two format-free wins, landed as code

Rows 1 and 3 of `format13-loss-attribution.md`, measured there as prototypes
(`format13-loss-attribution-ff.patch`), written as production code on `perf/query-wins`.

## The changes

1. **Counted unions prune** (row 1). `disjunction_scorer::collect_top_k` kept every clause
   essential and scored every match whenever the collector counted (`TOP_100_COUNT` with three
   or more clauses; two clauses take the exact count by inclusion and exclusion). It now counts
   from a fresh copy's window bits with `count()`, the same windows `COUNT` fills, and then
   collects as an uncounted top-k does: the MaxScore split from the collector's threshold, and
   non-essential clauses consulted only while a document can compete. A union whose clauses
   cannot make a fresh copy counts inline as before. The prototype interleaved the counting
   copies window by window inside the scoring loop; counting first through `count()` reuses
   its 4,096-aligned windows (where bitset blocks are OR'd whole) and keeps the scoring loop
   as top-k has it.
2. **Required plus optional skips blocks** (row 3), ported from Lucene 10.4's
   `ReqOptSumScorer` (Apache License 2.0; `NOTICE`, `LICENSES/Apache-2.0.txt`). A window is the
   required side's block; the optional side adds nothing to a window its next match lies past
   (`getMaxScore`). A window whose two bounds cannot compete is skipped; where the required
   bound alone cannot, the optional side becomes required and the two leapfrog, across windows
   when the required side can never compete alone (`upperBound`). Inside a window the
   per-document rule stays: a required match whose score plus the optional bound cannot
   compete is skipped before the optional side moves. Rounding follows the scorer's: every
   comparison bounds a two-term sum with `sum_upper`.

   Lucene also ends a window at the optional side's block, or just before its next match
   (`advanceShallow`). Ported that way first (`5bcc81a`), it measured slower than the
   prototype's required-block windows: intersection_union `TOP_10` 0.904 [0.850–0.961] against
   the prototype's 0.820, `TOP_100` 0.965 [0.899–1.036], `TOP_100_COUNT` 1.001, and `TOP_1000`
   1.063 [1.020–1.107] (three terms 1.114 [1.044–1.189], inside its A/A band of 0.936–1.141), at
   1M, bm25, eight rounds. More, smaller windows cost more bound evaluations (each optional
   clause's block search and bound per window) than their tighter bounds save. `3b5a311` keeps
   the required side's blocks.

## Proof

Host: macOS 15.6, Intel i9-9980HK (x86_64), Apple clang, Release unless noted. A is main
`57a3091`, B this branch.

### Tests, revert checks, mutants

`search_test` gains a scoring tally (a scorer that forwards and counts `score()` calls; window
scoring goes through it, so every posting scored counts). On 100,000 documents, docid-ordered
and reordered, at k 10 and 100: a counted union of four clauses (one rare, three at 40%)
returns the exhaustive scan's best k exactly (ordinals and scores, ties by docid) and its exact
count, scoring under half of its matches; a required term at 50% with an optional one at 2%
returns the scan's best k scoring under half of the required documents.

| Check | Result |
|---|---|
| Row 1 reverted | the tally check fails, 4 of 4 (two segments, k 10 and 100) |
| Row 3 reverted | the tally check fails, 4 of 4 |
| M1: count as `copy->cost()` | caught: counted top-k's count (random corpus), 21 failures |
| M1b: count also inline | caught: counted top-k's count, 21 failures |
| M3: skip a window on the required bound alone | caught: pruned top-k against brute force |
| M3b: leapfrog past the window when the required side competes alone | caught: counted and pruned top-k |
| M3c: the optional bound zero at `>=` its next match | caught: pruned and floored top-k |
| Release suite | 39 of 39 |
| ASan + UBSan (Homebrew LLVM, `-O1`) | `search`, `exact`, `multisegment`, `engine_query` pass; `strata_engine verify` over the 962 gate queries at 100k (reordered; 9 flushed segments) 0 mismatches, no reports |
| CI | run 39438785455 (`c507f2a`): all eight jobs green, `linux-asan-ubsan` and `linux-tsan` included |

### Identity and parity

`strata_engine verify` now checks the uncounted search as well as the counted one, and
`STRATA_DUMP_COUNT=0` dumps the uncounted path (`c507f2a`). Indexes built once with A's binary
from the first N corpus documents (identity seed 1): 100k (4 flushed segments) and 1M (32),
each docid-merged and reordered (4 threads); 100k also flushed, 9 segments.

| Index | dump, counted | dump, uncounted | verify (B) |
|---|---|---|---|
| 100k merged | identical (5,772 lines) | identical | 962 queries, 0 mismatches |
| 100k reordered | identical | identical | 0 mismatches |
| 100k flushed, 9 segments | identical | identical | 0 mismatches |
| 1M merged | identical | identical | 0 mismatches |
| 1M reordered | identical | identical | 0 mismatches |

Each dump is every gate query at k 10, 100, and 1,000 under both presets (docids and exact
scores), and B's counted and uncounted hit lists agree line for line. Parity against the
oracle's rank vectors (419 queries over 10,000 documents, counts and top 10, counted and
pruned): 419 of 419 in one segment, in four, and reordered (`PARITY_REORDER=2`), worst
relative error 3.87e-16 (the format-13 merge review recorded the same).

## Run-level A/B at 1M

The 1M reordered index above (A's build), the 962 gate queries and the 200 stratified terms,
`bench/quick/ab.sh` at one thread, best of 3 per query and round, 16 rounds for the A/A (A
against itself) and then 16 for the A/B, each preset, scored at the run level by perf/query-exp's
`bench/quick/ab.py --aa` (`ab-run-level.md`): a loss needs its lower bound above 1.03 and the A/A
band, a win its upper bound under 1.0 and the band with the ratio at most 0.97. Under a light
hold (`quiet run`), B at `3b5a311`; A/A then A/B took 4.5 and 4.2 minutes (bm25), 4.9 and 4.5
(bm25_xapian).

**No row is lost under either preset.** The two target rows, against the prototype's
(`format13-loss-attribution.md`, bm25, six rounds):

| Row | Prototype | bm25 | bm25_xapian |
|---|---:|---|---|
| `TOP_100_COUNT` union:num_tokens_3 | 0.478 | **0.461** [0.451–0.472], 2.17× faster | **0.390** [0.378–0.403], 2.56× faster |
| `TOP_100_COUNT` union:num_tokens_>3 | 0.364 | **0.345** [0.337–0.353], 2.90× faster | **0.291** [0.281–0.303], 3.44× faster |
| `TOP_10` intersection_union | 0.820 | **0.859** [0.840–0.879], 1.16× faster | **0.764** [0.740–0.789], 1.31× faster |
| `TOP_100` intersection_union | 0.879 | **0.890** [0.856–0.926], 1.12× faster | **0.848** [0.803–0.896], 1.18× faster |
| `TOP_100_COUNT` intersection_union | 0.876 | **0.878** [0.856–0.901], 1.14× faster | **0.837** [0.809–0.865], 1.19× faster |
| `TOP_1000` intersection_union | 1.001 | 0.997 [0.975–1.019], neither | 1.000 [0.965–1.036], neither |

Row 1 meets or beats the prototype under both presets; row 3 meets it at `TOP_100` and
`TOP_100_COUNT` and comes within 0.04 at bm25's `TOP_10` (beating it under bm25_xapian).
`TOP_1000` stays unmoved, as the attribution predicted (a low threshold).

The highest point estimates elsewhere are on code this change does not run: `COUNT`
intersection_union 1.023 and 1.040 (each of its 40 queries has one required term, so the count
is that term's document frequency, answered in about 6.4 µs without iterating), `COUNT` negated
1.046 [1.005–1.088] under bm25_xapian but 1.013 [0.973–1.054] under bm25 (`COUNT` is the same
work under both presets, so the gap is the session's noise), and `TOP_1000` term:frequent 1.050
[0.994–1.108] (term scorers are unchanged). None is a loss by the rule.

Every row, B/A [95% over runs], its direction, the A/A band, and the verdict:

| Command | Row | n | bm25 | bm25_xapian |
|---|---|---:|---|---|
| `TOP_10` | global | 657 | 1.004 [0.990–1.018], 0.4% slower; A/A 0.971 [0.938–1.005]; neither | 0.997 [0.961–1.035], 0.3% faster; A/A 1.001 [0.999–1.004]; neither |
| `TOP_10` | intersection | 300 | 1.007 [0.991–1.022], 0.7% slower; A/A 0.962 [0.919–1.007]; neither | 1.003 [0.966–1.042], 0.3% slower; A/A 1.002 [0.999–1.005]; neither |
| `TOP_10` | intersection:num_tokens_2 | 198 | 1.007 [0.991–1.023], 0.7% slower; A/A 0.965 [0.928–1.003]; neither | 1.005 [0.968–1.044], 0.5% slower; A/A 1.003 [0.999–1.007]; neither |
| `TOP_10` | intersection:num_tokens_3 | 83 | 1.007 [0.992–1.023], 0.7% slower; A/A 0.960 [0.913–1.010]; neither | 0.993 [0.951–1.036], 0.7% faster; A/A 1.002 [0.998–1.005]; neither |
| `TOP_10` | intersection:num_tokens_>3 | 19 | 1.002 [0.989–1.015], 0.2% slower; A/A 0.965 [0.917–1.016]; neither | 0.986 [0.957–1.017], 1.4% faster; A/A 0.997 [0.990–1.004]; neither |
| `TOP_10` | intersection_union | 40 | 0.859 [0.840–0.879], 1.16× faster; A/A 1.006 [0.961–1.053]; win | 0.764 [0.740–0.789], 1.31× faster; A/A 1.000 [0.987–1.014]; win |
| `TOP_10` | intersection_union:num_tokens_2 | 10 | 0.961 [0.936–0.986], 3.9% faster; A/A 1.014 [0.953–1.078]; neither | 0.795 [0.762–0.830], 1.26× faster; A/A 1.002 [0.966–1.039]; win |
| `TOP_10` | intersection_union:num_tokens_3 | 20 | 0.816 [0.793–0.839], 1.23× faster; A/A 0.977 [0.928–1.029]; win | 0.720 [0.690–0.752], 1.39× faster; A/A 0.998 [0.978–1.019]; win |
| `TOP_10` | intersection_union:num_tokens_>3 | 10 | 0.898 [0.872–0.925], 1.11× faster; A/A 1.027 [0.983–1.073]; win | 0.824 [0.795–0.854], 1.21× faster; A/A 0.996 [0.974–1.018]; win |
| `TOP_10` | negated | 19 | 0.986 [0.958–1.015], 1.4% faster; A/A 1.009 [0.957–1.063]; neither | 0.956 [0.886–1.031], 4.4% faster; A/A 1.010 [0.987–1.032]; neither |
| `TOP_10` | negated:num_tokens_2 | 4 | 0.986 [0.957–1.015], 1.4% faster; A/A 1.011 [0.959–1.066]; neither | 0.960 [0.895–1.031], 4.0% faster; A/A 1.007 [0.982–1.033]; neither |
| `TOP_10` | negated:num_tokens_3 | 15 | 0.986 [0.958–1.015], 1.4% faster; A/A 1.009 [0.955–1.065]; neither | 0.955 [0.885–1.032], 4.5% faster; A/A 1.010 [0.988–1.032]; neither |
| `TOP_10` | phrase | 300 | 1.008 [0.993–1.023], 0.8% slower; A/A 0.969 [0.930–1.008]; neither | 1.002 [0.965–1.039], 0.2% slower; A/A 1.000 [0.998–1.003]; neither |
| `TOP_10` | phrase:num_tokens_2 | 198 | 1.008 [0.992–1.023], 0.8% slower; A/A 0.971 [0.938–1.005]; neither | 1.004 [0.968–1.041], 0.4% slower; A/A 1.000 [0.997–1.003]; neither |
| `TOP_10` | phrase:num_tokens_3 | 83 | 1.009 [0.995–1.023], 0.9% slower; A/A 0.965 [0.920–1.013]; neither | 0.997 [0.955–1.040], 0.3% faster; A/A 1.001 [0.998–1.004]; neither |
| `TOP_10` | phrase:num_tokens_>3 | 19 | 1.006 [0.989–1.023], 0.6% slower; A/A 0.972 [0.935–1.010]; neither | 0.995 [0.967–1.023], 0.5% faster; A/A 1.000 [0.996–1.004]; neither |
| `TOP_10` | term | 201 | 0.994 [0.974–1.014], 0.6% faster; A/A 1.013 [0.977–1.051]; neither | 0.971 [0.940–1.003], 2.9% faster; A/A 1.014 [0.979–1.051]; neither |
| `TOP_10` | term:frequent | 66 | 1.008 [0.982–1.035], 0.8% slower; A/A 1.011 [0.969–1.054]; neither | 0.981 [0.960–1.003], 1.9% faster; A/A 1.010 [0.979–1.042]; neither |
| `TOP_10` | term:medium | 67 | 0.994 [0.974–1.015], 0.6% faster; A/A 1.012 [0.973–1.053]; neither | 0.970 [0.939–1.002], 3.0% faster; A/A 1.016 [0.979–1.055]; neither |
| `TOP_10` | term:rare | 67 | 0.975 [0.954–0.997], 2.5% faster; A/A 1.008 [0.974–1.043]; neither | 0.970 [0.932–1.010], 3.0% faster; A/A 1.013 [0.976–1.052]; neither |
| `TOP_10` | two-phase-critic | 1 | 1.000 [0.973–1.028], same; A/A 0.962 [0.914–1.013]; neither | 0.977 [0.943–1.012], 2.3% faster; A/A 0.982 [0.947–1.019]; neither |
| `TOP_10` | union | 301 | 1.004 [0.989–1.019], 0.4% slower; A/A 0.967 [0.927–1.008]; neither | 1.001 [0.964–1.039], 0.1% slower; A/A 1.000 [0.998–1.003]; neither |
| `TOP_10` | union:num_tokens_2 | 198 | 1.007 [0.992–1.023], 0.7% slower; A/A 0.968 [0.934–1.003]; neither | 1.004 [0.967–1.044], 0.4% slower; A/A 1.001 [0.998–1.004]; neither |
| `TOP_10` | union:num_tokens_3 | 83 | 1.000 [0.986–1.014], same; A/A 0.967 [0.922–1.015]; neither | 0.989 [0.952–1.027], 1.1% faster; A/A 1.000 [0.997–1.003]; neither |
| `TOP_10` | union:num_tokens_>3 | 20 | 0.999 [0.986–1.012], 0.1% faster; A/A 0.973 [0.930–1.018]; neither | 0.985 [0.953–1.018], 1.5% faster; A/A 1.001 [0.996–1.006]; neither |
| `TOP_100` | global | 657 | 0.996 [0.983–1.009], 0.4% faster; A/A 0.997 [0.989–1.005]; neither | 1.006 [0.995–1.016], 0.6% slower; A/A 1.001 [0.995–1.007]; neither |
| `TOP_100` | intersection | 300 | 0.999 [0.986–1.013], 0.1% faster; A/A 0.996 [0.986–1.006]; neither | 1.007 [0.997–1.017], 0.7% slower; A/A 0.998 [0.987–1.010]; neither |
| `TOP_100` | intersection:num_tokens_2 | 198 | 0.998 [0.984–1.012], 0.2% faster; A/A 0.995 [0.984–1.006]; neither | 1.006 [0.997–1.015], 0.6% slower; A/A 0.997 [0.982–1.012]; neither |
| `TOP_100` | intersection:num_tokens_3 | 83 | 1.005 [0.991–1.020], 0.5% slower; A/A 0.997 [0.987–1.006]; neither | 1.008 [0.995–1.022], 0.8% slower; A/A 1.000 [0.993–1.007]; neither |
| `TOP_100` | intersection:num_tokens_>3 | 19 | 1.000 [0.981–1.019], same; A/A 0.999 [0.983–1.015]; neither | 1.004 [0.994–1.015], 0.4% slower; A/A 1.008 [0.997–1.020]; neither |
| `TOP_100` | intersection_union | 40 | 0.890 [0.856–0.926], 1.12× faster; A/A 1.036 [0.991–1.083]; win | 0.848 [0.803–0.896], 1.18× faster; A/A 1.015 [0.998–1.033]; win |
| `TOP_100` | intersection_union:num_tokens_2 | 10 | 0.849 [0.808–0.893], 1.18× faster; A/A 1.047 [1.001–1.094]; win | 0.752 [0.697–0.811], 1.33× faster; A/A 1.029 [0.988–1.072]; win |
| `TOP_100` | intersection_union:num_tokens_3 | 20 | 0.883 [0.850–0.918], 1.13× faster; A/A 1.036 [0.982–1.092]; win | 0.869 [0.813–0.928], 1.15× faster; A/A 1.021 [0.995–1.047]; win |
| `TOP_100` | intersection_union:num_tokens_>3 | 10 | 0.915 [0.875–0.958], 8.5% faster; A/A 1.024 [0.977–1.073]; win | 0.885 [0.838–0.934], 1.13× faster; A/A 1.016 [1.001–1.031]; win |
| `TOP_100` | negated | 19 | 0.992 [0.949–1.036], 0.8% faster; A/A 1.017 [0.962–1.075]; neither | 1.024 [0.952–1.102], 2.4% slower; A/A 1.025 [0.996–1.055]; neither |
| `TOP_100` | negated:num_tokens_2 | 4 | 0.987 [0.941–1.036], 1.3% faster; A/A 1.022 [0.970–1.077]; neither | 1.025 [0.963–1.091], 2.5% slower; A/A 1.004 [0.978–1.031]; neither |
| `TOP_100` | negated:num_tokens_3 | 15 | 0.994 [0.951–1.038], 0.6% faster; A/A 1.015 [0.960–1.073]; neither | 1.021 [0.948–1.101], 2.1% slower; A/A 1.026 [0.997–1.057]; neither |
| `TOP_100` | phrase | 300 | 1.003 [0.990–1.017], 0.3% slower; A/A 0.996 [0.986–1.007]; neither | 1.008 [0.997–1.019], 0.8% slower; A/A 0.999 [0.991–1.008]; neither |
| `TOP_100` | phrase:num_tokens_2 | 198 | 1.001 [0.989–1.014], 0.1% slower; A/A 0.996 [0.985–1.007]; neither | 1.007 [0.997–1.017], 0.7% slower; A/A 0.998 [0.986–1.010]; neither |
| `TOP_100` | phrase:num_tokens_3 | 83 | 1.007 [0.993–1.021], 0.7% slower; A/A 0.998 [0.988–1.008]; neither | 1.009 [0.997–1.022], 0.9% slower; A/A 1.001 [0.995–1.008]; neither |
| `TOP_100` | phrase:num_tokens_>3 | 19 | 1.008 [0.988–1.028], 0.8% slower; A/A 0.984 [0.965–1.003]; neither | 1.001 [0.994–1.009], 0.1% slower; A/A 1.003 [0.997–1.008]; neither |
| `TOP_100` | term | 201 | 0.997 [0.974–1.021], 0.3% faster; A/A 1.017 [0.980–1.055]; neither | 0.985 [0.948–1.023], 1.5% faster; A/A 0.997 [0.936–1.061]; neither |
| `TOP_100` | term:frequent | 66 | 0.985 [0.956–1.015], 1.5% faster; A/A 1.036 [0.954–1.125]; neither | 1.002 [0.958–1.048], 0.2% slower; A/A 0.995 [0.956–1.035]; neither |
| `TOP_100` | term:medium | 67 | 0.995 [0.971–1.020], 0.5% faster; A/A 1.018 [0.978–1.059]; neither | 0.982 [0.945–1.019], 1.8% faster; A/A 1.004 [0.942–1.069]; neither |
| `TOP_100` | term:rare | 67 | 1.001 [0.978–1.023], 0.1% slower; A/A 1.022 [0.980–1.065]; neither | 0.972 [0.933–1.013], 2.8% faster; A/A 0.976 [0.884–1.078]; neither |
| `TOP_100` | two-phase-critic | 1 | 0.997 [0.967–1.028], 0.3% faster; A/A 1.041 [0.997–1.087]; neither | 1.001 [0.949–1.056], 0.1% slower; A/A 1.009 [0.963–1.058]; neither |
| `TOP_100` | union | 301 | 1.002 [0.991–1.013], 0.2% slower; A/A 0.995 [0.986–1.005]; neither | 1.008 [0.997–1.018], 0.8% slower; A/A 1.000 [0.992–1.008]; neither |
| `TOP_100` | union:num_tokens_2 | 198 | 1.002 [0.991–1.013], 0.2% slower; A/A 0.996 [0.986–1.006]; neither | 1.010 [1.000–1.020], 1.0% slower; A/A 0.998 [0.987–1.009]; neither |
| `TOP_100` | union:num_tokens_3 | 83 | 0.998 [0.986–1.011], 0.2% faster; A/A 0.994 [0.983–1.005]; neither | 1.005 [0.990–1.020], 0.5% slower; A/A 1.003 [0.997–1.009]; neither |
| `TOP_100` | union:num_tokens_>3 | 20 | 0.997 [0.980–1.014], 0.3% faster; A/A 0.994 [0.982–1.007]; neither | 1.002 [0.990–1.013], 0.2% slower; A/A 1.003 [0.997–1.009]; neither |
| `TOP_1000` | global | 657 | 0.994 [0.983–1.004], 0.6% faster; A/A 0.998 [0.990–1.006]; neither | 0.997 [0.989–1.006], 0.3% faster; A/A 1.014 [0.993–1.035]; neither |
| `TOP_1000` | intersection | 300 | 0.993 [0.983–1.003], 0.7% faster; A/A 0.997 [0.990–1.004]; neither | 0.997 [0.990–1.003], 0.3% faster; A/A 1.015 [0.992–1.038]; neither |
| `TOP_1000` | intersection:num_tokens_2 | 198 | 0.992 [0.979–1.006], 0.8% faster; A/A 0.997 [0.989–1.005]; neither | 0.997 [0.990–1.003], 0.3% faster; A/A 1.015 [0.992–1.038]; neither |
| `TOP_1000` | intersection:num_tokens_3 | 83 | 0.993 [0.983–1.004], 0.7% faster; A/A 0.999 [0.991–1.007]; neither | 0.997 [0.990–1.005], 0.3% faster; A/A 1.015 [0.992–1.038]; neither |
| `TOP_1000` | intersection:num_tokens_>3 | 19 | 0.988 [0.972–1.005], 1.2% faster; A/A 1.001 [0.989–1.013]; neither | 0.994 [0.984–1.004], 0.6% faster; A/A 1.014 [0.993–1.036]; neither |
| `TOP_1000` | intersection_union | 40 | 0.997 [0.975–1.019], 0.3% faster; A/A 1.009 [0.956–1.064]; neither | 1.000 [0.965–1.036], same; A/A 0.994 [0.972–1.016]; neither |
| `TOP_1000` | intersection_union:num_tokens_2 | 10 | 1.012 [0.979–1.046], 1.2% slower; A/A 1.006 [0.953–1.062]; neither | 1.009 [0.952–1.068], 0.9% slower; A/A 0.992 [0.952–1.033]; neither |
| `TOP_1000` | intersection_union:num_tokens_3 | 20 | 1.007 [0.985–1.028], 0.7% slower; A/A 1.016 [0.964–1.071]; neither | 1.018 [0.980–1.057], 1.8% slower; A/A 0.992 [0.971–1.012]; neither |
| `TOP_1000` | intersection_union:num_tokens_>3 | 10 | 0.953 [0.926–0.981], 4.7% faster; A/A 1.006 [0.950–1.066]; neither | 0.927 [0.891–0.966], 7.3% faster; A/A 1.011 [0.979–1.043]; win |
| `TOP_1000` | negated | 19 | 1.007 [0.970–1.045], 0.7% slower; A/A 1.015 [0.970–1.063]; neither | 1.010 [0.970–1.050], 1.0% slower; A/A 0.999 [0.969–1.031]; neither |
| `TOP_1000` | negated:num_tokens_2 | 4 | 1.016 [0.977–1.056], 1.6% slower; A/A 1.014 [0.966–1.064]; neither | 1.009 [0.969–1.051], 0.9% slower; A/A 0.999 [0.969–1.030]; neither |
| `TOP_1000` | negated:num_tokens_3 | 15 | 1.003 [0.964–1.043], 0.3% slower; A/A 1.013 [0.966–1.061]; neither | 1.005 [0.965–1.047], 0.5% slower; A/A 0.999 [0.968–1.031]; neither |
| `TOP_1000` | phrase | 300 | 0.994 [0.984–1.005], 0.6% faster; A/A 0.996 [0.989–1.004]; neither | 0.997 [0.990–1.003], 0.3% faster; A/A 1.014 [0.991–1.037]; neither |
| `TOP_1000` | phrase:num_tokens_2 | 198 | 0.995 [0.982–1.008], 0.5% faster; A/A 0.994 [0.982–1.006]; neither | 0.996 [0.989–1.003], 0.4% faster; A/A 1.014 [0.991–1.037]; neither |
| `TOP_1000` | phrase:num_tokens_3 | 83 | 0.993 [0.984–1.002], 0.7% faster; A/A 0.995 [0.987–1.002]; neither | 0.998 [0.992–1.005], 0.2% faster; A/A 1.013 [0.992–1.035]; neither |
| `TOP_1000` | phrase:num_tokens_>3 | 19 | 0.989 [0.976–1.002], 1.1% faster; A/A 1.002 [0.989–1.015]; neither | 0.993 [0.981–1.006], 0.7% faster; A/A 1.013 [0.990–1.037]; neither |
| `TOP_1000` | term | 201 | 0.995 [0.978–1.012], 0.5% faster; A/A 1.017 [0.942–1.097]; neither | 1.025 [0.944–1.114], 2.5% slower; A/A 0.978 [0.944–1.014]; neither |
| `TOP_1000` | term:frequent | 66 | 1.010 [0.984–1.037], 1.0% slower; A/A 0.993 [0.924–1.066]; neither | 1.050 [0.994–1.108], 5.0% slower; A/A 1.010 [0.983–1.037]; neither |
| `TOP_1000` | term:medium | 67 | 0.992 [0.970–1.015], 0.8% faster; A/A 1.013 [0.942–1.088]; neither | 1.004 [0.937–1.075], 0.4% slower; A/A 0.964 [0.920–1.010]; neither |
| `TOP_1000` | term:rare | 67 | 0.998 [0.974–1.022], 0.2% faster; A/A 1.041 [0.944–1.149]; neither | 1.029 [0.932–1.137], 2.9% slower; A/A 0.981 [0.945–1.018]; neither |
| `TOP_1000` | two-phase-critic | 1 | 0.990 [0.968–1.012], 1.0% faster; A/A 1.023 [0.985–1.062]; neither | 0.997 [0.967–1.027], 0.3% faster; A/A 0.990 [0.965–1.015]; neither |
| `TOP_1000` | union | 301 | 0.996 [0.988–1.005], 0.4% faster; A/A 0.998 [0.988–1.009]; neither | 0.996 [0.988–1.005], 0.4% faster; A/A 1.012 [0.993–1.032]; neither |
| `TOP_1000` | union:num_tokens_2 | 198 | 1.002 [0.991–1.012], 0.2% slower; A/A 0.995 [0.980–1.010]; neither | 0.996 [0.988–1.005], 0.4% faster; A/A 1.013 [0.993–1.034]; neither |
| `TOP_1000` | union:num_tokens_3 | 83 | 0.988 [0.977–0.999], 1.2% faster; A/A 0.999 [0.989–1.009]; neither | 0.994 [0.986–1.002], 0.6% faster; A/A 1.011 [0.992–1.031]; neither |
| `TOP_1000` | union:num_tokens_>3 | 20 | 0.993 [0.980–1.006], 0.7% faster; A/A 0.995 [0.978–1.012]; neither | 1.001 [0.986–1.017], 0.1% slower; A/A 1.006 [0.988–1.024]; neither |
| `TOP_100_COUNT` | global | 657 | 0.987 [0.976–0.999], 1.3% faster; A/A 0.992 [0.981–1.004]; neither | 0.995 [0.967–1.024], 0.5% faster; A/A 1.012 [0.993–1.032]; neither |
| `TOP_100_COUNT` | intersection | 300 | 0.996 [0.987–1.004], 0.4% faster; A/A 0.993 [0.981–1.004]; neither | 1.014 [0.980–1.049], 1.4% slower; A/A 1.013 [0.989–1.039]; neither |
| `TOP_100_COUNT` | intersection:num_tokens_2 | 198 | 0.996 [0.991–1.002], 0.4% faster; A/A 0.992 [0.979–1.004]; neither | 1.016 [0.978–1.054], 1.6% slower; A/A 1.013 [0.988–1.039]; neither |
| `TOP_100_COUNT` | intersection:num_tokens_3 | 83 | 0.997 [0.986–1.007], 0.3% faster; A/A 0.993 [0.979–1.007]; neither | 1.010 [0.980–1.041], 1.0% slower; A/A 1.016 [0.991–1.041]; neither |
| `TOP_100_COUNT` | intersection:num_tokens_>3 | 19 | 0.993 [0.981–1.005], 0.7% faster; A/A 0.990 [0.980–1.001]; neither | 1.015 [0.981–1.051], 1.5% slower; A/A 0.999 [0.977–1.021]; neither |
| `TOP_100_COUNT` | intersection_union | 40 | 0.878 [0.856–0.901], 1.14× faster; A/A 0.963 [0.922–1.006]; win | 0.837 [0.809–0.865], 1.19× faster; A/A 0.991 [0.968–1.014]; win |
| `TOP_100_COUNT` | intersection_union:num_tokens_2 | 10 | 0.854 [0.829–0.880], 1.17× faster; A/A 0.967 [0.932–1.003]; win | 0.765 [0.724–0.809], 1.31× faster; A/A 0.993 [0.964–1.023]; win |
| `TOP_100_COUNT` | intersection_union:num_tokens_3 | 20 | 0.868 [0.839–0.899], 1.15× faster; A/A 0.957 [0.899–1.018]; neither | 0.835 [0.800–0.871], 1.20× faster; A/A 0.990 [0.968–1.013]; win |
| `TOP_100_COUNT` | intersection_union:num_tokens_>3 | 10 | 0.911 [0.887–0.936], 8.9% faster; A/A 0.981 [0.942–1.022]; win | 0.886 [0.856–0.918], 1.13× faster; A/A 0.983 [0.956–1.011]; win |
| `TOP_100_COUNT` | negated | 19 | 0.999 [0.965–1.035], 0.1% faster; A/A 0.984 [0.943–1.026]; neither | 0.993 [0.949–1.039], 0.7% faster; A/A 0.972 [0.931–1.015]; neither |
| `TOP_100_COUNT` | negated:num_tokens_2 | 4 | 1.002 [0.968–1.037], 0.2% slower; A/A 0.981 [0.940–1.025]; neither | 0.984 [0.921–1.051], 1.6% faster; A/A 0.958 [0.905–1.015]; neither |
| `TOP_100_COUNT` | negated:num_tokens_3 | 15 | 0.997 [0.961–1.033], 0.3% faster; A/A 0.984 [0.945–1.026]; neither | 0.995 [0.951–1.042], 0.5% faster; A/A 0.982 [0.937–1.030]; neither |
| `TOP_100_COUNT` | phrase | 300 | 0.995 [0.985–1.006], 0.5% faster; A/A 0.993 [0.983–1.003]; neither | 1.015 [0.979–1.053], 1.5% slower; A/A 1.013 [0.990–1.037]; neither |
| `TOP_100_COUNT` | phrase:num_tokens_2 | 198 | 0.995 [0.985–1.007], 0.5% faster; A/A 0.992 [0.982–1.002]; neither | 1.017 [0.979–1.057], 1.7% slower; A/A 1.012 [0.988–1.037]; neither |
| `TOP_100_COUNT` | phrase:num_tokens_3 | 83 | 0.996 [0.988–1.004], 0.4% faster; A/A 0.994 [0.984–1.005]; neither | 1.010 [0.979–1.043], 1.0% slower; A/A 1.014 [0.991–1.037]; neither |
| `TOP_100_COUNT` | phrase:num_tokens_>3 | 19 | 0.986 [0.973–1.000], 1.4% faster; A/A 0.994 [0.980–1.009]; neither | 1.001 [0.971–1.033], 0.1% slower; A/A 1.006 [0.991–1.021]; neither |
| `TOP_100_COUNT` | term | 201 | 1.004 [0.991–1.017], 0.4% slower; A/A 0.989 [0.959–1.020]; neither | 1.002 [0.958–1.048], 0.2% slower; A/A 0.994 [0.961–1.028]; neither |
| `TOP_100_COUNT` | term:frequent | 66 | 1.003 [0.966–1.041], 0.3% slower; A/A 0.971 [0.932–1.012]; neither | 1.038 [0.994–1.084], 3.8% slower; A/A 1.003 [0.977–1.029]; neither |
| `TOP_100_COUNT` | term:medium | 67 | 1.003 [0.987–1.020], 0.3% slower; A/A 0.986 [0.957–1.016]; neither | 0.998 [0.950–1.048], 0.2% faster; A/A 0.993 [0.959–1.028]; neither |
| `TOP_100_COUNT` | term:rare | 67 | 1.006 [0.997–1.016], 0.6% slower; A/A 0.990 [0.954–1.027]; neither | 1.004 [0.958–1.052], 0.4% slower; A/A 0.991 [0.955–1.029]; neither |
| `TOP_100_COUNT` | two-phase-critic | 1 | 1.004 [0.974–1.035], 0.4% slower; A/A 0.983 [0.941–1.027]; neither | 0.969 [0.942–0.997], 3.1% faster; A/A 1.011 [0.981–1.041]; neither |
| `TOP_100_COUNT` | union | 301 | 0.973 [0.955–0.990], 2.7% faster; A/A 0.990 [0.979–1.002]; neither | 0.977 [0.949–1.005], 2.3% faster; A/A 1.010 [0.994–1.026]; neither |
| `TOP_100_COUNT` | union:num_tokens_2 | 198 | 0.998 [0.992–1.005], 0.2% faster; A/A 0.990 [0.977–1.003]; neither | 1.018 [0.981–1.056], 1.8% slower; A/A 1.014 [0.993–1.035]; neither |
| `TOP_100_COUNT` | union:num_tokens_3 | 83 | 0.461 [0.451–0.472], 2.17× faster; A/A 0.990 [0.977–1.003]; win | 0.390 [0.378–0.403], 2.56× faster; A/A 1.008 [0.995–1.021]; win |
| `TOP_100_COUNT` | union:num_tokens_>3 | 20 | 0.345 [0.337–0.353], 2.90× faster; A/A 0.989 [0.979–1.000]; win | 0.291 [0.281–0.303], 3.44× faster; A/A 1.005 [0.995–1.015]; win |
| `COUNT` | global | 657 | 1.005 [0.992–1.017], 0.5% slower; A/A 0.998 [0.970–1.027]; neither | 0.997 [0.986–1.009], 0.3% faster; A/A 1.004 [0.996–1.013]; neither |
| `COUNT` | intersection | 300 | 1.005 [0.992–1.019], 0.5% slower; A/A 0.997 [0.966–1.030]; neither | 0.993 [0.977–1.009], 0.7% faster; A/A 1.004 [0.996–1.013]; neither |
| `COUNT` | intersection:num_tokens_2 | 198 | 1.004 [0.993–1.015], 0.4% slower; A/A 0.998 [0.967–1.031]; neither | 0.993 [0.978–1.009], 0.7% faster; A/A 1.006 [0.995–1.018]; neither |
| `COUNT` | intersection:num_tokens_3 | 83 | 1.006 [0.991–1.020], 0.6% slower; A/A 0.995 [0.963–1.029]; neither | 0.991 [0.974–1.008], 0.9% faster; A/A 1.005 [0.994–1.015]; neither |
| `COUNT` | intersection:num_tokens_>3 | 19 | 1.004 [0.984–1.025], 0.4% slower; A/A 1.005 [0.981–1.031]; neither | 0.992 [0.972–1.012], 0.8% faster; A/A 1.004 [0.993–1.016]; neither |
| `COUNT` | intersection_union | 40 | 1.023 [0.980–1.067], 2.3% slower; A/A 0.974 [0.935–1.015]; neither | 1.040 [0.997–1.084], 4.0% slower; A/A 1.004 [0.992–1.016]; neither |
| `COUNT` | intersection_union:num_tokens_2 | 10 | 1.046 [0.957–1.144], 4.6% slower; A/A 0.974 [0.935–1.013]; neither | 1.040 [0.996–1.085], 4.0% slower; A/A 1.007 [0.995–1.018]; neither |
| `COUNT` | intersection_union:num_tokens_3 | 20 | 1.020 [0.978–1.065], 2.0% slower; A/A 0.972 [0.933–1.013]; neither | 1.039 [0.998–1.082], 3.9% slower; A/A 1.004 [0.993–1.016]; neither |
| `COUNT` | intersection_union:num_tokens_>3 | 10 | 1.020 [0.977–1.064], 2.0% slower; A/A 0.979 [0.937–1.023]; neither | 1.046 [1.000–1.093], 4.6% slower; A/A 1.003 [0.983–1.023]; neither |
| `COUNT` | negated | 19 | 1.013 [0.973–1.054], 1.3% slower; A/A 0.987 [0.942–1.033]; neither | 1.046 [1.005–1.088], 4.6% slower; A/A 1.004 [0.977–1.032]; neither |
| `COUNT` | negated:num_tokens_2 | 4 | 1.014 [0.975–1.054], 1.4% slower; A/A 0.996 [0.946–1.049]; neither | 1.045 [1.003–1.088], 4.5% slower; A/A 1.000 [0.977–1.023]; neither |
| `COUNT` | negated:num_tokens_3 | 15 | 1.012 [0.972–1.054], 1.2% slower; A/A 0.986 [0.942–1.033]; neither | 1.046 [1.004–1.090], 4.6% slower; A/A 1.005 [0.977–1.035]; neither |
| `COUNT` | phrase | 300 | 1.004 [0.996–1.012], 0.4% slower; A/A 1.000 [0.971–1.031]; neither | 0.994 [0.980–1.009], 0.6% faster; A/A 1.002 [0.996–1.009]; neither |
| `COUNT` | phrase:num_tokens_2 | 198 | 1.004 [0.996–1.012], 0.4% slower; A/A 1.001 [0.971–1.032]; neither | 0.994 [0.981–1.008], 0.6% faster; A/A 1.003 [0.996–1.010]; neither |
| `COUNT` | phrase:num_tokens_3 | 83 | 1.002 [0.994–1.011], 0.2% slower; A/A 0.997 [0.966–1.028]; neither | 0.993 [0.976–1.010], 0.7% faster; A/A 1.002 [0.994–1.010]; neither |
| `COUNT` | phrase:num_tokens_>3 | 19 | 1.007 [0.996–1.019], 0.7% slower; A/A 1.009 [0.985–1.034]; neither | 0.994 [0.977–1.011], 0.6% faster; A/A 1.001 [0.995–1.007]; neither |
| `COUNT` | term | 201 | 0.996 [0.989–1.004], 0.4% faster; A/A 1.005 [0.978–1.034]; neither | 1.004 [0.968–1.041], 0.4% slower; A/A 1.004 [0.951–1.060]; neither |
| `COUNT` | term:frequent | 66 | 1.000 [0.988–1.011], same; A/A 1.002 [0.973–1.032]; neither | 1.005 [0.967–1.044], 0.5% slower; A/A 1.008 [0.952–1.067]; neither |
| `COUNT` | term:medium | 67 | 0.996 [0.988–1.004], 0.4% faster; A/A 1.005 [0.978–1.034]; neither | 1.004 [0.966–1.043], 0.4% slower; A/A 1.005 [0.950–1.062]; neither |
| `COUNT` | term:rare | 67 | 1.000 [1.000–1.000], same; A/A 1.005 [0.978–1.034]; neither | 1.005 [0.971–1.040], 0.5% slower; A/A 1.003 [0.952–1.057]; neither |
| `COUNT` | two-phase-critic | 1 | 1.026 [0.998–1.054], 2.6% slower; A/A 1.000 [0.955–1.048]; neither | 0.989 [0.951–1.029], 1.1% faster; A/A 1.013 [0.987–1.040]; neither |
| `COUNT` | union | 301 | 1.004 [0.991–1.017], 0.4% slower; A/A 0.997 [0.965–1.029]; neither | 0.993 [0.979–1.007], 0.7% faster; A/A 1.006 [0.995–1.018]; neither |
| `COUNT` | union:num_tokens_2 | 198 | 1.004 [0.991–1.016], 0.4% slower; A/A 0.998 [0.965–1.033]; neither | 0.992 [0.977–1.007], 0.8% faster; A/A 1.007 [0.995–1.019]; neither |
| `COUNT` | union:num_tokens_3 | 83 | 1.004 [0.989–1.019], 0.4% slower; A/A 0.997 [0.966–1.029]; neither | 0.994 [0.981–1.008], 0.6% faster; A/A 1.007 [0.993–1.021]; neither |
| `COUNT` | union:num_tokens_>3 | 20 | 1.003 [0.989–1.018], 0.3% slower; A/A 0.994 [0.971–1.017]; neither | 1.003 [0.989–1.017], 0.3% slower; A/A 1.004 [0.987–1.021]; neither |

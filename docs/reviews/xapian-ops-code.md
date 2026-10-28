# Code review: phase4/xapian-ops (prospect:c60547f)

Reviewer: code-reviewer, 2026-10-27, medium effort. Scope: the five risks named by the lead. Proof auditor graded the proof adequate.

## Verdict: APPROVE WITH CONDITIONS

One blocker-class finding (CR1, a silent wrong score with no enforcement); everything else holds.

## Build and baseline

`operators_test` built with Homebrew LLVM, `-O1 -fsanitize=address,undefined -fno-sanitize-recover=all`, own build dir (`build-p3`, deleted). Passes clean (669 oracle rows, counts as the ledger states, 18 s). Not rebuilt with TSan.

## Findings

### CR1 (blocking): a nested `xor_scorer` is a silent wrong score; the contract is a comment only

`operators.hh:261-263` says callers flatten; nothing enforces it, and the only flattening in the tree is the test's `flatten_xor` (`operators_test.cc:146`, used in `build` and `model`). There is no production builder in the repo, so the "real builder path" is the constructor itself.

Probe P1 (`.scratch/p3-review/probe.cc`, `probe P1`): `xor_scorer(xor_scorer(a,b), c)` over 2,000 docs against the flattened model. The matching set is the same (parity is associative), but 740 of 1,185 documents get the wrong score: a document in a, b and c scores `c` instead of `a+b+c`, and one in a and b only is dropped from the inner and scored by `c` alone, never `a+b+c` as Xapian sums. No error, no assert. S2/S3 (the real builder) will be written by someone who reads only the header; the ledger's "Compatibility notes" line is the only other place the rule lives.

Fix (small): make the invariant structural. In the `xor_scorer` constructor, splice the children of any child that is itself an `xor_scorer` (`dynamic_cast`, move its `children_`), or add a `make_xor(children)` that does and make the constructor private; add a test that constructs the nested form directly and compares to the flat model (P1 is that test). Cost: a few lines, no hot path.

### CR2 (non-blocking): SYNONYM df rule is well determined by the rows; large N and half-way cases untested

Probe P2 (`probe P2`): the 74 term-only SYNONYM rows run against nine df rules built from the same candidate pool the fit could have chosen. Results: nearest one-shot 74/74 (round-the-miss is algebraically the same, 74/74); floor 44, ceil 44, pairwise-rounded 68 (differs in 6 rows), pairwise-floor 35, pairwise-ceil 34, capped sum 15, max df 14. So the rows do discriminate every plausible alternative, and pairwise integer rounding is the only near miss (6 rows decide it). Closest fractional part to .5 is 0.0135 and 13 rows lie within 0.1, so the rounding threshold is pinned to within about 0.014. Weakness: all rows come from one 600-document corpus, so behavior at large N (double rounding of `N * (1 - miss)`, where the ledger's own note about 1e-9 near-halves applies) is not exercised by any oracle row; the property tests use the same formula as the code (`synonym_estimate`), so they cannot catch a wrong rule. Follow-up: record a second corpus size in the oracle when the collector work (P4) adds vectors.

### CR3 (non-blocking): ELITE_SET ties are a known divergence from Xapian, not a test gap

The mutant "ties keep the later child" is killed. I rebuilt the test against a copy of the header with that mutation (`.scratch/p3-review/mut.sh`, `mut/include/...`, worktree header untouched): `mut_test` fails in `check_scorer` (`operators_test.cc:313,321,338,340`) in the dedicated ELITE_SET block (`:788-812`, t1..t12 tie at df 3, sizes 1, 3, 12), so the pin is real.

The 4 skipped rows (probe P3, `probe P3`): enumerating the tied-candidate subsets that reproduce Xapian's hits gives, per row, tied {0,4,9} pick 2 -> {0,4}; tied {0,2,7} pick 2 -> {0,2}; tied {0,1,2} pick 2 -> {0,2}; tied {0,1,3} pick 1 -> {1}. Xapian's choice is neither the earlier nor the later children. Probe P3b: ranking by each term's `max_score_total` instead of idf explains fewer rows (53/87 vs 85/87), so it is not the weight bound either. The ledger's statement stands. Consequence for the architect: on a query whose ELITE_SET cut falls among terms of equal df and wqf, prospect keeps different terms than Xapiand does today, so result sets can differ (4 of 87 term-only recorded rows, 5%). Xapiand's default text operator emits ELITE_SET (32 nodes in 2 suite requests), so this should be a stated, accepted divergence in the design, not only in the ledger.

### CR4 (no finding): SCALE_WEIGHT margins

Probe P4 (`probe P4`): 49.9 million samples of (child score c, factor f, threshold s), f log-uniform over 2^-1000..2^1000 including subnormal products, c and s within a few ulps of the boundary: 0 cases where `fl(c*f) > s` but `c <= nextafter(nextafter(s/f))`. Bounds: `scaled` is monotone so `score*f <= bound*f`; a zero factor returns 0 everywhere including for an infinite child bound (no NaN), and the zero-scale child is skipped by `set_min_competitive_score` only when it cannot matter. Zero-as-FILTER is covered by 24 oracle FILTER rows plus the random trees. Minor: a factor of +infinity passes the `>= 0` check and gives NaN for a child score of exactly 0 (`0 * inf`); Xapian's behavior for it is unrecorded. Reject non-finite factors in the constructor when this is touched for CR1.

### CR5 (no finding): unbounded source and pruning

Probe P5 (`probe P5`): 776 random trees over 600 and 9,000 documents with four sources (two unstated, one tight stated bound 2.0, one loose 5.0) as leaves, inside OR, AND, AND_MAYBE, AND_NOT, MAX, XOR (flat), SCALE with factors 0, 1e-3, 0.5, 2, 1e3, to depth 3, top-k at 1, 3, 10, 50 counted and pruned: 0 mismatches against a brute-force model, under ASan/UBSan. Reading the consumers of bounds (`search.hh:621-630, 710-725, 787, 1000-1015, 1542-1593`, `side_minimum`): an infinite bound only ever appears in `sum_upper(x + inf) <= min`, which is false, and never in a subtraction (`side_minimum` gets `s - inf = -inf`, returns 0); the MaxScore `below[]` sums only non-essential clauses, which exclude any infinite one. So no path turns infinity into NaN or into a drop. Residual contract, not enforced: a source that states a `max_weight` smaller than its `weight` prunes wrongly (as Xapian does); worth one sentence on `value_source`.

## Not blocking, brief

1. No production builder exists in this branch; CR1 matters when S2/S3 adds one, which is why it should be structural now.
2. `value_source` could static-document that `weight <= max_weight` when stated.

## Probes kept (in `.scratch/p3-review/`)

`probe.cc` (P1 nested XOR, P2 SYNONYM df rules, P3/P3b ELITE ties, P4 SCALE margins, P5 unbounded-source trees), `build.sh`, `mut.sh` with `mut/include/prospect/operators.hh` (ELITE later-child mutant), binaries `probe`, `mut_test`, logs.

## Conditions

1. CR1: enforce flattening in `xor_scorer` (or a factory) and add the nested-construction test; re-check by running `probe P1` (expect 0 wrong) and the new test with the fix reverted.
2. CR3: the architect records the ELITE_SET tie divergence in the design (no code).

## Not run

TSan; the full `ops_mutants.py` (only the ELITE tie mutant was rebuilt); CI; other platforms; Xapian source (not read); a second oracle corpus; a scale-by-infinity oracle row.

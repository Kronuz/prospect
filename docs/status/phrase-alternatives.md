> Status: CURRENT (2026-10-29) branch `phase8/phrase-alternatives` (from main `prospect:828cd4a`); workstream phrase-alternatives, developer.

# PHRASE and NEAR over alternatives

## State

`multi_phrase_scorer` (`include/prospect/operators.hh`) is built and tested; Xapiand repins to it for gate 7a L1.

## Done

- `multi_phrase_scorer`, `phrase_slot`: a phrase whose positions hold alternatives (docs/mechanisms/query-operators.md).
- `test/multi_phrase_test.cc`: fixed cases and 600 random phrases against a brute-force model (documents and scores; next, advance,
  two-phase, top-k counted and not, under a conjunction).
- Mutants, each killed (`.scratch/mutants.sh`): offset off by one, NEAR window `<=`, NEAR positions not distinct, score halved,
  every candidate accepted, a slot's terms not merged, only the first term's positions read.

## Next

Nothing in this repository; the Xapiand build reads it through `strata/search.cc`.

## Blocked on

Nothing.

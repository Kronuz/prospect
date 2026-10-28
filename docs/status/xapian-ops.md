# xapian-ops: P3, the operators Xapiand's query language needs

> Status: CURRENT (2026-10-27) in progress on `phase4/xapian-ops`.

- Task (the lead's, 2026-10-27): step P3 of strata's `docs/design/xapiand-on-strata.md`: SYNONYM, XOR, MAX,
  SCALE_WEIGHT and ELITE_SET scorers with oracle vectors, and the scorer over a host source, scoped by the
  engine census (Xapiand `docs/dev/engine-census.md`) and by every operator `src/query_dsl.cc` can emit.
- Branch / worktree: `phase4/xapian-ops` at `/Users/kronuz/code/Kronuz/prospect-wt/xapian-ops`, from main
  `9de01a1`. Oracle side: `strata-oracle` branch `ops-vectors` at
  `/Users/kronuz/code/Kronuz/strata-oracle-wt/ops` (not merged).
- How it works: `docs/mechanisms/query-operators.md`.

## Done criteria

- O1. Oracle parity per operator: match count and top-50 weights of Xapian's recorded rows, counted and pruned.
- O2. Property tests: random operator trees against a brute-force model; every bound holds.
- O3. A mutant per operator rule that the tests catch.
- O4. ASan+UBSan and TSan, each in its own build directory with its own dependencies.
- O5. CI green on the head.

## Scope: what was built and what was not

What the sources say (Xapiand `src/query_dsl.cc:213-336`, `src/engine/query.h`, `src/engine/xapian/query.cc`,
`docs/dev/engine-census.md`):

| Operator | DSL can emit | Census (339 suite requests) | Decision |
|---|---|---|---|
| SYNONYM | `_synonym` modifier; the combiner of every wildcard, partial and edit-distance expansion; text default operator | 3 nodes (wildcard 2, edit distance 1) | Built |
| ELITE_SET | `_elite_set` operator and modifier; text default operator | 32 nodes in 2 requests | Built |
| MAX | `_max` modifier; text default operator | 0 | Built (the DSL emits it and documents it) |
| XOR | `_xor` operator | 0 | Built (the DSL emits it and documents it) |
| SCALE_WEIGHT | `_scale_weight` goes through the generic list constructor, which Xapian refuses (`op not valid with a list of subqueries`, the SCALEN rows); `engine::Query::scale` has no caller; the docs entry is empty | 0 | Built anyway: 40 lines, and FILTER (11 requests) is `AND(a, SCALE_WEIGHT(b, 0))` (the vectors show the two equal), so the seam needs the node |
| AND_NOT as a node | `_not` is `match_all AND_NOT x`, `_and_not` | 0 | Exists: `exclusion_scorer`. Needs a match-all scorer, which is S2's |
| Host source | `MultipleValueRange`, `MultipleValueGE`, `MultipleValueLE`, `GeoSpatialRange` | Range 6 requests, GE 2, Geo 4, LE 0 | Built, generic over `slot_values` and `value_source` |

Deferred, with the evidence:

1. SYNONYM over a non-term child (AND, PHRASE). Xapiand's SYNONYM children are terms in every use above; the
   vectors record 12 such rows (`(SYNONYM (AND a b) c)`, `(SYNONYM (PHRASE 2 a b) c)`) and the weights follow
   no term formula. Build it when a use appears.
2. ELITE_SET keys for composite children (20 recorded rows): the choice follows no simple rule over the
   leaf weights (sum and maximum of idf tried; 12 of 20 at best). `elite_set` takes the key from the caller.
   The census saw terms only (parse-v2's `elite_set` default operator gives terms and phrases outside the set).
3. Match estimates (lower, estimated, upper) of the new operators: recorded at `check_at_least` 0 in the
   oracle file, not asserted; they belong to the collector work (P4).
4. A cursor-style posting source (`next`, `skip_to`, `check`): D4 amended it to a value-slot filter, which is
   what the three census sources are. `MultipleValueLE` needs nothing extra.
5. Values reader adapter: P1 (`phase4/values-bytes`) is not on main. `source_scorer` is generic over a small
   concept (`next_present`, `value`, `count`); S3 wraps P1's reader in it.

## State

Operators, vectors, tests and mutants committed. Sanitizers and CI: see below.

## Compatibility notes for the lead

- `MultipleValueRange::weight` returns 1.0 and `max_weight()` is not overridden (0), so Xapian prunes on a
  bound that understates the weight. `source_scorer` treats 0 as unbounded and never prunes on it: scores are
  exact, and any difference in Xapian's own pruning (matches dropped, percent) is not reproduced.
- ELITE_SET among equal term weights: Xapian's pick follows no order of the query (in the tied rows it kept
  a later term over an earlier one, and in another the middle one); ours is the earlier. Only terms of equal
  df and wqf at the edge of the set can differ; the 4 such rows are skipped by the test.
- SYNONYM's df is computed in double and rounded to nearest; a union estimate within 1e-9 of a half could round
  the other way. The nearest vector row is 0.0135 from a half.
- A nested XOR must be flattened by the builder (`flatten_xor` in the test shows how).

## Blocked on

Nothing.

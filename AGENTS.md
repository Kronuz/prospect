# AGENTS.md — prospect

The search library of the Elysian stack: the search layout (terms, postings, scoring, queries,
aggregation, reordering) on [strata](https://github.com/Kronuz/strata)'s segment core, built
phase by phase under measured gates. Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for what
is prospect's, and strata's [PLAN](https://github.com/Kronuz/strata/blob/main/docs/PLAN.md) and
[ARCHITECTURE](https://github.com/Kronuz/strata/blob/main/docs/ARCHITECTURE.md) for the phases
and what the two share, before changing anything.

## Repo map

```
include/prospect/ The library (header-only), namespace prospect, target prospect::prospect.
                  Links strata::core, fetched pinned by commit (CMakeLists.txt). strata.hh
                  brings core's names into prospect; reader.hh holds search_layout and the
                  inverted plugin; index.hh the search layout's index.
test/             ctest suites; *_mapped variants rerun under faultfs crash points.
                  order_source_negative proves a unit without reorder.hh fails to compile.
fuzz/             libFuzzer targets (segment, postings) and their corpora; each corpus
                  replays as a ctest, PROSPECT_FUZZ=ON (Clang) builds the fuzzers.
bench/            Gate benchmarks: engines/ (Lucene, Tantivy, Xapian harnesses for
                  search-benchmark-game), phase0/, phase2/ (gate.py), quick/ A/Bs, soak/.
tools/            prospect_engine.cc (the harness's engine), parity.cc (oracle ranks),
                  keybench.cc (T5's key lookups on a search index), coverage, vm/ (gate VM).
docs/             ARCHITECTURE; design/ (phase 4), gates/ (phase 2, phase 3's ratchets),
                  reviews/, perf/, status/ (search ledgers), testing/parity.md.
```

Shared documents stay in strata and are linked from here: PLAN, BENCHMARKS, TESTING,
format-13.md (the segment format version covers both repositories), phase-2.md (the segment
frame and the first search design), reordering.md (the docids section is the core's),
layers.md and core-split.md (the plugin seam), and the reviewers' `calibration.md`. The lead's
workstream index is strata's `docs/status/README.md`.

## Renamed from strata, and names kept

The search side's names say prospect. Result files, status ledgers, and reviews recorded before
the rename keep their names, so a past command in them names the old one:

| Before | Now |
|---|---|
| `strata_engine` (tool, CMake target) | `prospect_engine` |
| harness engine `engines/strata`, labels `strata` | `engines/prospect`, `prospect` |
| `engines/strata-merged`, `strata-merged` | `engines/prospect-merged`, `prospect-merged` |
| `strata-a`, `strata-b` (bench/ab.sh) | `prospect-a`, `prospect-b` |
| `gate.py --strata NAME` | `gate.py --prospect NAME` |
| `STRATA_THREADS`, `STRATA_BUDGET_MB`, `STRATA_BUFFER_MB`, `STRATA_AUTO_MERGE`, `STRATA_IDENTITY_SEED`, `STRATA_DELETE_EVERY`, `STRATA_REORDER`, `STRATA_REORDER_THREADS`, `STRATA_DUMP_COUNT` (prospect_engine) | the same names with `PROSPECT_` |
| `STRATA_CRASH_POINTS`, `STRATA_BG_CRASH_POINTS`, `STRATA_LIFECYCLE_REORDER`, `STRATA_CHURN_CYCLES`, `STRATA_WRITE_FIXTURES`, `STRATA_FUZZ_VERDICTS` (prospect's tests and fuzzers) | the same names with `PROSPECT_` |
| `STRATA_EXPERIMENT_NO_EQUAL_SKIP` (search.hh) | `PROSPECT_EXPERIMENT_NO_EQUAL_SKIP` |

The scripts that read results (`bench/phase2/gate.py`, `baseline.py`, `bench/ab.py`,
`bench/index_gate.py`, `bench/quick/engines_ab.py`) read the old engine labels as the new ones,
so recorded runs stay comparable (`gate.canon`); a baseline file or `i1-strata.time` from before
the rename works unchanged.

Kept as `STRATA_*` because strata's code reads them, or because the thing they name is strata's:

- `STRATA_FAULTFS_MAP`: read by strata's `faultfs.hh`; the `*_mapped` tests set it.
- The gate VM's `STRATA_VM_*`, `STRATA_BSD_*`, and `STRATA_TRIALS_LOG` (`tools/vm/`), and the VM's
  `strata` user and hostnames: the VM is strata's machine, shared by both repositories.
- `STRATA_WRITE_FIXTURES` in strata's own tests writes strata's fixtures; prospect's tests read
  `PROSPECT_WRITE_FIXTURES` for theirs.

Build options are prospect's (`PROSPECT_FUZZ`, `PROSPECT_FIXTURES`).

## Rules

- **Clean room.** Never read Xapian's or Xapiand's GPL sources (including
  `sortable_serialise`). Xapian behavior is learned only from oracle vectors produced
  by a separate harness. Permissive implementations (Lucene, Tantivy, PISA) and any
  literature are open to read; code ported rather than reimplemented keeps its license
  notice (NOTICE, LICENSES/).
- **One implementation per mechanism.** Compatibility with Lucene, Tantivy, or Xapian is
  a preset of one implementation, never a parallel code path. Lifecycle mechanisms are
  strata's: extend the core through its hooks, never re-implement them here.
- **Limits are load-bearing.** Every cap names the mechanism it protects, and is at
  least the largest of Lucene's, Tantivy's, and Xapian's.
- **Gates are measured.** A claim in a gate report carries its platform, commit, and
  trial count. Iterate on small samples (about 100,000 documents) against the fastest
  baseline; one larger run confirms a gate row (strata's PLAN.md, "Targeted runs").
- **Byte identity.** A change that is not meant to change the format keeps `fixtures_test`
  passing and keeps `prospect_engine index` output byte for byte under `PROSPECT_IDENTITY_SEED`.
- **Stage explicit paths.** Never `git commit -a`; reviewers may be editing alongside.

## Verify a change

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
```

To work on strata and prospect together, point the fetch at a strata checkout:
`-DFETCHCONTENT_SOURCE_DIR_STRATA=../strata`. A change to the core lands in strata first; then
prospect repins `GIT_TAG` to that commit.

Sanitizer and FreeBSD coverage come from CI; `cpp-libraries/tools/preflight.sh` runs the
same matrix locally.

# Why single-thread indexing peaks at 1.5x its budget

Exploration branch `explore/index-memory`; no fix lands from it. Question: prospect keyed indexing
at one thread peaks at 1.57x the node budget at 5M (3,221 MiB on 2,056 MiB) and 1.52x at 1M, past
the 1.1x limit, and with merges off the same (probe 261110-022424-057569).

## Answer

The budget misses almost nothing: at the peak it has drawn 1,532 MiB against 1,578 MiB of RSS
(1M, budget 1,039 MiB). The overshoot is the budget's own policy: the buffer is allowed to fill
the whole budget (`tools/prospect_engine.cc` sets `buffer_share = capacity`), and the flush then
draws its working set on top, forced past the capacity (`flush_charge::grow`, strata
`budget.hh`). The flush's 403 MiB at 1M is mostly the positions spill (`chunked_spill`, held in
memory until copied after the postings; the section is 284 MiB) plus about 120 MiB of builder
scratch.

## Evidence (1M keyed, one thread, budget 1,039 MiB, VM, light jobs)

Sampler: `PROSPECT_MEM_LOG` in `tools/prospect_engine.cc`, 100 ms, `tools/vm/index_mem.sh`.
Series in `docs/perf/index-memory/`.

| run | job | peak RSS | budget peak | heap in use / mmapped | ratio to budget |
| --- | --- | --- | --- | --- | --- |
| flush trigger 100% of the budget (as gated) | 261111-044412-0f0818, repeat 261111-045331-c505f3 | 1,578 MiB | 1,532 MiB | 1,340 / 279 MiB | 1.52x |
| flush trigger 70% (`PROSPECT_SHARE_PCT=70`) | 261111-045333-430cd7 | 1,107 MiB | 1,121 MiB | 934 / 257 MiB | 1.07x |

At 5M (budget 2,056 MiB, trigger 65%, job 261111-045716-03d9a7): peak RSS 2,095 MiB (1.02x),
budget peak 2,131 MiB, 5 segments, 3 min 44 s wall (not a timing claim), against the 3,221 MiB
(1.57x) of the 100% trigger (phase3-ratchets A4).

Timeline of the 100% run: the buffer charges 1,035 MiB by 19.4 s; the flush starts at 20.4 s with
1,129 MiB used and the document count frozen at 854,999; the charge climbs 1,129 to 1,532 MiB over
10 s (the spill), RSS 1,194 to 1,578; the copy at 32 s releases it. After the flush RSS drops to
1,020 MiB while glibc keeps 907 MiB free in the arena: retention is large but reused by the next
buffer, so it does not set the peak here. The first segment's sections (job
261111-045331-c505f3): positions 284 MiB, postings 173 MiB, file 483 MiB.

## Fix

Reserve the flush's share in the trigger. Either the gate engine's `o.buffer_share`
(`tools/prospect_engine.cc:139`, about capacity/1.5) or, for every user of a budget, strata's
`needs_flush` (`include/strata/core/index.hh:2410`, `buffered + map >= capacity / (1 + k)`, k the
flush working set over the buffer, 0.39 at 1M, 0.57 at 5M) or `pressure(reserve)`. Stream the
positions to a temporary file (`file_spill` exists in `section_io.hh`) instead of
`chunked_spill` (`index.hh:4559`) to remove the 284 MiB and keep the full buffer. The 70% run
makes the same two segments at 1M.

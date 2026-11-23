# Source layout

Where the code lives, for the headers that were restructured. `docs/SPLITTING.md` in cpp-libraries
is the rule: a piece is a unit someone opens alone, and a file over 1,000 lines stays whole only
when it is one coherent unit.

## tuple_buffer.hh and tuple_buffer_*.hh

`include/prospect/tuple_buffer.hh` keeps its path and is the header `inverter.hh` includes. It
holds the namespace-scope helpers (`tuple_detail`: the chunked vector, the byte log, the hash,
LEB128, the bytes arena) and `class detail::tuple_buffer` with its data members, nested types and
member declarations, then includes one header per pipeline stage after ingest, holding those
stages' member definitions. The pieces sit beside it with its name as prefix, as prospect's headers
are flat. Each includes `tuple_buffer.hh` and compiles alone.

| File | Stage | Holds |
|---|---|---|
| `tuple_buffer.hh` | ingest, flush planning | `add` and its bucket, arena and column accounting; `flush`, `plan`, `add_documents`, `field_plan`, `splitters`; the class body |
| `tuple_buffer_run.hh` | run building | `drain`, `group_run`, `split`, `composite`, `sort_groups`, `radix_sort`, `sort_run`, `key_at`, `bytes_of`, and the drain's scratch accounting |
| `tuple_buffer_codec.hh` | the run entry codec | `view`, `encode_entry`, `decode_entry`, `decode`, `relabel`: the bytes a run carries |
| `tuple_buffer_read.hh` | the read side | the k-way `merger`'s members, `each_term`, `postings_of`, `scan` |

The class keeps the nested types of every stage (the tuple, groups, run references, the entry
view, the merger's cursors and heap), since the members' declarations name them. The merger stays
a nested class whose members are defined in `tuple_buffer_read.hh`: `field_plan` and `flush`,
which stay in the class, construct one, so it is complete there. One-line accessors stay defined
in the class beside their declarations.

`tuple_buffer.hh` is 751 lines (was 1,229), 540 of them `tuple_buffer`'s body (was 1,024).

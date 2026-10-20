#!/usr/bin/env python3
"""Rewrites a strata patch so it applies to prospect (strata's docs/status/repo-split.md).

  git -C STRATA format-patch --stdout BASE..BRANCH -- PATHS... | replay_strata_patch.py > out.patch
  git -C PROSPECT am -3 out.patch

BASE must be at or after strata 3f88a15 (the core split; rebase older branches onto it first).
The rewrite is the one the split applied to prospect's tree, line by line, so context lines match:
`include/strata/search/` is `include/prospect/`, its headers' `namespace strata` is
`namespace prospect`, a caller's `strata::NAME` is `prospect::NAME` when prospect declares NAME,
and `STRATA_FIXTURES`/`STRATA_FUZZ` are `PROSPECT_`. What it cannot redo is context-dependent
(the sorted include blocks, `format::` qualified by owner in tests), so a hunk there is fixed by
hand. Patches to core paths belong to strata, not here.
"""

from __future__ import annotations

import re
import sys

# Names prospect declares at namespace scope, and its own nested namespaces (from the AST of
# every prospect header at the split). Core's names stay strata::.
PROSPECT_NAMES = frozenset(
    [
        "bm25",
        "bm25_xapian",
        "check",
        "check_report",
        "collect_top_k",
        "conjunction_scorer",
        "count",
        "disjunction_scorer",
        "doc_hit",
        "doc_t",
        "document",
        "equal_bound",
        "exact_count",
        "exclusion_scorer",
        "find_term",
        "hit",
        "index",
        "index_options",
        "inverted_builder",
        "inverted_plugin",
        "inverting_buffer",
        "live_scorer",
        "merge_segments",
        "merge_source",
        "operation",
        "phrase_scorer",
        "positions_reader",
        "posting_input",
        "postings_iterator",
        "records",
        "required_optional_scorer",
        "scheme",
        "scored_filter",
        "scorer",
        "scorer_builder",
        "scorer_ptr",
        "search",
        "search_count",
        "search_layout",
        "segment",
        "segment_snapshot",
        "segment_writer",
        "snapshot_df",
        "snapshot_statistics",
        "statistics",
        "sum_upper",
        "tail_impacts",
        "term_entry",
        "term_postings",
        "term_scorer",
        "term_table",
        "text_token",
        "top_collector",
        "top_k",
        "transaction",
        "weight",
        "aggregate",
        "bisection",
        "collapse",
        "docvalues",
        "exact",
        "ordered",
        "reorder",
        "sortkey",
        "text_detail",
        "tuple_detail",
    ]
)
PROSPECT_FORMAT = frozenset(
    [
        "absolute",
        "advance_bytes",
        "any_sized_impact",
        "bitset",
        "block_codec",
        "cf_is_df",
        "competitive",
        "counts_are_wdf",
        "decode_gaps",
        "decode_impacts",
        "decode_length",
        "decode_term_info",
        "deltas",
        "encode_gaps",
        "encode_impacts",
        "encode_length",
        "encode_sized_impacts",
        "encode_term_info",
        "get_bits",
        "impact",
        "max_block_codec",
        "max_length_of",
        "no_positions",
        "no_wdf",
        "put_bits",
        "seed_ks",
        "seed_pairs",
        "single_posting",
        "skip_sized_impacts",
        "term_flags",
        "term_info",
        "term_run",
        "skip",
        "detail",
    ]
)
PROSPECT_DETAIL = frozenset(["document_fields", "tuple_buffer", "writer_access"])

QUALIFIED = re.compile(r"\bstrata::([A-Za-z_]\w*)(?:::([A-Za-z_]\w*))?")
HEADER_NAMESPACE = re.compile(
    r"namespace strata(::\w+)? \{|\}  // namespace strata(::\w+)?$"
)


def requalify(m: re.Match[str]) -> str:
    first, second = m.group(1), m.group(2)
    tail = f"::{second}" if second else ""
    if first in PROSPECT_NAMES:
        return f"prospect::{first}{tail}"
    if (first == "format" and second in PROSPECT_FORMAT) or (
        first == "detail" and second in PROSPECT_DETAIL
    ):
        return f"prospect::{first}{tail}"
    return m.group(0)


def rewrite_code(text: str, in_prospect_header: bool) -> str:
    text = text.replace('"strata/search/', '"prospect/')
    text = text.replace("STRATA_FIXTURES", "PROSPECT_FIXTURES").replace(
        "STRATA_FUZZ", "PROSPECT_FUZZ"
    )
    if in_prospect_header:
        text = HEADER_NAMESPACE.sub(
            lambda m: m.group(0).replace("strata", "prospect"), text
        )
    text = text.replace("using namespace strata;", "using namespace prospect;")
    text = text.replace(
        "namespace f = strata::format;", "namespace f = prospect::format;"
    )
    return QUALIFIED.sub(requalify, text)


def main() -> int:
    in_prospect_header = False
    for line in sys.stdin:
        if line.startswith(
            ("diff --git ", "--- ", "+++ ", "rename from ", "rename to ")
        ):
            line = line.replace("include/strata/search/", "include/prospect/")
            if line.startswith("diff --git "):
                in_prospect_header = " b/include/prospect/" in line
            sys.stdout.write(line)
        elif line[:1] in (" ", "+", "-") and not line.startswith(("---", "+++")):
            sys.stdout.write(line[0] + rewrite_code(line[1:], in_prospect_header))
        else:
            sys.stdout.write(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())

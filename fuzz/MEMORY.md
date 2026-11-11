# fuzz_postings memory

Under ASan and UBSan, `fuzz_postings` grew past libFuzzer's 2,048 MB RSS limit within ten minutes.
The growth was not the postings decoder: strata's stored-section check decoded each document's frame
afresh, so one input allocated up to 189 MB (mean 19.9 MB) while at most 4.5 MB was live, and ASan's
quarantine kept the freed blocks. strata decodes each frame once in the check (strata:71750940), and
rejects a frame whose records overlap the next (strata:94480d52).

Before the fix (prospect:10870d9, strata 9d4630c; macOS x86_64, Homebrew clang 23, seed 1):


| Run | RSS over time (MiB) | Executions |
|---|---|---|
| ASan+UBSan as built, 10 min | 1,280 at 60 s, 1,881 at 240 s, peak 2,095 (default `-rss_limit_mb=2048` kills it near 9 min) | 4,914 (8/s) |
| Live heap at `-rss_limit_mb=768` | OOM after 32 inputs: 27.7 MB live (21 MB is libFuzzer's own), 243 MB quarantined | |
| ASan, `quarantine_size_mb=0` | 43 to 51, flat | 2,462 |
| ASan, `allocator_release_to_os_interval_ms=1000` | 1,112 at 30 s, 2,053 at 300 s | 2,547 |
| `-fsanitize=fuzzer` only | 78 to 104, flat after warm-up | 7,995 |
| ASan, strata check with frame cache | 325 at 30 s, 380 from 120 s on, peak 400 | 9,393 (31/s) |

Per-input allocation by the target (counting `operator new`, no sanitizer, 841 corpus inputs):
mean 19.9 MB, max 189 MB, while peak live is at most 4.5 MB; with the frame cache mean 4.1 MB,

At the landed pin, seven minutes held RSS at 368-388 MiB after warm-up, peak 405 MiB, 0 crashes;
one CPU-hour at strata:71750940 held 374-408 MiB.

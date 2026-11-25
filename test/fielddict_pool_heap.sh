#!/usr/bin/env bash
# fielddict_pool_heap.sh BUILD SRC OUT: fielddict_pool_heap.cc compiled against SRC's include/, with
# the dependency flags and libraries of BUILD's fielddict_test (a configured, built prospect build
# dir: `ninja -C BUILD fielddict_test`); OUT is the binary.
set -euo pipefail
build=$1 src=$2 out=$3
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$(dirname "$out")"
python3 - "$build" "$src" "$here/fielddict_pool_heap.cc" "$out" <<'PY'
import json, shlex, subprocess, sys
build, src, cc, out = sys.argv[1:5]
lines = subprocess.check_output(["ninja", "-C", build, "-t", "commands", "fielddict_test"], text=True).splitlines()
args = shlex.split(next(l for l in lines if " -c " in l and l.endswith("test/fielddict_test.cc")))
res, skip = [], False
for a in args:
    if skip:
        skip = False
    elif a in ("-o", "-MF", "-MT"):
        skip = True
    elif a not in ("-MD", "-c") and not a.endswith("fielddict_test.cc") and not (a.startswith("-I") and a.endswith("/include") and "_deps" not in a):
        res.append(a)
libs = [build + "/" + t for t in shlex.split(lines[-1]) if t.endswith(".a")]
cc_at = next(i for i, a in enumerate(res) if a.endswith("c++") or a.endswith("clang++"))  # past a ccache prefix
cmd = res[:cc_at + 1] + ["-I" + src + "/include"] + res[cc_at + 1:] + ["-o", out, cc] + libs
subprocess.check_call(cmd)
PY

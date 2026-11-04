#!/bin/bash
# mk.sh NAME [-Dflags...]: compile prospect_engine with flags and link as .scratch/eng-NAME
cd "$(dirname "$0")/rel" || exit 1
name=$1; shift
INC=$(grep '^CXX_INCLUDES' CMakeFiles/prospect_engine.dir/flags.make | sed 's/^CXX_INCLUDES = //')
nice -n 19 /usr/bin/c++ $INC -O3 -DNDEBUG -std=gnu++20 "$@" -c ../../tools/prospect_engine.cc -o ../obj-$name.o || exit 1
/usr/bin/c++ -O3 -Wl,-search_paths_first ../obj-$name.o -o ../eng-$name _deps/compressors-build/libcompressors.a _deps/lz4-build/liblz4.a _deps/zstd-build/lib/libzstd.a _deps/threadpool-build/libthreadpool.a _deps/snowball-build/libsnowball.a

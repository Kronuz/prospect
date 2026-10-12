# strata as a search-benchmark-game engine; copy to engines/strata.
# STRATA_SRC points at a strata checkout; LIBS at sibling Kronuz library checkouts.
STRATA_SRC ?= $(HOME)/libs/strata
LIBS ?= $(HOME)/libs
OVERRIDES := $(foreach l,docset bitpack fst task hashes residency varint termdict unicode snowball analysis,-DFETCHCONTENT_SOURCE_DIR_$(shell echo $(l) | tr a-z A-Z)=$(LIBS)/$(l))

clean:
	rm -fr idx build

# Always rebuilds through ninja, which tracks sources; build/strata_engine existing is
# not proof that it is current.
.PHONY: compile build/strata_engine
compile: build/strata_engine

build/strata_engine:
	[ -f build/build.ninja ] || cmake -S $(STRATA_SRC) -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-march=native" $(OVERRIDES)
	cmake --build build --target strata_engine

index: idx

idx: | build/strata_engine
	@echo "--- Indexing strata ---"
	rm -fr idx && mkdir -p idx
	build/strata_engine index idx < $(CORPUS)

serve: | build/strata_engine
	@build/strata_engine serve idx

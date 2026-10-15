# strata as a search-benchmark-game engine; copy to engines/strata.
# STRATA_SRC points at a strata checkout. LIBS, when set, names a directory of sibling
# Kronuz library checkouts used instead of the pinned commits; unset, the pins build.
STRATA_SRC ?= $(HOME)/libs/strata
LIBS ?=
# Only the checkouts that exist under LIBS.
OVERRIDES := $(if $(LIBS),$(foreach l,docset bitpack fst task hashes residency varint termdict unicode snowball analysis fusefilter completion metrics,$(if $(wildcard $(LIBS)/$(l)),-DFETCHCONTENT_SOURCE_DIR_$(shell echo $(l) | tr a-z A-Z)=$(LIBS)/$(l))))

clean:
	rm -fr idx build

# compile always rebuilds through ninja, which tracks sources: build/strata_engine
# existing is not proof that it is current. serve writes only the engine's answers to
# stdout, so it runs the binary as built.
.PHONY: compile clean index serve
compile:
	@[ -f build/build.ninja ] || cmake -S $(STRATA_SRC) -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-march=native" $(OVERRIDES)
	@cmake --build build --target strata_engine

build/strata_engine:
	$(MAKE) compile

index: idx

idx: | build/strata_engine
	@echo "--- Indexing strata ---"
	rm -fr idx && mkdir -p idx
	build/strata_engine index idx < $(CORPUS)

serve:
	@build/strata_engine serve idx

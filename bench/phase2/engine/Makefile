# prospect as a search-benchmark-game engine, named prospect in the harness and its results;
# copy to engines/prospect. PROSPECT_SRC points at a prospect checkout. LIBS, when set, names a
# directory of sibling Kronuz library checkouts (strata among them) used instead of the pinned
# commits; unset, the pins build.
PROSPECT_SRC ?= $(HOME)/libs/prospect
LIBS ?=
# Only the checkouts that exist under LIBS.
OVERRIDES := $(if $(LIBS),$(foreach l,strata docset bitpack fst task hashes residency varint termdict unicode snowball analysis fusefilter completion metrics,$(if $(wildcard $(LIBS)/$(l)),-DFETCHCONTENT_SOURCE_DIR_$(shell echo $(l) | tr a-z A-Z)=$(LIBS)/$(l))))

clean:
	rm -fr idx build

# compile always rebuilds through ninja, which tracks sources: build/prospect_engine
# existing is not proof that it is current. serve writes only the engine's answers to
# stdout, so it runs the binary as built.
.PHONY: compile clean index serve
compile:
	@[ -f build/build.ninja ] || cmake -S $(PROSPECT_SRC) -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-march=native" $(OVERRIDES)
	@cmake --build build --target prospect_engine

build/prospect_engine:
	$(MAKE) compile

index: idx

idx: | build/prospect_engine
	@echo "--- Indexing prospect ---"
	rm -fr idx && mkdir -p idx
	build/prospect_engine index idx < $(CORPUS)

serve:
	@build/prospect_engine serve idx

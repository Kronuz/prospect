# strata after a full merge, as a search-benchmark-game engine: a copy of engines/strata's
# index force-merged to one segment, served by the same build (the "after merging" rows,
# against baselines that force-merge in their builders).
STRATA ?= ../strata

.PHONY: compile clean index serve
compile:
	@$(MAKE) -s -C $(STRATA) compile

clean:
	rm -fr idx

index:
	@echo "--- Indexing strata (merged) ---"
	rm -fr idx && cp -a $(STRATA)/idx idx
	$(STRATA)/build/strata_engine merge idx

serve:
	@$(STRATA)/build/strata_engine serve idx

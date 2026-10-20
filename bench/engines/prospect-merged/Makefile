# prospect after a full merge, as a search-benchmark-game engine: a copy of engines/prospect's
# index force-merged to one segment, served by the same build (the "after merging" rows,
# against baselines that force-merge in their builders).
PROSPECT ?= ../prospect

.PHONY: compile clean index serve
compile:
	@$(MAKE) -s -C $(PROSPECT) compile

clean:
	rm -fr idx

index:
	@echo "--- Indexing prospect (merged) ---"
	rm -fr idx && cp -a $(PROSPECT)/idx idx
	$(PROSPECT)/build/prospect_engine merge idx

serve:
	@$(PROSPECT)/build/prospect_engine serve idx

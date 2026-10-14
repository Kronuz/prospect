XAPIAN_PREFIX ?= $(HOME)/opt/xapian-2.1.0
CXXFLAGS := -std=c++20 -O3 -march=native -DNDEBUG -I$(XAPIAN_PREFIX)/include
LDFLAGS := -L$(XAPIAN_PREFIX)/lib -Wl,-rpath,$(XAPIAN_PREFIX)/lib -lxapian

clean:
	rm -fr idx idx.build xapian_engine

compile: xapian_engine

xapian_engine: xapian_engine.cc
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

index: idx

idx: xapian_engine
	@echo "--- Indexing Xapian 2.1.0 ---"
	rm -fr idx idx.build
	./xapian_engine index idx < $(CORPUS)
	rm -fr idx.build

serve: xapian_engine
	@./xapian_engine serve idx

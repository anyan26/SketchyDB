CXX ?= c++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS ?= -Iinclude
LDFLAGS ?=
LDLIBS ?=
SKDB_USE_DUCKDB ?= 0
DUCKDB_PREFIX ?=
PERF_TRIALS ?= 5
PERF_ROWS ?= 500000
PERF_DISTINCT ?= 100000
PERF_BATCH_SIZE ?= 1000
PERF_SEED ?= 1337
SKDB_HASH_SEED ?=

ifeq ($(SKDB_USE_DUCKDB),1)
CPPFLAGS += -DSKDB_USE_DUCKDB
LDLIBS += -lduckdb
ifneq ($(DUCKDB_PREFIX),)
CPPFLAGS += -I$(DUCKDB_PREFIX)
LDFLAGS += -L$(DUCKDB_PREFIX) -Wl,-rpath,$(DUCKDB_PREFIX)
ifneq ($(wildcard $(DUCKDB_PREFIX)/include),)
CPPFLAGS += -I$(DUCKDB_PREFIX)/include
endif
ifneq ($(wildcard $(DUCKDB_PREFIX)/lib),)
LDFLAGS += -L$(DUCKDB_PREFIX)/lib -Wl,-rpath,$(DUCKDB_PREFIX)/lib
endif
endif
endif

BUILD_DIR := build
LIB_OBJECTS := $(BUILD_DIR)/sketchydb.o $(BUILD_DIR)/planner.o $(BUILD_DIR)/duckdb_backend.o $(BUILD_DIR)/hyperloglog.o

.PHONY: all clean test perf

all: $(BUILD_DIR)/libsketchydb.a $(BUILD_DIR)/sketchydb

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/sketchydb.o: src/sketchydb.cpp include/sketchydb.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/planner.o: src/planner.cpp src/planner.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/duckdb_backend.o: src/duckdb_backend.cpp src/duckdb_backend.hpp include/sketchydb.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/hyperloglog.o: src/hyperloglog.cpp src/hyperloglog.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/libsketchydb.a: $(LIB_OBJECTS)
	ar rcs $@ $^

$(BUILD_DIR)/shell.o: shell/shell.cpp include/sketchydb.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/sketchydb: $(BUILD_DIR)/shell.o $(BUILD_DIR)/libsketchydb.a
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/test_smoke.o: tests/test_smoke.cpp include/sketchydb.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/test_smoke: $(BUILD_DIR)/test_smoke.o $(BUILD_DIR)/libsketchydb.a
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/test_planner.o: tests/test_planner.cpp src/planner.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/test_planner: $(BUILD_DIR)/test_planner.o $(BUILD_DIR)/planner.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD_DIR)/test_hyperloglog.o: tests/test_hyperloglog.cpp src/hyperloglog.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/test_hyperloglog: $(BUILD_DIR)/test_hyperloglog.o $(BUILD_DIR)/hyperloglog.o
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(BUILD_DIR)/test_smoke $(BUILD_DIR)/test_planner $(BUILD_DIR)/test_hyperloglog
	./$(BUILD_DIR)/test_smoke
	./$(BUILD_DIR)/test_planner
	./$(BUILD_DIR)/test_hyperloglog

$(BUILD_DIR)/bench_count_distinct.o: tests/bench_count_distinct.cpp include/sketchydb.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/bench_count_distinct: $(BUILD_DIR)/bench_count_distinct.o $(BUILD_DIR)/libsketchydb.a
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

perf: $(BUILD_DIR)/bench_count_distinct
	SKDB_HASH_SEED="$(SKDB_HASH_SEED)" ./$(BUILD_DIR)/bench_count_distinct $(PERF_TRIALS) $(PERF_ROWS) $(PERF_DISTINCT) $(PERF_BATCH_SIZE) $(PERF_SEED)

clean:
	rm -rf $(BUILD_DIR)

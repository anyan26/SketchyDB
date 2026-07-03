CXX ?= c++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS ?= -Iinclude
LDFLAGS ?=
LDLIBS ?=
SKDB_USE_DUCKDB ?= 0
DUCKDB_PREFIX ?=

ifeq ($(SKDB_USE_DUCKDB),1)
CPPFLAGS += -DSKDB_USE_DUCKDB
LDLIBS += -lduckdb
ifneq ($(DUCKDB_PREFIX),)
CPPFLAGS += -I$(DUCKDB_PREFIX)/include
LDFLAGS += -L$(DUCKDB_PREFIX)/lib
endif
endif

BUILD_DIR := build
LIB_OBJECTS := $(BUILD_DIR)/sketchydb.o $(BUILD_DIR)/planner.o $(BUILD_DIR)/duckdb_backend.o

.PHONY: all clean test

all: $(BUILD_DIR)/libsketchydb.a $(BUILD_DIR)/sketchydb

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/sketchydb.o: src/sketchydb.cpp include/sketchydb.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/planner.o: src/planner.cpp src/planner.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/duckdb_backend.o: src/duckdb_backend.cpp src/duckdb_backend.hpp include/sketchydb.h | $(BUILD_DIR)
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

test: $(BUILD_DIR)/test_smoke
	./$(BUILD_DIR)/test_smoke

clean:
	rm -rf $(BUILD_DIR)

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g
WARNFLAGS ?= -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude
LDFLAGS ?=
LDLIBS += -pthread

BUILD_DIR ?= build
PREFIX ?= /usr/local
AR ?= ar

LIB_SOURCES := src/planning.cpp src/legacy.cpp src/json.cpp src/kunpeng.cpp src/api.cpp
LIB_OBJECTS := $(LIB_SOURCES:src/%.cpp=$(BUILD_DIR)/%.o)
LIBRARY := $(BUILD_DIR)/libasymsched.a

SERVER_OBJECTS := $(BUILD_DIR)/server.o $(BUILD_DIR)/server_main.o
CLI_OBJECTS := $(BUILD_DIR)/cli_main.o
TEST_SOURCES := tests/test_main.cpp tests/test_planning.cpp tests/test_legacy.cpp \
	tests/test_json.cpp tests/test_api.cpp
TEST_OBJECTS := $(TEST_SOURCES:tests/%.cpp=$(BUILD_DIR)/tests/%.o)

.PHONY: all clean test check install

all: $(BUILD_DIR)/asymsched-server $(BUILD_DIR)/asymsched-cli

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/tests:
	mkdir -p $(BUILD_DIR)/tests

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARNFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/tests/%.o: tests/%.cpp | $(BUILD_DIR)/tests
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARNFLAGS) -MMD -MP -c $< -o $@

$(LIBRARY): $(LIB_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/asymsched-server: $(SERVER_OBJECTS) $(LIBRARY)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/asymsched-cli: $(CLI_OBJECTS) $(LIBRARY)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/asymsched-tests: $(TEST_OBJECTS) $(LIBRARY)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

test check: $(BUILD_DIR)/asymsched-tests
	./$(BUILD_DIR)/asymsched-tests

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BUILD_DIR)/asymsched-server $(DESTDIR)$(PREFIX)/bin/
	install -m 0755 $(BUILD_DIR)/asymsched-cli $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -rf $(BUILD_DIR)

-include $(LIB_OBJECTS:.o=.d) $(SERVER_OBJECTS:.o=.d) $(CLI_OBJECTS:.o=.d) \
	$(TEST_OBJECTS:.o=.d)

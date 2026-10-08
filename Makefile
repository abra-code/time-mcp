# Builds time-mcp with GNU make 3.81 or later and the system's C and C++ compilers: what a Mac
# with Xcode or its Command Line Tools has, and a Linux system with its usual build package
# (build-essential on Debian and Ubuntu).
#
#   make                the server, build/time-mcp
#   make zonedump       the tool two of the tests use, build/zonedump
#   make test           build both and run the three tests (needs python3)
#   make test-protocol  one test; also test-tzif and test-zones
#   make install        copy the server to $(PREFIX)/bin
#   make clean          remove what was built
#
# Settings, given on the command line, as in: make ARCHS="arm64 x86_64"
#
#   BUILD_DIR           the folder everything is built in (default: build)
#   ARCHS               macOS: the processor types to build for (default: this Mac's)
#   MACOSX_DEPLOYMENT_TARGET
#                       macOS: the oldest macOS the program runs on (default: 12.0)
#   CFLAGS, CXXFLAGS    optimization and debugging options (default: -Os)
#   CPPFLAGS            options for every compilation (default: -DNDEBUG)
#   LDFLAGS             options for the links
#   PREFIX, DESTDIR     where install copies to (default: /usr/local)
#   PYTHON              the interpreter the tests run with (default: python3)

VERSION := 0.1.0

BUILD_DIR ?= build
PREFIX ?= /usr/local
PYTHON ?= python3

# Optimized for size: nothing the server does takes long enough for speed to matter.
CFLAGS ?= -Os
CXXFLAGS ?= -Os
CPPFLAGS ?= -DNDEBUG

# Object files go to $(BUILD_DIR)/obj and clean removes that folder, so the source folder
# itself must not be the build folder.
ifeq ($(filter-out . ./,$(strip $(BUILD_DIR))),)
  $(error BUILD_DIR must name a folder of its own, not the source folder)
endif

OBJ_DIR := $(BUILD_DIR)/obj

# Everything but main(), so the test tool is linked from the same object files as the server.
SHARED_OBJECTS := $(addprefix $(OBJ_DIR)/,McpStdio.o Platform.o TimeTools.o TimeZone.o yyjson.o)

# yyjson, vendored. The server reads and writes whole documents and nothing else, so the
# optional parts are left out (about half of the library). The header has to see the same
# choices as the library, so every compilation gets them.
YYJSON_OPTIONS := -Ivendor/yyjson -DYYJSON_DISABLE_UTILS=1 -DYYJSON_DISABLE_INCR_READER=1 \
    -DYYJSON_DISABLE_NON_STANDARD=1 -DYYJSON_DISABLE_FAST_FP_CONV=1

# For the project's own code only: yyjson is compiled the way its authors ship it.
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion

ifeq ($(shell uname -s),Darwin)
  # Nothing here needs a recent macOS, and 12.0 is the oldest target Xcode 27 builds for.
  MACOSX_DEPLOYMENT_TARGET ?= 12.0
  PLATFORM_OPTIONS := -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) $(addprefix -arch ,$(ARCHS))
  PLATFORM_LINK_OPTIONS := -Wl,-dead_strip
else
  # Every function and variable in a section of its own, so the link can leave out the unused.
  PLATFORM_OPTIONS := -ffunction-sections -fdata-sections
  PLATFORM_LINK_OPTIONS := -Wl,--gc-sections
endif

ALL_CFLAGS := -std=c99 $(YYJSON_OPTIONS) $(PLATFORM_OPTIONS) $(CPPFLAGS) $(CFLAGS)
ALL_CXXFLAGS := -std=c++17 -Isrc $(YYJSON_OPTIONS) $(WARNINGS) -fno-exceptions -fno-rtti \
    $(PLATFORM_OPTIONS) $(CPPFLAGS) $(CXXFLAGS)
ALL_LDFLAGS := $(PLATFORM_OPTIONS) $(PLATFORM_LINK_OPTIONS) $(CXXFLAGS) $(LDFLAGS)

.PHONY: all time-mcp zonedump test test-protocol test-tzif test-zones install clean FORCE
.DELETE_ON_ERROR:

all: time-mcp
time-mcp: $(BUILD_DIR)/time-mcp
zonedump: $(BUILD_DIR)/zonedump

$(BUILD_DIR)/time-mcp: $(OBJ_DIR)/main.o $(SHARED_OBJECTS)
	$(CXX) $(ALL_LDFLAGS) $^ -o $@

# Test tool: prints a zone's offsets over a range of instants, for comparison with an
# independent reader of the same database.
$(BUILD_DIR)/zonedump: $(OBJ_DIR)/zonedump.o $(SHARED_OBJECTS)
	$(CXX) $(ALL_LDFLAGS) $^ -o $@

# The one place the version number is written; main.cpp prints it for --version and reports
# it to clients.
$(OBJ_DIR)/main.o: ALL_CXXFLAGS += -DTIME_MCP_VERSION='"$(VERSION)"'

$(OBJ_DIR)/%.o: src/%.cpp $(BUILD_DIR)/build-settings.txt
	$(CXX) $(ALL_CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.o: test/%.cpp $(BUILD_DIR)/build-settings.txt
	$(CXX) $(ALL_CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/yyjson.o: vendor/yyjson/yyjson.c $(BUILD_DIR)/build-settings.txt
	$(CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

# The headers each object file was compiled from, written by the compiler (-MMD -MP).
-include $(wildcard $(OBJ_DIR)/*.d)

# make compares file dates only, so after a change of compiler, options or version number it
# would keep the old object files. The settings are therefore kept in a file that is rewritten
# only when they differ, and every object file depends on it. They reach the command through
# the environment, so no character in them needs quoting.
#
# make 3.81 compares dates in whole seconds, and an object file is rebuilt only when the
# settings file is newer. Object files of a build that ended in this very second would
# therefore be kept, now and in every later run, so the file is written a second later when
# there are object files.
export TIME_MCP_BUILD_SETTINGS := $(CC) $(ALL_CFLAGS) / $(CXX) $(ALL_CXXFLAGS) / $(ALL_LDFLAGS) / $(VERSION)

$(BUILD_DIR)/build-settings.txt: FORCE | $(OBJ_DIR)
	@printf '%s\n' "$$TIME_MCP_BUILD_SETTINGS" | cmp -s - $@ || { $(if $(wildcard $(OBJ_DIR)/*.o),sleep 1;) printf '%s\n' "$$TIME_MCP_BUILD_SETTINGS" > $@; }

$(OBJ_DIR):
	mkdir -p $@

# One target per test, so that "make -k test" runs the others when one fails.
test: test-protocol test-tzif test-zones

test-protocol: $(BUILD_DIR)/time-mcp
	$(PYTHON) test/test_time_mcp.py $(BUILD_DIR)/time-mcp

test-tzif test-zones: test-%: $(BUILD_DIR)/zonedump $(BUILD_DIR)/time-mcp
	$(PYTHON) test/test_$*.py $(BUILD_DIR)/zonedump $(BUILD_DIR)/time-mcp

install: $(BUILD_DIR)/time-mcp
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD_DIR)/time-mcp $(DESTDIR)$(PREFIX)/bin/time-mcp

clean:
	rm -rf $(OBJ_DIR) $(BUILD_DIR)/time-mcp $(BUILD_DIR)/zonedump $(BUILD_DIR)/build-settings.txt

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
# On a Mac, for Linux (see "Linux programs built on a Mac" below for what it needs):
#
#   make linux          build/linux-aarch64 and build/linux-x86_64, each with a static
#                       time-mcp and zonedump
#   make linux-aarch64  one of the two; also linux-x86_64
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
#   LINUX_TOOLCHAIN, LINUX_SDK
#                       make linux: the compiler's folder and the Linux libraries' folder
#                       (default: found by scripts/find-linux-toolchain.sh)

VERSION := 0.1.1

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

# Linux programs built on a Mac. "make linux" runs this makefile once per processor type with
# LINUX_ARCH set, each in a build folder of its own. Xcode's compiler cannot build for Linux,
# so the compiler is the clang of a swift.org toolchain and the libraries are those of the
# static Linux SDK of the same version (musl and libc++), all linked into the program: it
# needs nothing of the Linux system it runs on but the kernel and the time zone files.
LINUX_ARCHS := aarch64 x86_64

ifdef LINUX_ARCH
  ifeq ($(filter $(LINUX_ARCH),$(LINUX_ARCHS)),)
    $(error LINUX_ARCH is "$(LINUX_ARCH)"; this makefile builds for: $(LINUX_ARCHS))
  endif
  ifeq ($(LINUX_TOOLCHAIN)$(LINUX_SDK),)
    # Two lines, the toolchain's folder of tools and the SDK's folder; on failure nothing, and
    # the script has said what is missing.
    LINUX_FOUND := $(shell /bin/bash scripts/find-linux-toolchain.sh)
    LINUX_TOOLCHAIN := $(word 1,$(LINUX_FOUND))
    LINUX_SDK := $(word 2,$(LINUX_FOUND))
    ifeq ($(LINUX_SDK),)
      $(error No compiler and libraries for Linux were found)
    endif
  endif
  ifeq ($(and $(LINUX_TOOLCHAIN),$(LINUX_SDK)),)
    $(error LINUX_TOOLCHAIN and LINUX_SDK go together: give both or neither)
  endif
  LINUX_SYSROOT := $(LINUX_SDK)/$(LINUX_ARCH)
  # Not the compilers of the make that started this one, whatever it was given.
  override CC := $(LINUX_TOOLCHAIN)/clang
  override CXX := $(LINUX_TOOLCHAIN)/clang++
  TARGET_SYSTEM := Linux
  TARGET_OPTIONS := --target=$(LINUX_ARCH)-swift-linux-musl --sysroot=$(LINUX_SYSROOT)
  # The SDK keeps the start files and the compiler's support library in a folder of its own,
  # which clang finds only when told (-resource-dir). The libraries carry debugging
  # information, most of the program's size, which the link leaves out (--strip-all).
  TARGET_LINK_OPTIONS := -static -fuse-ld=lld -resource-dir $(LINUX_SYSROOT)/usr/lib/swift/clang \
      -Wl,--strip-all
else
  TARGET_SYSTEM := $(shell uname -s)
endif

ifeq ($(TARGET_SYSTEM),Darwin)
  # Nothing here needs a recent macOS, and 12.0 is the oldest target Xcode 27 builds for.
  MACOSX_DEPLOYMENT_TARGET ?= 12.0
  PLATFORM_OPTIONS := -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) $(addprefix -arch ,$(ARCHS))
  PLATFORM_LINK_OPTIONS := -Wl,-dead_strip
else
  # Every function and variable in a section of its own, so the link can leave out the unused.
  PLATFORM_OPTIONS := -ffunction-sections -fdata-sections $(TARGET_OPTIONS)
  PLATFORM_LINK_OPTIONS := -Wl,--gc-sections $(TARGET_LINK_OPTIONS)
endif

ALL_CFLAGS := -std=c99 $(YYJSON_OPTIONS) $(PLATFORM_OPTIONS) $(CPPFLAGS) $(CFLAGS)
ALL_CXXFLAGS := -std=c++17 -Isrc $(YYJSON_OPTIONS) $(WARNINGS) -fno-exceptions -fno-rtti \
    $(PLATFORM_OPTIONS) $(CPPFLAGS) $(CXXFLAGS)
ALL_LDFLAGS := $(PLATFORM_OPTIONS) $(PLATFORM_LINK_OPTIONS) $(CXXFLAGS) $(LDFLAGS)

.PHONY: all time-mcp zonedump test test-protocol test-tzif test-zones install clean FORCE
.PHONY: linux $(addprefix linux-,$(LINUX_ARCHS))
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

# The test tool is built too: a Linux program cannot be tested on the Mac that built it, so
# both go to a Linux system together.
linux: $(addprefix linux-,$(LINUX_ARCHS))

$(addprefix linux-,$(LINUX_ARCHS)): linux-%:
	$(MAKE) LINUX_ARCH=$* BUILD_DIR=$(BUILD_DIR)/linux-$* time-mcp zonedump

install: $(BUILD_DIR)/time-mcp
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD_DIR)/time-mcp $(DESTDIR)$(PREFIX)/bin/time-mcp

clean:
	rm -rf $(OBJ_DIR) $(BUILD_DIR)/time-mcp $(BUILD_DIR)/zonedump $(BUILD_DIR)/build-settings.txt
	rm -rf $(addprefix $(BUILD_DIR)/linux-,$(LINUX_ARCHS))

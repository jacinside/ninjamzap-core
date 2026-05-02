# video-sync integration tests — host build (macOS).
#
# Builds njclient + jnetlib + WDL + harness + Catch2 amalgamated into
# ./video_sync_tests. Server runs in Docker via docker/run-server.sh.
#
# Usage:
#   make            # build
#   make test       # build + run all scenarios (boots server in Docker)
#   make clean

CXX      ?= clang++
CC       ?= clang
CXXSTD   ?= -std=c++17
OPT      ?= -O0 -g
WARN     ?= -Wall -Wno-unused-parameter -Wno-deprecated-declarations -Wno-deprecated-copy

ROOT     := $(abspath $(CURDIR)/../..)
CORE     := $(ROOT)/core/ninjamclient/libninjamcore
WDL      := $(CORE)/WDL
JNET     := $(WDL)/jnetlib
HARNESS  := $(CURDIR)/harness
SCEN     := $(CURDIR)/scenarios
VENDOR   := $(CURDIR)/vendor
BUILD    := $(CURDIR)/build

# Homebrew on Apple Silicon
BREW_PREFIX ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)

INCLUDES := -I$(CORE) -I$(WDL) -I$(VENDOR) -I$(HARNESS) -I$(BREW_PREFIX)/include
DEFINES  := -DNINJAMCORE_HOST_BUILD=1 -D_DARWIN_C_SOURCE
CXXFLAGS := $(CXXSTD) $(OPT) $(WARN) $(INCLUDES) $(DEFINES)
CFLAGS   := $(OPT) $(WARN) $(INCLUDES) $(DEFINES)
LDFLAGS  := -L$(BREW_PREFIX)/lib
LIBS     := -lvorbis -lvorbisenc -logg -lpthread

# --- Core sources --------------------------------------------------------
CORE_CPP_OBJS := \
  $(BUILD)/core/njclient.o \
  $(BUILD)/core/mpb.o \
  $(BUILD)/core/netmsg.o \
  $(BUILD)/wdl/rng.o \
  $(BUILD)/wdl/sha.o \
  $(BUILD)/jnet/asyncdns.o \
  $(BUILD)/jnet/connection.o \
  $(BUILD)/jnet/listen.o \
  $(BUILD)/jnet/util.o

CORE_C_OBJS := \
  $(BUILD)/core/vorbis_encoder.o

# --- Harness + scenarios -------------------------------------------------
HARNESS_SRCS := $(wildcard $(HARNESS)/*.cpp)
SCEN_SRCS    := $(wildcard $(SCEN)/*.cpp)
HARNESS_OBJS := $(HARNESS_SRCS:$(HARNESS)/%.cpp=$(BUILD)/harness/%.o)
SCEN_OBJS    := $(SCEN_SRCS:$(SCEN)/%.cpp=$(BUILD)/scenarios/%.o)

CATCH_OBJ := $(BUILD)/vendor/catch_amalgamated.o

ALL_OBJS := $(CORE_CPP_OBJS) $(CORE_C_OBJS) $(HARNESS_OBJS) $(SCEN_OBJS) $(CATCH_OBJ)

# --- Targets -------------------------------------------------------------
.PHONY: all clean test

all: video_sync_tests

video_sync_tests: $(ALL_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(ALL_OBJS) $(LDFLAGS) $(LIBS)

# Core .cpp
$(BUILD)/core/%.o: $(CORE)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Core .c (vorbis_encoder.c)
$(BUILD)/core/%.o: $(CORE)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# WDL .cpp
$(BUILD)/wdl/%.o: $(WDL)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# jnetlib .cpp
$(BUILD)/jnet/%.o: $(JNET)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Harness
$(BUILD)/harness/%.o: $(HARNESS)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Scenarios
$(BUILD)/scenarios/%.o: $(SCEN)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Catch2 amalgamated — disable its built-in main(); we provide one in harness/main_test.cpp.
$(CATCH_OBJ): $(VENDOR)/catch_amalgamated.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXSTD) $(OPT) -DCATCH_AMALGAMATED_CUSTOM_MAIN=1 -I$(VENDOR) -c $< -o $@

clean:
	rm -rf $(BUILD) video_sync_tests

# Boots ninjamzap-server-test container, exports NJ_TEST_PORT, runs binary.
test: video_sync_tests
	@OUT=$$(./docker/run-server.sh) && \
	  NAME=$$(echo "$$OUT" | awk '{print $$1}') && \
	  PORT=$$(echo "$$OUT" | awk '{print $$2}') && \
	  echo "[test] server container=$$NAME port=$$PORT" && \
	  trap "docker rm -f $$NAME >/dev/null 2>&1" EXIT && \
	  NJ_TEST_HOST=127.0.0.1 NJ_TEST_PORT=$$PORT ./video_sync_tests

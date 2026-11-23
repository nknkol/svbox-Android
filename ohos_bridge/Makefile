# SPDX-License-Identifier: MIT
CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -DOHOS_BRIDGE_HEADLESS=1
INCLUDES := -Iinclude -I../gpu/ohos_host
LDFLAGS  := -lpthread -ldl

BRIDGE_SRCS := \
	src/sched_qos.cpp \
	src/input_bridge.cpp \
	src/audio_bridge.cpp \
	src/usb_bridge.cpp \
	src/surface_bridge.cpp \
	src/gpu_bridge.cpp \
	src/container_runner.cpp

BRIDGE_OBJS := $(patsubst src/%.cpp,build/%.o,$(BRIDGE_SRCS))

.PHONY: all clean

all: build/svbox-linux

build:
	mkdir -p build

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

build/libohos_bridge.a: $(BRIDGE_OBJS)
	ar rcs $@ $^

build/svbox-linux: tools/svbox_linux.cpp build/libohos_bridge.a
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< build/libohos_bridge.a $(LDFLAGS) -o $@

clean:
	rm -rf build

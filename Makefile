# Copyright (c) 2025 Johannes Overmann
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

TARGET = scanbadblocks
#CXX = /opt/homebrew/opt/llvm/bin/clang++

CPPFLAGS ?= -pedantic

WARNING_FLAGS ?= -Weverything \
	-Wno-c++98-compat \
	-Wno-c++98-compat-pedantic \
	-Wno-padded \
	-Wno-shorten-64-to-32 \
	-Wno-missing-prototypes \
	-Wno-sign-conversion \
	-Wno-implicit-int-conversion \
	-Wno-poison-system-directories \
	-fcomment-block-commands=n \
	-Wno-string-conversion \
	-Wno-covered-switch-default \
	-Wno-exit-time-destructors \
	-Wno-global-constructors \
	-Wno-implicit-int-float-conversion \
	-Wno-unsafe-buffer-usage \
	-Wno-unsafe-buffer-usage-in-libc-call \
	-Wno-extra-semi-stmt
CXXFLAGS ?= -O3 -Wall

CXXSTD ?= -std=c++23

BUILDDIR=build
SOURCES = $(wildcard src/*.cpp)
OBJECTS = $(SOURCES:%.cpp=$(BUILDDIR)/%.o)
DEPENDS := $(SOURCES:%.cpp=$(BUILDDIR)/%.d)

default: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $^ -o $@

build/%.o: %.cpp build/%.d
	$(CXX) $(CXXSTD) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/%.d: %.cpp Makefile
	@mkdir -p $(@D)
	$(CXX) $(CXXSTD) $(CPPFLAGS) -MM -MQ $@ $< -o $@

clean:
	rm -rf build $(TARGET) unit_test
	find . -name '*~' -delete

unit_test: CPPFLAGS += -D ENABLE_UNIT_TEST
unit_test: $(OBJECTS)
	$(CXX) $^ -o $@
	./unit_test

test: unit_test

format:
	clang-format -i --style=file src/*.hpp src/*.cpp

tidy: CXXFLAGS += -MJ $@.cdb
tidy: $(TARGET)
	echo "[" > $(BUILDDIR)/compile_commands.json
	cat $(BUILDDIR)/src/*.cdb >> $(BUILDDIR)/compile_commands.json
	echo "]" >> $(BUILDDIR)/compile_commands.json
	clang-tidy -p $(BUILDDIR) --config-file .clang-tidy src/*.cpp src/*.hpp

warnings:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="-O3 $(WARNING_FLAGS)" $(TARGET)

.PHONY: clean default unit_test test format warnings

ifeq ($(findstring $(MAKECMDGOALS),clean),)
-include $(DEPENDS)
endif

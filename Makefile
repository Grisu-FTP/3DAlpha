# Thin wrapper over CMake so the documented commands stay short.
#
#   make                      build a1.1.2 for 3DS
#   make VERSION=a1.2.6       build another version
#   make all-versions         build every manifest in versions/
#   make cia                  also package a CIA (needs makerom on PATH)
#   make host                 build for Linux (unit tests, fast iteration)
#   make index                regenerate docs/code-map.md and docs/doc-index.md
#   make run                  send the 3dsx to a console over Wi-Fi (3dslink)
#   make clean

VERSION   ?= a1.1.2
BUILD_TYPE ?= Release

BUILD_DIR  := build/$(VERSION)
HOST_DIR   := build-host
VERSIONS   := $(basename $(notdir $(wildcard versions/*.json)))

# devkitPro's own install path. Defaulted rather than required so the build
# works in a shell that has not sourced the devkit environment -- CI and editor
# integrations routinely have not.
DEVKITPRO  ?= /opt/devkitpro
DEVKITARM  ?= $(DEVKITPRO)/devkitARM
# The arm-none-eabi-cmake wrapper reads these from the environment to locate its
# toolchain file, so exporting them is what makes the default above take effect.
export DEVKITPRO
export DEVKITARM
CMAKE_3DS  := $(DEVKITPRO)/portlibs/3ds/bin/arm-none-eabi-cmake

.PHONY: all cia host test all-versions run index clean

all: $(BUILD_DIR)/CMakeCache.txt
	@cmake --build $(BUILD_DIR)

cia: $(BUILD_DIR)/CMakeCache.txt
	@cmake --build $(BUILD_DIR) --target cia

$(BUILD_DIR)/CMakeCache.txt:
	@$(CMAKE_3DS) -S . -B $(BUILD_DIR) -DMCVER=$(VERSION) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

all-versions:
	@for v in $(VERSIONS); do \
		echo "==> $$v"; \
		$(MAKE) --no-print-directory VERSION=$$v || exit 1; \
	done

host: $(HOST_DIR)/CMakeCache.txt
	@cmake --build $(HOST_DIR)

$(HOST_DIR)/CMakeCache.txt:
	@cmake -S . -B $(HOST_DIR) -DMCVER=$(VERSION) -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=ON

test: host
	@ctest --test-dir $(HOST_DIR) --output-on-failure

# The two generated indexes. Cheap enough to run on a whim; `--check` is the
# form for CI, which fails rather than rewriting.
index:
	@python3 tools/gen_index.py

run: all
	@3dslink $(BUILD_DIR)/3DAlpha-$(VERSION).3dsx

clean:
	@rm -rf build build-host

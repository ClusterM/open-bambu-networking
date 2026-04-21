# Thin wrapper around CMake so the usual './configure && make && make install'
# dance works. Every target here just shells out to cmake -- there is nothing
# else going on.
#
# If you run 'make' before ./configure, we auto-configure with default flags.
# Customise the build by running ./configure yourself with the options
# printed by './configure --help'.

-include config.mk

BUILD_DIR ?= build

NPROC := $(shell command -v nproc >/dev/null 2>&1 && nproc || echo 2)

# Everything forwards to cmake; no file-based rules.
.PHONY: all build configure reconfigure install uninstall test clean distclean help

all: build

# Auto-run ./configure if the build tree is missing. Users that want a
# specific prefix / flags should run ./configure themselves.
$(BUILD_DIR)/CMakeCache.txt:
	@echo "Makefile: no build tree at $(BUILD_DIR), running ./configure with defaults"
	@./configure

configure reconfigure:
	@./configure $(CONFIGURE_ARGS)

build: $(BUILD_DIR)/CMakeCache.txt
	@cmake --build $(BUILD_DIR) -j$(NPROC)

install: build
	@cmake --install $(BUILD_DIR)

uninstall:
	@if [ -f $(BUILD_DIR)/install_manifest.txt ]; then \
	    echo "Makefile: removing files listed in $(BUILD_DIR)/install_manifest.txt"; \
	    xargs -r rm -fv < $(BUILD_DIR)/install_manifest.txt; \
	else \
	    echo "Makefile: no install manifest at $(BUILD_DIR)/install_manifest.txt -- run 'make install' first"; \
	fi

test: build
	@ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	@if [ -d $(BUILD_DIR) ]; then \
	    cmake --build $(BUILD_DIR) --target clean; \
	fi

distclean:
	@rm -rf $(BUILD_DIR) config.mk

help:
	@echo 'Targets:'
	@echo '  make                 Configure (if needed) and build the plugin'
	@echo '  make install         Install into the configured prefix and patch'
	@echo '                       BambuStudio.conf (when installing to the'
	@echo '                       default BambuStudio dir)'
	@echo '  make test            Run the smoke tests via ctest'
	@echo '  make clean           Remove compiled artefacts, keep the cache'
	@echo '  make distclean       Remove the build directory entirely'
	@echo "  make uninstall       Remove every file 'make install' put down"
	@echo '  make reconfigure     Re-run ./configure (forward flags via CONFIGURE_ARGS)'
	@echo ''
	@echo 'Customise the build:'
	@echo '  ./configure --help   Full list of configure options'
	@echo '  make reconfigure CONFIGURE_ARGS="--prefix=/opt/my-bambu --build-type=Debug"'

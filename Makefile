# Astravox — VCV Rack Plugin
# Vocal processing suite: Vocoder + Expander
#
# RACK_DIR points to the extracted Rack SDK — set via environment or command line:
#   make RACK_DIR=/path/to/Rack-SDK
RACK_DIR ?= ../Rack-SDK

# Detect platform early so we can apply platform-specific linker flags
# before including plugin.mk (which also includes arch.mk — safe to include twice).
include $(RACK_DIR)/arch.mk

# FLAGS will be passed to both the C and C++ compiler
FLAGS +=
CFLAGS +=
CXXFLAGS +=

# Careful about linking to shared libraries, since you can't assume much about
# the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
LDFLAGS +=

# plugin.mk statically links libstdc++ on Windows but not libgcc, leaving the
# plugin DLL dependent on libgcc_s_seh-1.dll at a version tied to the compiler.
# VCV Rack bundles its own copy of that DLL (matched to the rack-plugin-toolchain
# MinGW), so a plugin built with any other MinGW version crashes on load.
# Statically linking libgcc makes the plugin self-contained regardless of toolchain.
ifdef ARCH_WIN
LDFLAGS += -static-libgcc
endif

# Add .cpp files to the build
SOURCES += $(wildcard src/*.cpp)

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk

# Custom install target for local development
RACK_USER_DIR := $(HOME)/Library/Application Support/Rack2
INSTALL_DIR := $(RACK_USER_DIR)/plugins-mac-arm64/Astravox
localinstall: all
	mkdir -p "$(INSTALL_DIR)"
	cp plugin.dylib "$(INSTALL_DIR)/"
	cp plugin.json "$(INSTALL_DIR)/"
	cp -r res "$(INSTALL_DIR)/"
	@echo "Installed Astravox to $(INSTALL_DIR)"

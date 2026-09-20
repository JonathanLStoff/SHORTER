# ------------------------------------------------------------------
# WireViz PNG Exporter
# ------------------------------------------------------------------
# Installs WireViz + Graphviz (macOS / Windows)
# Exports the newest version folder in ./wiring to PNG
# ------------------------------------------------------------------

SHELL := /bin/bash
WIRING_DIR := wiring
EXPORT_DIR := $(WIRING_DIR)/exported

# Detect OS
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    OS := mac
else
    OS := windows
endif

# Find the latest version folder (e.g., v0.2 > v0.1)
LATEST_VERSION := $(shell \
    find $(WIRING_DIR) -maxdepth 1 -type d -name 'v*' -print 2>/dev/null \
    | sort -V \
    | tail -n 1 \
)

# All YAML files (both .yml and .yaml) inside the latest version folder
LATEST_YAMLS := $(shell find $(LATEST_VERSION) -maxdepth 1 -type f \( -name '*.yml' -o -name '*.yaml' \) 2>/dev/null)

.PHONY: all install export clean help

## Default target: install dependencies and export
all: install export

## Install WireViz and Graphviz
install:
ifeq ($(OS),mac)
	@echo "🍎 Installing on macOS..."
	@which brew >/dev/null 2>&1 || (echo "Homebrew not found. Please install Homebrew first." && exit 1)
	brew install graphviz
	pip3 install wireviz
else
	@echo "🪟 Installing on Windows..."
	@which choco >/dev/null 2>&1 || (echo "Chocolatey not found. Please install Chocolatey first." && exit 1)
	choco install graphviz -y
	pip install wireviz
endif
	@echo "✅ WireViz and Graphviz installed."

## Export every YAML in the latest version folder to PNG
export:
ifeq ($(strip $(LATEST_VERSION)),)
	@echo "❌ No version folder found in $(WIRING_DIR)."
	@exit 1
endif
ifeq ($(strip $(LATEST_YAMLS)),)
	@echo "❌ No .yml or .yaml files found in $(LATEST_VERSION)."
	@exit 1
endif
	@echo "📂 Using latest version: $(LATEST_VERSION)"
	@mkdir -p $(EXPORT_DIR)
	@for f in $(LATEST_YAMLS); do \
		echo "  → Exporting $$f ..."; \
		wireviz -f p -o $(EXPORT_DIR) "$$f"; \
	done
	@echo "✅ PNG files written to $(EXPORT_DIR)/"

## Remove generated PNG files
clean:
	rm -rf $(EXPORT_DIR)

## Show this help
help:
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-15s\033[0m %s\n", $$1, $$2}'
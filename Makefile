# macOS Apple Silicon build wrapper. The script builds for the current
# architecture; on Apple Silicon this is arm64.
MACOS_DEPLOYMENT_TARGET ?= 15.4

.PHONY: build bootstrap deps

# Incremental application build: reuse the configured CMake tree and staged
# dependencies. Recompiles only changed targets and their dependents.
build:
	./build_release_macos.sh -s -b -t $(MACOS_DEPLOYMENT_TARGET)

# First-time setup (or after deliberately rebuilding dependencies).
bootstrap: deps
	./build_release_macos.sh -s -t $(MACOS_DEPLOYMENT_TARGET)

deps:
	./build_release_macos.sh -d -t $(MACOS_DEPLOYMENT_TARGET)
